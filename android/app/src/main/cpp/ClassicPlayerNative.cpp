#include <jni.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#define TSF_IMPLEMENTATION
#include "tsf.h"

namespace
{
constexpr int kLayerCount = 6;
constexpr int kSampleRate = 48000;
constexpr int kMaxFrames = 2048;

std::array<tsf*, kLayerCount> fonts {};
std::array<float, kLayerCount> layerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float, kLayerCount> layerPeaks {};
std::array<short, kMaxFrames * 2> scratch {};
float masterGain = 0.8f;
float masterPeak = 0.0f;
std::mutex synthMutex;

void releaseLayer(const int layer)
{
    if (fonts[(size_t) layer] != nullptr)
    {
        tsf_close(fonts[(size_t) layer]);
        fonts[(size_t) layer] = nullptr;
    }
}

void sendAllNotesOff()
{
    for (auto* font : fonts)
        if (font != nullptr)
            tsf_channel_note_off_all(font, 0);
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadLayer(
        JNIEnv* env, jclass, jint layer, jstring path)
{
    if (layer < 0 || layer >= kLayerCount || path == nullptr) return JNI_FALSE;
    const char* utf8Path = env->GetStringUTFChars(path, nullptr);
    if (utf8Path == nullptr) return JNI_FALSE;

    std::lock_guard<std::mutex> lock(synthMutex);
    releaseLayer(layer);
    auto* loaded = tsf_load_filename(utf8Path);
    env->ReleaseStringUTFChars(path, utf8Path);
    if (loaded == nullptr) return JNI_FALSE;

    tsf_set_output(loaded, TSF_STEREO_INTERLEAVED, kSampleRate, 0.0f);
    // A layered SF2 often needs several voices for a single key. Reserve a
    // generous voice pool so chords and sustain behave like the desktop build.
    tsf_set_max_voices(loaded, 256);
    tsf_channel_set_presetnumber(loaded, 0, 0, TSF_FALSE);
    fonts[(size_t) layer] = loaded;
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeUnloadAll(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    for (int layer = 0; layer < kLayerCount; ++layer) releaseLayer(layer);
    layerPeaks.fill(0.0f);
    masterPeak = 0.0f;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetMaster(JNIEnv*, jclass, jfloat value)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    masterGain = std::clamp((float) value, 0.0f, 1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerGain(JNIEnv*, jclass, jint layer, jfloat value)
{
    if (layer < 0 || layer >= kLayerCount) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    layerGains[(size_t) layer] = std::clamp((float) value, 0.0f, 1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOn(JNIEnv*, jclass, jint note, jint velocity)
{
    if (note < 0 || note > 127 || velocity <= 0) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    for (auto* font : fonts)
        if (font != nullptr) tsf_channel_note_on(font, 0, note, std::min(velocity, 127) / 127.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOff(JNIEnv*, jclass, jint note)
{
    if (note < 0 || note > 127) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    for (auto* font : fonts)
        if (font != nullptr) tsf_channel_note_off(font, 0, note);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeControl(JNIEnv*, jclass, jint controller, jint value)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    for (auto* font : fonts)
        if (font != nullptr) tsf_channel_midi_control(font, 0, controller & 0x7f, value & 0x7f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeAllNotesOff(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    sendAllNotesOff();
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeRender(
        JNIEnv* env, jclass, jshortArray destination, jint frames)
{
    if (destination == nullptr || frames <= 0) return;
    frames = std::min(frames, kMaxFrames);
    const auto samples = frames * 2;
    if (env->GetArrayLength(destination) < samples) return;
    auto* output = env->GetShortArrayElements(destination, nullptr);
    if (output == nullptr) return;

    std::lock_guard<std::mutex> lock(synthMutex);
    std::memset(output, 0, (size_t) samples * sizeof(short));
    std::array<float, kLayerCount> renderedPeaks {};
    for (int layer = 0; layer < kLayerCount; ++layer)
    {
        auto* font = fonts[(size_t) layer];
        if (font == nullptr) continue;
        std::memset(scratch.data(), 0, (size_t) samples * sizeof(short));
        tsf_render_short(font, scratch.data(), frames, TSF_FALSE);
        const float gain = layerGains[(size_t) layer] * masterGain;
        for (int sample = 0; sample < samples; ++sample)
        {
            const int mixed = output[sample] + (int) (scratch[(size_t) sample] * gain);
            output[sample] = (short) std::clamp(mixed, -32768, 32767);
            renderedPeaks[(size_t) layer] = std::max(renderedPeaks[(size_t) layer],
                    std::abs((float) scratch[(size_t) sample] * gain) / 32768.0f);
        }
    }
    float renderedMasterPeak = 0.0f;
    for (int sample = 0; sample < samples; ++sample)
        renderedMasterPeak = std::max(renderedMasterPeak, std::abs((float) output[sample]) / 32768.0f);
    for (int layer = 0; layer < kLayerCount; ++layer)
        layerPeaks[(size_t) layer] = std::max(renderedPeaks[(size_t) layer], layerPeaks[(size_t) layer] * 0.88f);
    masterPeak = std::max(renderedMasterPeak, masterPeak * 0.88f);
    env->ReleaseShortArrayElements(destination, output, 0);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLayerPeak(JNIEnv*, jclass, jint layer)
{
    if (layer < 0 || layer >= kLayerCount) return 0.0f;
    std::lock_guard<std::mutex> lock(synthMutex);
    return layerPeaks[(size_t) layer];
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeMasterPeak(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    return masterPeak;
}
