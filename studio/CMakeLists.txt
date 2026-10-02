cmake_minimum_required(VERSION 3.22)

project(ClassicPlayerStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CLASSIC_PLAYER_STUDIO_FETCH_JUCE "Fetch JUCE when JUCE_DIR is not supplied" ON)

if(DEFINED JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" JUCE)
elseif(CLASSIC_PLAYER_STUDIO_FETCH_JUCE)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
else()
    message(FATAL_ERROR "Set JUCE_DIR or enable CLASSIC_PLAYER_STUDIO_FETCH_JUCE")
endif()

juce_add_gui_app(ClassicPlayerStudio
    PRODUCT_NAME "Classic Player Studio"
    COMPANY_NAME "Classic Keys"
    BUNDLE_ID "com.classickeys.classicplayerstudio"
    VERSION "${PROJECT_VERSION}")

target_sources(ClassicPlayerStudio PRIVATE
    src/Main.cpp
    src/AudioEngine.cpp
    src/AudioEngine.h
    src/AudioRecorder.cpp
    src/AudioRecorder.h
    src/InstrumentHost.cpp
    src/InstrumentHost.h
    src/MixerState.cpp
    src/MixerState.h
    src/Session.cpp
    src/Session.h
    src/StudioApplication.cpp
    src/StudioApplication.h
    src/TimelineView.cpp
    src/TimelineView.h
    src/TransportState.cpp
    src/TransportState.h)

target_compile_definitions(ClassicPlayerStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
    CLASSIC_PLAYER_STUDIO_VERSION="${PROJECT_VERSION}")

target_link_libraries(ClassicPlayerStudio PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(ClassicPlayerStudioSessionTest PRODUCT_NAME "Classic Player Studio Session Test")
    target_sources(ClassicPlayerStudioSessionTest PRIVATE
        tests/SessionTest.cpp
        src/MixerState.cpp
        src/MixerState.h
        src/Session.cpp
        src/Session.h
        src/TransportState.cpp
        src/TransportState.h)
    target_link_libraries(ClassicPlayerStudioSessionTest PRIVATE
        juce::juce_core
        juce::juce_data_structures
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_test(NAME ClassicPlayerStudioSessionTest COMMAND ClassicPlayerStudioSessionTest)
endif()
