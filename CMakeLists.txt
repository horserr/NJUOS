cmake_minimum_required(VERSION 3.22)
project(L1 C)

set(CMAKE_C_STANDARD 11)


add_executable(L1 main.c
               src/pmm.c
               include/common.h
               include/external/kernel.h
               include/external/am.h
               include/external/klib-macros.h
               include/external/klib.h)
add_definitions(-Dclion)
