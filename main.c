#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
//采用https://github.com/mackron/dr_libs/blob/master/dr_wav.h 解码
#define DR_WAV_IMPLEMENTATION

#include "dr_wav.h"
#include "noise_suppression.h"

#ifndef nullptr
#define nullptr 0
#endif

//写wav文件
void wavWrite_int16(char *filename, int16_t *buffer, size_t sampleRate, size_t totalSampleCount) {
    drwav_data_format format = {};
    format.container = drwav_container_riff;     // <-- drwav_container_riff = normal WAV files, drwav_container_w64 = Sony Wave64.
    format.format = DR_WAVE_FORMAT_PCM;          // <-- Any of the DR_WAVE_FORMAT_* codes.
    format.channels = 1;
    format.sampleRate = (drwav_uint32) sampleRate;
    format.bitsPerSample = 16;
    drwav *pWav = drwav_open_file_write(filename, &format);
    if (pWav) {
        drwav_uint64 samplesWritten = drwav_write(pWav, totalSampleCount, buffer);
        drwav_uninit(pWav);
        if (samplesWritten != totalSampleCount) {
            fprintf(stderr, "ERROR\n");
            exit(1);
        }
    }
}

//读取wav文件
int16_t *wavRead_int16(char *filename, uint32_t *sampleRate, uint64_t *totalSampleCount) {
    unsigned int channels;
    int16_t *buffer = drwav_open_and_read_file_s16(filename, &channels, sampleRate, totalSampleCount);
    if (buffer == nullptr) {
        printf("读取wav文件失败.");
    }
    //仅仅处理单通道音频
    if (channels != 1) {
        drwav_free(buffer);
        buffer = nullptr;
        *sampleRate = 0;
        *totalSampleCount = 0;
    }
    return buffer;
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

static float S16ToFloat_C(int16_t v) {
    if (v > 0) {
        return ((float) v) / (float) INT16_MAX;
    }

    return (((float) v) / ((float) -INT16_MIN));
}

void S16ToFloat(const int16_t *src, size_t size, float *dest) {
    size_t i;
    for (i = 0; i < size; ++i)
        dest[i] = S16ToFloat_C(src[i]);
}

static int16_t FloatToS16_C(float v) {
    static const float kMaxRound = (float) INT16_MAX - 0.5f;
    static const float kMinRound = (float) INT16_MIN + 0.5f;
    if (v > 0) {
        v *= kMaxRound;
        return v >= kMaxRound ? INT16_MAX : (int16_t) (v + 0.5f);
    }

    v *= -kMinRound;
    return v <= kMinRound ? INT16_MIN : (int16_t) (v - 0.5f);
}

void FloatToS16(const float *src, size_t size, int16_t *dest) {
    size_t i;
    for (i = 0; i < size; ++i)
        dest[i] = FloatToS16_C(src[i]);
}

int nsProcess(int16_t *buffer, size_t sampleRate, int samplesCount, enum nsLevel level) {
    if (buffer == nullptr) return -1;
    if (samplesCount == 0) return -1;
    size_t samples = WEBRTC_SPL_MIN(160, sampleRate / 100);
    if (samples == 0) return -1;
    const int maxSamples = 320;
    int num_bands = 1;
    int16_t *input = buffer;
    size_t nTotal = (samplesCount / samples);

    NsHandle *nsHandle = WebRtcNs_Create();

    int status = WebRtcNs_Init(nsHandle, sampleRate);
    if (status != 0) {
        printf("WebRtcNs_Init fail\n");
        return -1;
    }
    status = WebRtcNs_set_policy(nsHandle, level);
    if (status != 0) {
        printf("WebRtcNs_set_policy fail\n");
        return -1;
    }
    for (int i = 0; i < nTotal; i++) {
        float inf_buffer[maxSamples];
        float outf_buffer[maxSamples];
        S16ToFloat(input, samples, inf_buffer);
        float *nsIn[1] = {inf_buffer};   //ns input[band][data]
        float *nsOut[1] = {outf_buffer};  //ns output[band][data]
        WebRtcNs_Analyze(nsHandle, nsIn[0]);
        WebRtcNs_Process(nsHandle, (const float *const *) nsIn, num_bands, nsOut);
        FloatToS16(outf_buffer, samples, input);
        input += samples;
    }
    WebRtcNs_Free(nsHandle);

    return 1;
}

void noise_suppression(char *in_file, char *out_file) {
    //音频采样率
    uint32_t sampleRate = 0;
    //总音频采样数
    uint64_t inSampleCount = 0;
    int16_t *inBuffer = wavRead_int16(in_file, &sampleRate, &inSampleCount);

    //如果加载成功
    if (inBuffer != nullptr) {
        nsProcess(inBuffer, sampleRate, inSampleCount, kVeryHigh);
        wavWrite_int16(out_file, inBuffer, sampleRate, inSampleCount);

        free(inBuffer);
    }
}

int main(int argc, char *argv[]) {
    printf("WebRtc Noise Suppression\n");
    printf("博客:http://cpuimage.cnblogs.com/\n");
    printf("音频噪声抑制\n");
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

    printf("按任意键退出程序 \n");
    getchar();
    return 0;
}
