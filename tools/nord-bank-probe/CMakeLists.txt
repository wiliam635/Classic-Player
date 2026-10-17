cmake_minimum_required(VERSION 3.22)
project(NordBankProbe VERSION 2.0.5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
# Standalone test harness: FluidSynth headers supply LayerConfig's declarations;
# the Nord bank engine itself does not call or link libfluidsynth.
set(BUILD_SHARED_LIBS OFF)
set(FLUIDSYNTH_VERSION 2.5.5)
set(FLUIDSYNTH_VERSION_MAJOR 2)
set(FLUIDSYNTH_VERSION_MINOR 5)
set(FLUIDSYNTH_VERSION_MICRO 5)
configure_file("${FLUID_HEADERS}/fluidsynth.cmake" "${CMAKE_CURRENT_BINARY_DIR}/include/fluidsynth.h")
configure_file("${FLUID_HEADERS}/fluidsynth/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/include/fluidsynth/version.h")
add_subdirectory("${JUCE_DIR}" JUCE)
juce_add_console_app(NordBankProbe PRODUCT_NAME "Nord Bank Probe")
target_sources(NordBankProbe PRIVATE "${ROOT}/Source/NordBankRegressionTest.cpp"
    "${ROOT}/Source/NordBankEngine.cpp" "${ROOT}/Source/NordBundleImporter.cpp"
    "${ROOT}/Source/NordProgramDecoder.cpp" "${ROOT}/Source/SampleEngine.cpp"
    "${ROOT}/Source/NordPianoDecoder.cpp" "${ROOT}/Source/HammondEngine.cpp" "${ROOT}/Source/NordSampleDecoder.cpp"
    "${ROOT}/Source/NordSampleEngine.cpp" "${ROOT}/Source/AnalogSynthEngine.cpp" "${ROOT}/Source/NordSampleRegressionTest.cpp")
target_include_directories(NordBankProbe PRIVATE "${ROOT}/Source" "${CMAKE_CURRENT_BINARY_DIR}/include" "${FLUID_HEADERS}")
target_compile_definitions(NordBankProbe PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NORD_SAMPLE_TEST_EMBEDDED=1)
target_link_libraries(NordBankProbe PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_data_structures)
