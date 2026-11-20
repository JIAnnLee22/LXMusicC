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

# 原样 vendor 的上游引擎独立编译；不链接 quickjs-libc，不提供文件/进程/模块加载。
set(QJS vendor/quickjs)
add_library(quickjs STATIC ${QJS}/quickjs.c ${QJS}/cutils.c ${QJS}/libregexp.c
        ${QJS}/libunicode.c ${QJS}/dtoa.c)
set_target_properties(quickjs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2025-09-13")
target_include_directories(quickjs PUBLIC ${QJS})
target_link_libraries(quickjs m)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS lx_compat.js)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lx_compat.js" LX_COMPAT_C)
string(REPLACE "\\" "\\\\" LX_COMPAT_C "${LX_COMPAT_C}")
string(REPLACE "\"" "\\\"" LX_COMPAT_C "${LX_COMPAT_C}")
string(REPLACE "\n" "\\n\"\n\"" LX_COMPAT_C "${LX_COMPAT_C}")
configure_file(lx_compat.h.in lx_compat.h @ONLY)

add_library(lxmusicc SHARED main.c render.c ui.c platform.c source.c source_parse.c
        source_bridge.c source_runtime.c search.c playback.c playback_bridge.c)

target_include_directories(lxmusicc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
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
        native_app_glue
        quickjs)
