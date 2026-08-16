cmake_minimum_required(VERSION 3.22)

project(ClassicPlayer VERSION 1.6.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CLASSIC_PLAYER_FETCH_JUCE "Download JUCE when JUCE_DIR is not supplied" ON)
option(CLASSIC_PLAYER_STANDALONE_ONLY "Build only the standalone application" OFF)
option(CLASSIC_PLAYER_BUILD_TESTS "Build the native SF2 smoke test" OFF)
option(CLASSIC_PLAYER_BUILD_WEBUI_APP "Build the legacy WebUI standalone application instead of the native editor" OFF)

if(DEFINED JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" JUCE)
elseif(CLASSIC_PLAYER_FETCH_JUCE)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
else()
    message(FATAL_ERROR "Set JUCE_DIR or enable CLASSIC_PLAYER_FETCH_JUCE")
endif()

find_package(FluidSynth CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

if(TARGET FluidSynth::libfluidsynth)
    set(CLASSIC_PLAYER_FLUID_TARGET FluidSynth::libfluidsynth)
elseif(TARGET fluidsynth)
    set(CLASSIC_PLAYER_FLUID_TARGET fluidsynth)
else()
    message(FATAL_ERROR "FluidSynth was found, but its CMake target is unknown")
endif()

# The native editor in PluginEditor.cpp is the approved Classic Player interface.
# Only the explicit legacy option builds the separate WebUI executable.
if(CLASSIC_PLAYER_BUILD_WEBUI_APP)
    set(CLASSIC_PLAYER_FORMATS VST3)
else()
    set(CLASSIC_PLAYER_FORMATS Standalone VST3)
    if(APPLE)
        list(APPEND CLASSIC_PLAYER_FORMATS AU)
    endif()
endif()

juce_add_plugin(ClassicPlayer
    COMPANY_NAME "Classic Keys"
    BUNDLE_ID "com.classickeys.classicplayer"
    PLUGIN_MANUFACTURER_CODE ClKy
    PLUGIN_CODE ClPl
    FORMATS ${CLASSIC_PLAYER_FORMATS}
    PRODUCT_NAME "Classic Player"
    DESCRIPTION "Classic Player SF2 workstation"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    ICON_BIG "Resources/classic-player-app-icon.png"
    ICON_SMALL "Resources/classic-player-app-icon.png"
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth)

if(APPLE AND TARGET ClassicPlayer_Standalone)
    set_target_properties(ClassicPlayer_Standalone PROPERTIES
        JUCE_BUNDLE_ID "com.classickeys.classicplayer.app")
endif()
if(APPLE AND TARGET ClassicPlayer_VST3)
    set_target_properties(ClassicPlayer_VST3 PROPERTIES
        JUCE_BUNDLE_ID "com.classickeys.classicplayer.vst3")
endif()
if(APPLE AND TARGET ClassicPlayer_AU)
    set_target_properties(ClassicPlayer_AU PROPERTIES
        JUCE_BUNDLE_ID "com.classickeys.classicplayer.au")
endif()

juce_add_binary_data(ClassicPlayerAssets
    HEADER_NAME ClassicPlayerAssets.h
    NAMESPACE ClassicPlayerAssets
    SOURCES
        Resources/classic-player-license-public.pem
        Resources/classic-player-app-icon.png
        Resources/WebUI/classic-keys-logo.jpeg
        Resources/WebUI/willam-silva-logo.png)

target_sources(ClassicPlayer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/Sf2Engine.cpp
    Source/Sf2Engine.h
    Source/Dx7Engine.cpp
    Source/Dx7Engine.h
    Source/ExternalInstrumentHost.cpp
    Source/ExternalInstrumentHost.h
    Source/LicenseVerifier.cpp
    Source/LicenseVerifier.h
    Source/ChordDetector.cpp
    Source/ChordDetector.h)

target_compile_definitions(ClassicPlayer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    CLASSIC_PLAYER_VERSION="${PROJECT_VERSION}")

if(APPLE)
    target_compile_definitions(ClassicPlayer PUBLIC JUCE_PLUGINHOST_AU=1)
endif()

if(WIN32)
    # A local SDK copy may be supplied for developer builds.  It is deliberately
    # not downloaded or redistributed automatically because ASIO has dual
    # licensing obligations (Steinberg proprietary or GPLv3).
    if(NOT DEFINED ASIO_SDK_DIR)
        set(_bundled_asio "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk")
        if(EXISTS "${_bundled_asio}/common/asio.h")
            set(ASIO_SDK_DIR "${_bundled_asio}")
        endif()
    endif()
    if(DEFINED ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/asio.h")
        target_compile_definitions(ClassicPlayer PUBLIC JUCE_ASIO=1)
        target_include_directories(ClassicPlayer PRIVATE "${ASIO_SDK_DIR}/common")
        message(STATUS "Classic Player: ASIO habilitado (ASIO_SDK_DIR=${ASIO_SDK_DIR})")
    else()
        message(WARNING "Classic Player: ASIO desabilitado; forneça ASIO_SDK_DIR apontando para o Steinberg ASIO SDK. WASAPI/WDM permanece disponível.")
    endif()
endif()

target_link_libraries(ClassicPlayer PRIVATE
    ClassicPlayerAssets
    ${CLASSIC_PLAYER_FLUID_TARGET}
    OpenSSL::Crypto
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Keep the complete WebUI inside plugin bundles as well.  The VST3/AU editor
# can then resolve resources relative to its own bundle instead of the host's
# working directory.
if(TARGET ClassicPlayer_VST3)
    juce_add_bundle_resources_directory(ClassicPlayer_VST3 Resources/WebUI)
endif()
if(TARGET ClassicPlayer_AU)
    juce_add_bundle_resources_directory(ClassicPlayer_AU Resources/WebUI)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h
    Source/Sf2Engine.cpp Source/Sf2Engine.h
    Source/Dx7Engine.cpp Source/Dx7Engine.h
    Source/ExternalInstrumentHost.cpp Source/ExternalInstrumentHost.h
    Source/LicenseVerifier.cpp Source/LicenseVerifier.h
    Source/ChordDetector.cpp Source/ChordDetector.h)

if(CLASSIC_PLAYER_BUILD_WEBUI_APP)
    juce_add_gui_app(ClassicPlayerApp
        PRODUCT_NAME "Classic Player"
        COMPANY_NAME "Classic Keys"
        BUNDLE_ID "com.classickeys.classicplayer"
        VERSION "${PROJECT_VERSION}"
        NEEDS_WEBVIEW2 TRUE
        ICON_BIG "Resources/classic-player-app-icon.png"
        ICON_SMALL "Resources/classic-player-app-icon.png")

    target_sources(ClassicPlayerApp PRIVATE Source/StandaloneMain.cpp)
    target_compile_definitions(ClassicPlayerApp PRIVATE
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        CLASSIC_PLAYER_VERSION="${PROJECT_VERSION}")
    target_link_libraries(ClassicPlayerApp PRIVATE
        juce::juce_audio_devices
        juce::juce_gui_extra
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
    juce_add_bundle_resources_directory(ClassicPlayerApp Resources/WebUI)
endif()

if(CLASSIC_PLAYER_BUILD_TESTS)
    juce_add_console_app(ClassicPlayerEngineSmokeTest PRODUCT_NAME "Classic Player Engine Smoke Test")
    target_sources(ClassicPlayerEngineSmokeTest PRIVATE
        Source/Sf2EngineSmokeTest.cpp
        Source/Sf2Engine.cpp
        Source/Sf2Engine.h)
    target_compile_definitions(ClassicPlayerEngineSmokeTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(ClassicPlayerEngineSmokeTest PRIVATE
        ${CLASSIC_PLAYER_FLUID_TARGET}
        juce::juce_audio_basics
        juce::juce_core
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    juce_add_console_app(ClassicPlayerChordDetectorTest PRODUCT_NAME "Classic Player Chord Detector Test")
    target_sources(ClassicPlayerChordDetectorTest PRIVATE
        Source/ChordDetectorTest.cpp
        Source/ChordDetector.cpp
        Source/ChordDetector.h)
    target_compile_definitions(ClassicPlayerChordDetectorTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ClassicPlayerChordDetectorTest PRIVATE
        juce::juce_core
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
