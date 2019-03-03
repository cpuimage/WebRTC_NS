cmake_minimum_required(VERSION 2.8)
project(ns LANGUAGES C)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
SET(CMAKE_BUILD_TYPE "Release")
add_executable(ns main.c
        noise_suppression.c)
target_link_libraries(ns m)