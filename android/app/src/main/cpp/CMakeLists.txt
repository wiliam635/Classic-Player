cmake_minimum_required(VERSION 3.22.1)
project(classic_player_native)

# TinySoundFont is vendored in this directory and compiled into the APK. This
# keeps SF2 loading available offline on Android 6+ without relying on a server.
add_library(classic_player_native SHARED ClassicPlayerNative.cpp)
target_compile_features(classic_player_native PRIVATE cxx_std_17)
target_link_libraries(classic_player_native android log m)
