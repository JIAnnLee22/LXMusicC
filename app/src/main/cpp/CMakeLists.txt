cmake_minimum_required(VERSION 3.22)

project(lxmusicc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# NativeActivity 胶水代码随 NDK 分发。
add_library(native_app_glue STATIC
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
        ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(native_app_glue android log)

add_library(lxmusicc SHARED main.c render.c ui.c platform.c source.c source_parse.c source_bridge.c)

target_include_directories(lxmusicc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lxmusicc PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Java 侧通过 dlsym 查找入口，防止 --gc-sections 把入口优化掉。
target_link_options(lxmusicc PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")

target_link_libraries(lxmusicc
        EGL
        GLESv3
        android
        log
        m
        dl
        native_app_glue)
