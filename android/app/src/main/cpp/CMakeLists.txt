cmake_minimum_required(VERSION 3.22.1)
project(classic_player_native)

# Pin the official Oboe source and verify its archive. Oboe negotiates the
# platform's fast output path and converts locally when a USB DAC uses 44.1 kHz.
include(FetchContent)
FetchContent_Declare(oboe
    URL https://codeload.github.com/google/oboe/tar.gz/refs/tags/1.9.3
    URL_HASH SHA256=9d2486b74bd396d9d9112625077d5eb656fd6942392dc25ebf222b184ff4eb61)
set(OBOE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OBOE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(oboe)

# TinySoundFont is vendored in this directory and compiled into the APK. This
# keeps SF2 loading available offline on Android 6+ without relying on a server.
set(MSFA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../third_party/msfa")
add_library(classic_player_native SHARED
    ClassicPlayerNative.cpp
    NativeAudioOutput.cpp
    ${MSFA_DIR}/dx7note.cc
    ${MSFA_DIR}/env.cc
    ${MSFA_DIR}/exp2.cc
    ${MSFA_DIR}/fm_core.cc
    ${MSFA_DIR}/fm_op_kernel.cc
    ${MSFA_DIR}/freqlut.cc
    ${MSFA_DIR}/pitchenv.cc
    ${MSFA_DIR}/porta.cpp
    ${MSFA_DIR}/sin.cc
    ${MSFA_DIR}/tuning.cc)
target_include_directories(classic_player_native PRIVATE ${MSFA_DIR})
target_compile_features(classic_player_native PRIVATE cxx_std_17)
# The distributed test APK is debug, but its real-time DSP must not run at
# -O0. Preserve debug symbols and floating-point semantics (no fast-math).
target_compile_options(classic_player_native PRIVATE -O3)
target_link_libraries(classic_player_native oboe android log m)
