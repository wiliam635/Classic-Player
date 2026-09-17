cmake_minimum_required(VERSION 3.22.1)
project(classic_player_native)

# Keep the JNI boundary small. The full SoundFont dependency is fetched by the
# Android workflow, while the Java side remains compatible with API 23.
add_library(classic_player_native SHARED ClassicPlayerNative.cpp)
target_compile_features(classic_player_native PRIVATE cxx_std_17)
target_link_libraries(classic_player_native android log)
