cmake_minimum_required(VERSION 3.9)
project(ns)

set(CMAKE_CXX_STANDARD 11)
add_executable(ns main.c
        noise_suppression.c
        )