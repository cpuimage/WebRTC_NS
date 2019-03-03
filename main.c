#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define DR_MP3_IMPLEMENTATION

#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION

#include "dr_wav.h"
#include "timing.h"

#include "noise_suppression.h"

#ifndef nullptr
#define nullptr 0
#endif

#ifndef MIN
#define MIN(A, B)        ((A) < (B) ? (A) : (B))
#endif


void wavWrite_int16(char *filename, int16_t *buffer, int sampleRate, uint64_t totalSampleCount, uint32_t channels) {
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = channels;
    format.sampleRate = (drwav_uint32) sampleRate;
    format.bitsPerSample = 16;

    drwav *pWav = drwav_open_file_write(filename, &format);
    if (pWav) {
        drwav_uint64 samplesWritten = drwav_write(pWav, totalSampleCount, buffer);
        drwav_uninit(pWav);
        if (samplesWritten != totalSampleCount) {
            fprintf(stderr, "write file [%s] error.\n", filename);
            exit(1);
        }
    }
}

int16_t *wavRead_int16(const char *filename, uint32_t *sampleRate, uint64_t *sampleCount, uint32_t *channels) {
    drwav_uint64 totalSampleCount = 0;
    int16_t *input = drwav_open_file_and_read_pcm_frames_s16(filename, channels, sampleRate, &totalSampleCount);
    if (input == NULL) {
        drmp3_config pConfig;
        float *mp3_buf = drmp3_open_file_and_read_f32(filename, &pConfig, &totalSampleCount);
        if (mp3_buf != NULL) {
            *channels = pConfig.outputChannels;
            *sampleRate = pConfig.outputSampleRate;
        }
        input = (int16_t *) mp3_buf;
        for (int32_t i = 0; i < *sampleCount; ++i) {
            input[i] = (int16_t) drwav_clamp((mp3_buf[i] * 32768.0f), -32768, 32767);
        }
    }
    if (input == NULL) {
        fprintf(stderr, "read file [%s] error.\n", filename);
        exit(1);
    }
    *sampleCount = totalSampleCount * (*channels);

    return input;
}


//分割路径函数
void splitpath(const char *path, char *drv, char *dir, char *name, char *ext) {
    const char *end;
    const char *p;
    const char *s;
    if (path[0] && path[1] == ':') {
        if (drv) {
            *drv++ = *path++;
            *drv++ = *path++;
            *drv = '\0';
        }
    } else if (drv)
        *drv = '\0';
    for (end = path; *end && *end != ':';)
        end++;
    for (p = end; p > path && *--p != '\\' && *p != '/';)
        if (*p == '.') {
            end = p;
            break;
        }
    if (ext)
        for (s = end; (*ext = *s++);)
            ext++;
    for (p = end; p > path;)
        if (*--p == '\\' || *p == '/') {
            p++;
            break;
        }
    if (name) {
        for (s = p; s < end;)
            *name++ = *s++;
        *name = '\0';
    }
    if (dir) {
        for (s = path; s < p;)
            *dir++ = *s++;
        *dir = '\0';
    }
}

enum nsLevel {
    kLow,
    kModerate,
    kHigh,
    kVeryHigh
};


int nsProcess(int16_t *buffer, uint32_t sampleRate, uint64_t samplesCount, uint32_t channels, enum nsLevel level) {
    if (buffer == nullptr) return -1;
    if (samplesCount == 0) return -1;
    size_t samples = MIN(160, sampleRate / 100);
    if (samples == 0) return -1;
    uint32_t num_bands = 1;
    int16_t *input = buffer;
    size_t frames = (samplesCount / (samples * channels));
    int16_t *frameBuffer = (int16_t *) malloc(sizeof(*frameBuffer) * channels * samples);
    NsHandle **NsHandles = (NsHandle **) malloc(channels * sizeof(NsHandle *));
    if (NsHandles == NULL || frameBuffer == NULL) {
        if (NsHandles)
            free(NsHandles);
        if (frameBuffer)
            free(frameBuffer);
        fprintf(stderr, "malloc error.\n");
        return -1;
    }
    for (int i = 0; i < channels; i++) {
        NsHandles[i] = WebRtcNs_Create();
        if (NsHandles[i] != NULL) {
            int status = WebRtcNs_Init(NsHandles[i], sampleRate);
            if (status != 0) {
                fprintf(stderr, "WebRtcNs_Init fail\n");
                WebRtcNs_Free(NsHandles[i]);
                NsHandles[i] = NULL;
            } else {
                status = WebRtcNs_set_policy(NsHandles[i], level);
                if (status != 0) {
                    fprintf(stderr, "WebRtcNs_set_policy fail\n");
                    WebRtcNs_Free(NsHandles[i]);
                    NsHandles[i] = NULL;
                }
            }
        }
        if (NsHandles[i] == NULL) {
            for (int x = 0; x < i; x++) {
                if (NsHandles[x]) {
                    WebRtcNs_Free(NsHandles[x]);
                }
            }
            free(NsHandles);
            free(frameBuffer);
            return -1;
        }
    }
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            for (int k = 0; k < samples; k++)
                frameBuffer[k] = input[k * channels + c];

            int16_t *nsIn[1] = {frameBuffer};   //ns input[band][data]
            int16_t *nsOut[1] = {frameBuffer};  //ns output[band][data]
            WebRtcNs_Analyze(NsHandles[c], nsIn[0]);
            WebRtcNs_Process(NsHandles[c], (const int16_t *const *) nsIn, num_bands, nsOut);
            for (int k = 0; k < samples; k++)
                input[k * channels + c] = frameBuffer[k];
        }
        input += samples * channels;
    }

    for (int i = 0; i < channels; i++) {
        if (NsHandles[i]) {
            WebRtcNs_Free(NsHandles[i]);
        }
    }
    free(NsHandles);
    free(frameBuffer);
    return 1;
}

void noise_suppression(char *in_file, char *out_file) {
    //音频采样率
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    //总音频采样数
    uint64_t inSampleCount = 0;
    int16_t *inBuffer = wavRead_int16(in_file, &sampleRate, &inSampleCount, &channels);

    //如果加载成功
    if (inBuffer != nullptr) {
        double startTime = now();
        nsProcess(inBuffer, sampleRate, inSampleCount, channels, kModerate);
        double time_interval = calcElapsed(startTime, now());
        printf("time interval: %d ms\n ", (int) (time_interval * 1000));


        wavWrite_int16(out_file, inBuffer, sampleRate, inSampleCount, channels);
        free(inBuffer);
    }
}

int main(int argc, char *argv[]) {
    printf("WebRtc Noise Suppression\n");
    printf("blog:http://cpuimage.cnblogs.com/\n");
    if (argc < 2)
        return -1;
    char *in_file = argv[1];
    char drive[3];
    char dir[256];
    char fname[256];
    char ext[256];
    char out_file[1024];
    splitpath(in_file, drive, dir, fname, ext);
    sprintf(out_file, "%s%s%s_out%s", drive, dir, fname, ext);
    noise_suppression(in_file, out_file);

    printf("press any key to exit. \n");
    getchar();
    return 0;
}
