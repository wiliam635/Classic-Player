#include <jni.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TSF_IMPLEMENTATION
#include "tsf.h"
#include "dx7note.h"
#include "exp2.h"
#include "sin.h"
#include "freqlut.h"
#include "env.h"
#include "pitchenv.h"
#include "porta.h"

namespace
{
constexpr int kLayerCount = 6;
constexpr int kSampleRate = 48000;
constexpr int kMaxFrames = 2048;

std::array<tsf*, kLayerCount> fonts {};
enum class EngineType : int { empty = 0, sf2 = 1, dx7 = 2, analog = 3 };
std::array<EngineType, kLayerCount> engineTypes {};
std::array<float, kLayerCount> layerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float, kLayerCount> layerPeaks {};
std::array<short, kMaxFrames * 2> scratch {};
float masterGain = 0.8f;
float masterPeak = 0.0f;
std::mutex synthMutex;

struct DxVoice
{
    bool active = false;
    int note = -1;
    std::unique_ptr<Dx7Note> synth;
    std::array<int32_t, N> samples {};
    int read = N;
};
struct DxLayer
{
    std::array<std::array<uint8_t, 156>, 32> patches {};
    std::array<std::string, 32> names {};
    int count = 0;
    int selected = 0;
    std::array<DxVoice, 32> voices {};
};
std::array<DxLayer, kLayerCount> dxLayers {};
FmCore fmCore;
Controllers controllers;
std::shared_ptr<TuningState> tuning;
bool dxReady = false;

struct AnalogVoice { bool active=false,releasing=false; int note=-1; double phase=0.0; float envelope=0.0f; };
struct AnalogLayer { int preset=0; std::array<AnalogVoice,32> voices {}; };
std::array<AnalogLayer,kLayerCount> analogLayers {};
constexpr std::array<const char*,8> analogNames { "Warm Pad", "Analog Brass", "Synth Lead", "Sub Bass", "Soft Poly", "Pulse Keys", "Air Pad", "Vintage Strings" };

void initialiseDx()
{
    if (dxReady) return;
    Exp2::init(); Sin::init(); Freqlut::init(kSampleRate); Env::init_sr(kSampleRate);
    PitchEnv::init(kSampleRate); Porta::init_sr(kSampleRate);
    tuning = createStandardTuning();
    controllers.core = &fmCore;
    controllers.refresh();
    dxReady = true;
}

std::string dxName(const uint8_t* data, int size)
{
    std::string result;
    for (int i = 0; i < size; ++i) { const char c = (char) (data[i] & 0x7f); if (c >= 32 && c <= 126) result.push_back(c); }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result.empty() ? "DX7 Voice" : result;
}

void expandDxPatch(const uint8_t* voice, std::array<uint8_t,156>& raw)
{
    raw.fill(0);
    for (int op = 0; op < 6; ++op) {
        const int packed=op*17, out=op*21;
        for (int i=0;i<=10;++i) raw[(size_t)(out+i)]=voice[packed+i];
        raw[(size_t)(out+11)]=voice[packed+11]&3; raw[(size_t)(out+12)]=(voice[packed+11]>>2)&3;
        raw[(size_t)(out+13)]=voice[packed+12]&7; raw[(size_t)(out+14)]=voice[packed+13]&3;
        raw[(size_t)(out+15)]=(voice[packed+13]>>2)&7; raw[(size_t)(out+16)]=voice[packed+14];
        raw[(size_t)(out+17)]=voice[packed+15]&1; raw[(size_t)(out+18)]=(voice[packed+15]>>1)&31;
        raw[(size_t)(out+19)]=voice[packed+16]; raw[(size_t)(out+20)]=(voice[packed+12]>>3)&15;
    }
    for(int i=0;i<8;++i) raw[(size_t)(126+i)]=voice[102+i];
    raw[134]=voice[110]&31; raw[135]=voice[111]&7; raw[136]=(voice[111]>>3)&1;
    for(int i=0;i<8;++i) raw[(size_t)(137+i)]=voice[112+i];
    for(int i=0;i<10;++i) raw[(size_t)(145+i)]=voice[118+i];
}

void clearDxLayer(int layer) { dxLayers[(size_t)layer] = {}; }

void releaseLayer(const int layer)
{
    if (fonts[(size_t) layer] != nullptr)
    {
        tsf_close(fonts[(size_t) layer]);
        fonts[(size_t) layer] = nullptr;
    }
    clearDxLayer(layer);
    analogLayers[(size_t)layer]={};
    engineTypes[(size_t)layer] = EngineType::empty;
}

void sendAllNotesOff()
{
    for (int layer=0;layer<kLayerCount;++layer) {
        if(fonts[(size_t)layer]!=nullptr)tsf_channel_note_off_all(fonts[(size_t)layer],0);
        for(auto& voice:dxLayers[(size_t)layer].voices)if(voice.active&&voice.synth)voice.synth->keyup();
        // Panic must be immediate; a lost MIDI note-off must never leave an
        // oscillator running while changing screens/devices.
        for(auto& voice:analogLayers[(size_t)layer].voices)voice={};
    }
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
    engineTypes[(size_t) layer] = EngineType::sf2;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadDx7(
        JNIEnv* env, jclass, jint layer, jstring path)
{
    if (layer < 0 || layer >= kLayerCount || path == nullptr) return JNI_FALSE;
    const char* utf8Path = env->GetStringUTFChars(path, nullptr);
    if (utf8Path == nullptr) return JNI_FALSE;
    std::ifstream input(utf8Path, std::ios::binary);
    env->ReleaseStringUTFChars(path, utf8Path);
    if (!input) return JNI_FALSE;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    int start = -1;
    for (int i=0; i+4102 <= (int)bytes.size(); ++i)
        if (bytes[(size_t)i]==0xf0 && bytes[(size_t)i+1]==0x43 && bytes[(size_t)i+3]==0x09
            && bytes[(size_t)i+4]==0x20 && bytes[(size_t)i+5]==0x00) { start=i+6; break; }
    if (start < 0) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(synthMutex);
    initialiseDx();
    releaseLayer(layer);
    auto& dx = dxLayers[(size_t)layer];
    for (int patch=0;patch<32;++patch) {
        const auto* packed=bytes.data()+start+patch*128;
        expandDxPatch(packed,dx.patches[(size_t)patch]);
        dx.names[(size_t)patch]=dxName(packed+118,10);
    }
    dx.count=32; dx.selected=0;
    engineTypes[(size_t)layer]=EngineType::dx7;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeEngineType(JNIEnv*, jclass, jint layer)
{
    if (layer<0||layer>=kLayerCount) return 0;
    std::lock_guard<std::mutex> lock(synthMutex);
    return (jint)engineTypes[(size_t)layer];
}

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeDx7PatchCount(JNIEnv*, jclass, jint layer)
{
    if(layer<0||layer>=kLayerCount)return 0; std::lock_guard<std::mutex> lock(synthMutex); return dxLayers[(size_t)layer].count;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeDx7PatchName(JNIEnv* env,jclass,jint layer,jint patch)
{
    if(layer<0||layer>=kLayerCount)return env->NewStringUTF(""); std::lock_guard<std::mutex> lock(synthMutex);
    auto& dx=dxLayers[(size_t)layer]; if(patch<0||patch>=dx.count)return env->NewStringUTF(""); return env->NewStringUTF(dx.names[(size_t)patch].c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetDx7Patch(JNIEnv*,jclass,jint layer,jint patch)
{
    if(layer<0||layer>=kLayerCount)return JNI_FALSE; std::lock_guard<std::mutex> lock(synthMutex);
    auto& dx=dxLayers[(size_t)layer]; if(patch<0||patch>=dx.count)return JNI_FALSE; dx.selected=patch;
    for(auto& voice:dx.voices)voice={}; return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeActivateAnalog(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return; std::lock_guard<std::mutex> lock(synthMutex); releaseLayer(layer); engineTypes[(size_t)layer]=EngineType::analog;
}
extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeAnalogPresetCount(JNIEnv*,jclass){return (jint)analogNames.size();}
extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeAnalogPresetName(JNIEnv* env,jclass,jint preset){return env->NewStringUTF(preset>=0&&preset<(int)analogNames.size()?analogNames[(size_t)preset]:"");}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetAnalogPreset(JNIEnv*,jclass,jint layer,jint preset)
{
    if(layer<0||layer>=kLayerCount||preset<0||preset>=(int)analogNames.size())return JNI_FALSE;
    std::lock_guard<std::mutex> lock(synthMutex); analogLayers[(size_t)layer].preset=preset; analogLayers[(size_t)layer].voices={}; return JNI_TRUE;
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

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativePresetCount(JNIEnv*, jclass, jint layer)
{
    if (layer < 0 || layer >= kLayerCount) return 0;
    std::lock_guard<std::mutex> lock(synthMutex);
    auto* font = fonts[(size_t) layer];
    return font == nullptr ? 0 : tsf_get_presetcount(font);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativePresetName(JNIEnv* env, jclass, jint layer, jint preset)
{
    if (layer < 0 || layer >= kLayerCount) return env->NewStringUTF("");
    std::lock_guard<std::mutex> lock(synthMutex);
    auto* font = fonts[(size_t) layer];
    const char* name = font == nullptr ? nullptr : tsf_get_presetname(font, preset);
    return env->NewStringUTF(name == nullptr ? "" : name);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetPreset(JNIEnv*, jclass, jint layer, jint preset)
{
    if (layer < 0 || layer >= kLayerCount) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(synthMutex);
    auto* font = fonts[(size_t) layer];
    if (font == nullptr || preset < 0 || preset >= tsf_get_presetcount(font)) return JNI_FALSE;
    tsf_channel_note_off_all(font, 0);
    return tsf_channel_set_presetindex(font, 0, preset) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOn(JNIEnv*, jclass, jint note, jint velocity)
{
    if (note < 0 || note > 127 || velocity <= 0) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    for (int layer=0;layer<kLayerCount;++layer) {
        if (engineTypes[(size_t)layer]==EngineType::sf2 && fonts[(size_t)layer]!=nullptr)
            tsf_channel_note_on(fonts[(size_t)layer],0,note,std::min(velocity,127)/127.0f);
        else if(engineTypes[(size_t)layer]==EngineType::dx7) {
            auto& dx=dxLayers[(size_t)layer]; DxVoice* target=nullptr;
            for(auto& voice:dx.voices)if(!voice.active){target=&voice;break;}
            if(target==nullptr)target=&dx.voices.front();
            target->active=true; target->note=note; target->read=N; target->samples.fill(0);
            target->synth=std::make_unique<Dx7Note>(tuning,nullptr);
            target->synth->init(dx.patches[(size_t)dx.selected].data(),note,std::min(velocity,127),1,&controllers);
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            auto& analog=analogLayers[(size_t)layer]; AnalogVoice* target=nullptr;
            for(auto& voice:analog.voices)if(!voice.active){target=&voice;break;} if(target==nullptr)target=&analog.voices.front();
            *target={}; target->active=true; target->note=note;
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOff(JNIEnv*, jclass, jint note)
{
    if (note < 0 || note > 127) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    for (int layer=0;layer<kLayerCount;++layer) {
        if(engineTypes[(size_t)layer]==EngineType::sf2&&fonts[(size_t)layer]!=nullptr)tsf_channel_note_off(fonts[(size_t)layer],0,note);
        else if(engineTypes[(size_t)layer]==EngineType::dx7)for(auto& voice:dxLayers[(size_t)layer].voices)if(voice.active&&voice.note==note&&voice.synth)voice.synth->keyup();
        // Stop analog voices immediately. This is intentionally stricter than
        // the release tail until device-specific note-off behaviour is proven.
        else if(engineTypes[(size_t)layer]==EngineType::analog)for(auto& voice:analogLayers[(size_t)layer].voices)if(voice.active&&voice.note==note)voice={};
    }
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
        if (engineTypes[(size_t)layer]==EngineType::empty) continue;
        std::memset(scratch.data(), 0, (size_t) samples * sizeof(short));
        if(engineTypes[(size_t)layer]==EngineType::sf2 && font!=nullptr) tsf_render_short(font, scratch.data(), frames, TSF_FALSE);
        else if(engineTypes[(size_t)layer]==EngineType::dx7) {
            auto& dx=dxLayers[(size_t)layer];
            for(int sample=0;sample<frames;++sample) {
                float value=0.0f;
                for(auto& voice:dx.voices) {
                    if(!voice.active||!voice.synth)continue;
                    if(voice.read>=N) {
                        if(!voice.synth->isPlaying()){voice={};continue;}
                        voice.samples.fill(0); voice.synth->compute(voice.samples.data(),1<<23,1<<24,&controllers); voice.read=0;
                    }
                    value+=(float)voice.samples[(size_t)voice.read++]/(float)(1<<24)*0.12f;
                }
                const int s=std::clamp((int)(value*32767.0f),-32768,32767);
                scratch[(size_t)sample*2]=(short)s; scratch[(size_t)sample*2+1]=(short)s;
            }
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            auto& analog=analogLayers[(size_t)layer]; const int preset=analog.preset;
            const float attack=preset==0||preset==6||preset==7?0.0018f:0.012f;
            const float release=preset==0||preset==6||preset==7?0.9992f:0.996f;
            for(int sample=0;sample<frames;++sample){float value=0.0f;
                for(auto& voice:analog.voices){if(!voice.active)continue;
                    voice.envelope=voice.releasing?voice.envelope*release:std::min(1.0f,voice.envelope+attack);
                    if(voice.envelope<0.0002f){voice={};continue;}
                    const double frequency=440.0*std::pow(2.0,((double)voice.note-69.0)/12.0);
                    voice.phase+=frequency/kSampleRate; if(voice.phase>=1.0)voice.phase-=1.0;
                    float wave=preset==2||preset==3?(float)(voice.phase<0.5?1.0:-1.0):(float)(2.0*voice.phase-1.0);
                    if(preset==0||preset==6)wave=0.65f*wave+0.35f*(float)std::sin(voice.phase*6.28318530718);
                    value+=wave*voice.envelope*0.12f;
                }
                int s=std::clamp((int)(value*32767.0f),-32768,32767); scratch[(size_t)sample*2]=(short)s; scratch[(size_t)sample*2+1]=(short)s;
            }
        }
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
