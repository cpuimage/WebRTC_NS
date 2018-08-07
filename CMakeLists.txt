cmake_minimum_required(VERSION 2.8)
project(ns) 
add_executable(ns main.c
        noise_suppression.c
        )
target_link_libraries(ns -lm)