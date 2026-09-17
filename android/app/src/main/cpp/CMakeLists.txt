cmake_minimum_required(VERSION 3.22.1)
project(classic_player_native)

# TinySoundFont is vendored in this directory and compiled into the APK. This
# keeps SF2 loading available offline on Android 6+ without relying on a server.
set(MSFA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../third_party/msfa")
add_library(classic_player_native SHARED
    ClassicPlayerNative.cpp
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
target_link_libraries(classic_player_native android log m)
