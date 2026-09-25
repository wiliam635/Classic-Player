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
enum class EngineType : int { empty = 0, sf2 = 1, dx7 = 2, analog = 3, hammond = 4 };
std::array<EngineType, kLayerCount> engineTypes {};
std::array<float, kLayerCount> layerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float, kLayerCount> smoothedLayerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float,kLayerCount> layerPan{},smoothedLayerPan{};
std::array<int,kLayerCount> layerMidiChannel { -1,-1,-1,-1,-1,-1 }, layerOctave {}, layerLowNote {0,0,0,0,0,0}, layerHighNote {127,127,127,127,127,127}, layerVelocityCurve {};
std::array<int,kLayerCount> layerMidiMode {};
std::array<bool,kLayerCount> layerSustainEnabled {true,true,true,true,true,true};
std::array<std::array<std::array<int,128>,16>,kLayerCount> routedNotes {};
std::array<float, kLayerCount> layerAttack { 0.01f,0.01f,0.01f,0.01f,0.01f,0.01f };
std::array<float, kLayerCount> layerRelease { 0.25f,0.25f,0.25f,0.25f,0.25f,0.25f };
std::array<float,kLayerCount> layerCutoff {100,100,100,100,100,100},layerReverbSend {},layerCompressorMix {},layerChorusMix {};
std::array<float,kLayerCount> layerReverbSize {55,55,55,55,55,55},layerReverbDamping {45,45,45,45,45,45},layerReverbWidth {100,100,100,100,100,100};
std::array<float,kLayerCount> compressorAttack {0.01f,0.01f,0.01f,0.01f,0.01f,0.01f},compressorRelease {0.12f,0.12f,0.12f,0.12f,0.12f,0.12f},compressorMakeupDb {};
std::array<float,kLayerCount> lowPassState {},compressorEnvelope {};
std::array<float, kLayerCount> eqLow {}, eqMid {}, eqHigh {};
std::array<float,kLayerCount> eqLowFrequency {220,220,220,220,220,220},eqMidFrequency {1200,1200,1200,1200,1200,1200},eqHighFrequency {4200,4200,4200,4200,4200,4200};
std::array<float,kLayerCount> eqLowQ {.707f,.707f,.707f,.707f,.707f,.707f},eqMidQ {1,1,1,1,1,1},eqHighQ {.707f,.707f,.707f,.707f,.707f,.707f};
std::array<float,kLayerCount> eqHighPassHz {20,20,20,20,20,20},eqLowPassHz {20000,20000,20000,20000,20000,20000};
struct Biquad { float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0; };
std::array<std::array<Biquad,5>,kLayerCount> layerEqFilters{};
std::array<float, kLayerCount> compressorThreshold {0.126f,0.126f,0.126f,0.126f,0.126f,0.126f};
std::array<float, kLayerCount> compressorRatio {4.f,4.f,4.f,4.f,4.f,4.f};
float reverbMix = 0.0f, chorusMix = 0.0f;
float reverbDelayMs = 72.0f,reverbFeedback = 0.48f,reverbStereoWidth = 1.0f;
std::array<float, kSampleRate * 2> reverbBuffer {};
constexpr int kChorusFrames=2048;
std::array<std::array<float,kChorusFrames>,kLayerCount> layerChorusBuffer{};
std::array<int,kLayerCount> layerChorusCursor{};
int effectCursor = 0;
std::array<float, kLayerCount> layerPeaks {};
std::array<short, kMaxFrames * 2> scratch {};
float masterGain = 0.8f;
float smoothedMasterGain = 0.8f;
float masterPeak = 0.0f;
std::mutex synthMutex;

void setBiquad(Biquad& f,float b0,float b1,float b2,float a0,float a1,float a2)
{
    const float inv=1.0f/std::max(1.0e-8f,a0);f.b0=b0*inv;f.b1=b1*inv;f.b2=b2*inv;f.a1=a1*inv;f.a2=a2*inv;
}
void configurePeaking(Biquad& f,float frequency,float gainDb,float q)
{
    const float w=6.28318530718f*std::clamp(frequency,20.0f,20000.0f)/(float)kSampleRate;
    const float cosine=std::cos(w),alpha=std::sin(w)/(2.0f*std::clamp(q,0.1f,20.0f)),a=std::pow(10.0f,std::clamp(gainDb,-18.0f,18.0f)/40.0f);
    setBiquad(f,1.0f+alpha*a,-2.0f*cosine,1.0f-alpha*a,1.0f+alpha/a,-2.0f*cosine,1.0f-alpha/a);
}
void configureHighPass(Biquad& f,float frequency)
{
    const float w=6.28318530718f*std::clamp(frequency,20.0f,20000.0f)/(float)kSampleRate,cosine=std::cos(w),alpha=std::sin(w)/(2.0f*0.70710678f);
    setBiquad(f,(1+cosine)*.5f,-(1+cosine),(1+cosine)*.5f,1+alpha,-2*cosine,1-alpha);
}
void configureLowPass(Biquad& f,float frequency)
{
    const float w=6.28318530718f*std::clamp(frequency,20.0f,20000.0f)/(float)kSampleRate,cosine=std::cos(w),alpha=std::sin(w)/(2.0f*0.70710678f);
    setBiquad(f,(1-cosine)*.5f,1-cosine,(1-cosine)*.5f,1+alpha,-2*cosine,1-alpha);
}
void updateLayerEq(int layer)
{
    const auto i=(size_t)layer;configurePeaking(layerEqFilters[i][0],eqLowFrequency[i],eqLow[i],eqLowQ[i]);
    configurePeaking(layerEqFilters[i][1],eqMidFrequency[i],eqMid[i],eqMidQ[i]);configurePeaking(layerEqFilters[i][2],eqHighFrequency[i],eqHigh[i],eqHighQ[i]);
    configureHighPass(layerEqFilters[i][3],eqHighPassHz[i]);configureLowPass(layerEqFilters[i][4],eqLowPassHz[i]);
}
float processBiquad(Biquad& f,float input)
{
    const float output=f.b0*input+f.z1;f.z1=f.b1*input-f.a1*output+f.z2;f.z2=f.b2*input-f.a2*output;return output;
}
// The small built-in synths do not have a MIDI channel object like TSF does.
// Keep their physical key state here so sustain can defer Note Off correctly.
std::array<std::array<bool,128>,16> physicalKeys {};
std::array<bool,16> sustainDown {};

struct DxVoice
{
    bool active = false;
    int note = -1, channel = 0;
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

// A hard safety limit prevents a malformed/missing MIDI Note Off from leaving
// an oscillator active forever. Normal Note Off still stops it immediately.
constexpr int kInternalVoiceSafetySamples = kSampleRate * 8;
struct AnalogVoice {
    bool active=false,releasing=false; int note=-1,channel=0; int age=0;
    std::array<double,3> phase{}; std::array<float,3> increment{},targetIncrement{};
    double pitch=69.0,lfoPhase=0.0; float envelope=0.0f,filterState=0.0f,pinkState=0.0f;
    uint32_t noiseState=0x12345678u;
};
struct AnalogLayer {
    int preset=0;
    // Same 19 controls, in the order used by the approved desktop editor.
    std::array<float,19> controls {80,55,25,0,-12,0,72,12,35,6,180,78,260,4,0,0,0,0,0};
    float oscillator1Semitones=0.0f;
    std::array<int,3> waves {1,1,0};
    std::array<bool,3> oscillatorEnabled {true,true,true};
    std::array<AnalogVoice,32> voices {};
    float modWheel=0.0f;
    bool pinkNoise=false,monophonic=false;
};
std::array<AnalogLayer,kLayerCount> analogLayers {};
constexpr std::array<const char*,32> analogNames {
    "Solo Lead", "Modern Lead", "Classic Minimoog Lead", "Lucky Man", "Unison Lead", "Vintage Lead", "Easy Lead", "Screaming Lead",
    "Waterfall Lead", "Brass Lead", "Taurus Bass", "Funk Bass", "Authentic Minimoog Bass", "Thick Bass", "Analog Bass", "Percussive Bass",
    "Dub Bass", "Pulse Bass", "Crystal Pad", "Dream Pad", "Space Mod", "Atmospheric Pad", "Warm Pad", "Vintage Minimoog Pad",
    "Shimmer Pad", "Velvet Cloud", "Alien Landscape", "Digital Rain", "Submarine Sonar", "Thunder Storm", "Glass Harmonica", "Cosmic Drone"
};
struct HammondVoice { bool active=false; int note=-1,channel=0; int age=0; float envelope=0.0f; std::array<double,9> phase{}; };
struct HammondLayer {
    int preset=0, leslie=1, percussion=0;
    std::array<float,9> bars{};
    float click=0.15f, leakage=0.12f, drive=0.12f, level=0.8f;
    std::array<HammondVoice,32> voices{};
};
std::array<HammondLayer,kLayerCount> hammondLayers{};
constexpr std::array<const char*,8> hammondNames { "Jimmy Gospel", "Jazz Ballad", "Rock Organ", "Percussive B3", "Full Drawbar", "Gospel Fullness", "Slow Leslie", "Fast Leslie" };
constexpr std::array<std::array<float,9>,8> hammondBars {{
    {{.8f,.5f,1.f,.8f,.3f,.5f,.2f,.3f,.2f}}, {{.5f,.3f,1.f,.6f,.2f,.3f,.1f,.1f,0.f}},
    {{1.f,.8f,1.f,.9f,.7f,.8f,.6f,.7f,.6f}}, {{.3f,.2f,1.f,.7f,.1f,.2f,0.f,0.f,0.f}},
    {{1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f}}, {{1.f,.7f,1.f,.9f,.5f,.7f,.3f,.5f,.4f}},
    {{.5f,.4f,1.f,.7f,.3f,.4f,.2f,.2f,.1f}}, {{.8f,.6f,1.f,.9f,.5f,.7f,.4f,.5f,.3f}}
}};
constexpr std::array<double,9> hammondRatios {.5,1.5,1.,2.,3.,4.,5.,6.,8.};

float analogWave(int type,double phase)
{
    phase-=std::floor(phase);
    switch(type){case 0:return (float)(1.0-4.0*std::abs(phase-.5));case 1:return (float)(2.0*phase-1.0);
        case 2:return phase<.5?1.0f:-1.0f;case 3:return phase<.25?1.0f:-1.0f;default:return (float)std::sin(phase*6.28318530718);}
}

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
    hammondLayers[(size_t)layer]={};
    engineTypes[(size_t)layer] = EngineType::empty;
}

void sendAllNotesOff()
{
    for(auto& channel:sustainDown)channel=false;
    for(auto& channel:physicalKeys)channel.fill(false);
    for(auto& layer:routedNotes)for(auto& channel:layer)channel.fill(0);
    for (int layer=0;layer<kLayerCount;++layer) {
        if(fonts[(size_t)layer]!=nullptr)tsf_channel_note_off_all(fonts[(size_t)layer],0);
        for(auto& voice:dxLayers[(size_t)layer].voices)if(voice.active&&voice.synth)voice.synth->keyup();
        // Panic must be immediate; a lost MIDI note-off must never leave an
        // oscillator running while changing screens/devices.
        for(auto& voice:analogLayers[(size_t)layer].voices)voice={};
        for(auto& voice:hammondLayers[(size_t)layer].voices)voice={};
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

    // Keep the SF2 engine close to the desktop reference level while leaving
    // a little headroom for layered chords and the final safety limiter.
    // Four more dB of headroom keeps dense layered chords clean.
    tsf_set_output(loaded, TSF_STEREO_INTERLEAVED, kSampleRate, -6.0f);
    // Mobile devices cannot sustain desktop-sized voice pools. 64 voices keeps
    // normal piano chords responsive and avoids CPU underruns/distortion.
    tsf_set_max_voices(loaded, 64);
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
    // A DX7 32-voice bank is normally a 4104-byte SysEx message, but files
    // exported by different editors may contain a leading header, multiple
    // messages, or a trailing checksum/F7.  Locate the bulk-data header and
    // only require the 4096 bytes that contain the 32 packed voices.
    int start = -1;
    for (int i=0; i+6 <= (int)bytes.size(); ++i) {
        if (bytes[(size_t)i] == 0xf0 && bytes[(size_t)i+1] == 0x43 &&
            bytes[(size_t)i+3] == 0x09 && bytes[(size_t)i+4] == 0x20 &&
            bytes[(size_t)i+5] == 0x00 && i + 6 + 32 * 128 <= (int)bytes.size()) {
            start = i + 6;
            break;
        }
    }
    if (start < 0) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(synthMutex);
    initialiseDx();
    releaseLayer(layer);
    auto& dx = dxLayers[(size_t)layer];
    for (int patch=0;patch<32;++patch) {
        const auto* packed=bytes.data()+start+patch*128;
        expandDxPatch(packed,dx.patches[(size_t)patch]);
        dx.names[(size_t)patch]=dxName(packed+118,10);
        if (dx.names[(size_t)patch].empty())
            dx.names[(size_t)patch] = "Timbre DX7 " + std::to_string(patch + 1);
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
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetAnalogControls(JNIEnv* env,jclass,jint layer,jfloatArray values,jintArray flags)
{
    if(layer<0||layer>=kLayerCount||values==nullptr||flags==nullptr||env->GetArrayLength(values)<20||env->GetArrayLength(flags)<8)return;
    jfloat controls[20]{};jint options[8]{};env->GetFloatArrayRegion(values,0,20,controls);env->GetIntArrayRegion(flags,0,8,options);
    std::lock_guard<std::mutex> lock(synthMutex);auto& analog=analogLayers[(size_t)layer];
    constexpr float minimum[19]={0,0,0,-24,-24,0,0,0,0,1,1,0,1,.05f,0,0,0,0,0};
    constexpr float maximum[19]={100,100,100,24,24,100,100,100,100,2000,5000,100,5000,20,12,100,100,100,100};
    for(int i=0;i<19;++i)analog.controls[(size_t)i]=std::clamp((float)controls[i],minimum[i],maximum[i]);
    analog.oscillator1Semitones=std::clamp((float)controls[19],-24.0f,24.0f);
    for(int i=0;i<3;++i)analog.waves[(size_t)i]=std::clamp((int)options[i],0,4);
    for(int i=0;i<3;++i)analog.oscillatorEnabled[(size_t)i]=options[i+3]!=0;
    analog.pinkNoise=options[6]!=0;analog.monophonic=options[7]!=0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeActivateHammond(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return; std::lock_guard<std::mutex> lock(synthMutex); releaseLayer(layer); engineTypes[(size_t)layer]=EngineType::hammond;
    auto& organ=hammondLayers[(size_t)layer]; organ.bars=hammondBars[0];
}
extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeHammondPresetCount(JNIEnv*,jclass){return (jint)hammondNames.size();}
extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeHammondPresetName(JNIEnv* env,jclass,jint preset){return env->NewStringUTF(preset>=0&&preset<(int)hammondNames.size()?hammondNames[(size_t)preset]:"");}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetHammondPreset(JNIEnv*,jclass,jint layer,jint preset)
{
    if(layer<0||layer>=kLayerCount||preset<0||preset>=(int)hammondNames.size())return JNI_FALSE;
    std::lock_guard<std::mutex> lock(synthMutex); auto& organ=hammondLayers[(size_t)layer]; organ.preset=preset; organ.bars=hammondBars[(size_t)preset];
    organ.leslie=preset==7?2:preset==6?0:1; organ.percussion=preset==3?1:0; organ.voices={}; return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetHammondControls(JNIEnv* env,jclass,jint layer,jintArray bars,jint leslie,jint percussion,jfloat click,jfloat leakage,jfloat drive,jfloat level)
{
    if(layer<0||layer>=kLayerCount||bars==nullptr||env->GetArrayLength(bars)<9)return;
    jint values[9]{};env->GetIntArrayRegion(bars,0,9,values);std::lock_guard<std::mutex> lock(synthMutex);
    auto& organ=hammondLayers[(size_t)layer];for(int i=0;i<9;++i)organ.bars[(size_t)i]=std::clamp(values[i],0,8)/8.0f;
    organ.leslie=std::clamp((int)leslie,0,2);organ.percussion=std::clamp((int)percussion,0,2);
    organ.click=std::clamp((float)click,0.0f,1.0f);organ.leakage=std::clamp((float)leakage,0.0f,1.0f);
    organ.drive=std::clamp((float)drive,0.0f,1.0f);organ.level=std::clamp((float)level,0.0f,1.0f);
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
Java_com_classickeys_classicplayer_PolySynthEngine_nativeClearLayer(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);releaseLayer(layer);
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
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerPan(JNIEnv*,jclass,jint layer,jfloat value)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);layerPan[(size_t)layer]=std::clamp((float)value,-1.0f,1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerRouting(JNIEnv*,jclass,jint layer,jint channel,jint octave,jint low,jint high,jint velocityCurve,jboolean sustain,jint mode)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;layerMidiChannel[i]=channel<0?-1:std::clamp((int)channel,0,15);
    layerOctave[i]=std::clamp((int)octave,-4,4);layerLowNote[i]=std::clamp((int)low,0,127);
    layerHighNote[i]=std::clamp((int)high,layerLowNote[i],127);layerVelocityCurve[i]=std::clamp((int)velocityCurve,0,2);
    layerSustainEnabled[i]=sustain==JNI_TRUE;layerMidiMode[i]=std::clamp((int)mode,0,2);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerEnvelope(JNIEnv*, jclass, jint layer, jfloat attack, jfloat release)
{
    if (layer < 0 || layer >= kLayerCount) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    layerAttack[(size_t)layer] = std::clamp((float)attack, 0.001f, 2.0f);
    layerRelease[(size_t)layer] = std::clamp((float)release, 0.001f, 5.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerTone(JNIEnv*,jclass,jint layer,jfloat cutoff,jfloat reverb,jfloat compMix,jfloat chorus)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);const auto i=(size_t)layer;
    layerCutoff[i]=std::clamp((float)cutoff,0.0f,100.0f);layerReverbSend[i]=std::clamp((float)reverb,0.0f,1.0f);layerCompressorMix[i]=std::clamp((float)compMix,0.0f,1.0f);layerChorusMix[i]=std::clamp((float)chorus,0.0f,1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerEq(JNIEnv*, jclass, jint layer, jfloat low, jfloat mid, jfloat high,
    jfloat lowFrequency,jfloat midFrequency,jfloat highFrequency,jfloat lowQ,jfloat midQ,jfloat highQ,jfloat highPassHz,jfloat lowPassHz)
{
    if (layer < 0 || layer >= kLayerCount) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;
    eqLow[i]=std::clamp((float)low,-18.0f,18.0f);eqMid[i]=std::clamp((float)mid,-18.0f,18.0f);eqHigh[i]=std::clamp((float)high,-18.0f,18.0f);
    eqLowFrequency[i]=std::clamp((float)lowFrequency,40.0f,2000.0f);eqMidFrequency[i]=std::clamp((float)midFrequency,60.0f,12000.0f);eqHighFrequency[i]=std::clamp((float)highFrequency,1000.0f,20000.0f);
    eqLowQ[i]=std::clamp((float)lowQ,.1f,4.0f);eqMidQ[i]=std::clamp((float)midQ,.1f,20.0f);eqHighQ[i]=std::clamp((float)highQ,.1f,4.0f);
    eqHighPassHz[i]=std::clamp((float)highPassHz,20.0f,250.0f);eqLowPassHz[i]=std::clamp((float)lowPassHz,2000.0f,20000.0f);updateLayerEq(layer);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerCompressor(JNIEnv*, jclass, jint layer, jfloat threshold, jfloat ratio,jfloat attackMs,jfloat releaseMs,jfloat makeupDb)
{
    if (layer < 0 || layer >= kLayerCount) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    compressorThreshold[(size_t)layer] = std::clamp((float)threshold, 0.001f, 1.0f);
    compressorRatio[(size_t)layer] = std::clamp((float)ratio, 1.0f, 20.0f);
    compressorAttack[(size_t)layer]=std::clamp((float)attackMs,0.1f,100.0f)*0.001f;
    compressorRelease[(size_t)layer]=std::clamp((float)releaseMs,5.0f,1000.0f)*0.001f;
    compressorMakeupDb[(size_t)layer]=std::clamp((float)makeupDb,0.0f,24.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerReverb(JNIEnv*,jclass,jint layer,jfloat size,jfloat damping,jfloat width)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);const auto i=(size_t)layer;
    layerReverbSize[i]=std::clamp((float)size,0.0f,100.0f);layerReverbDamping[i]=std::clamp((float)damping,0.0f,100.0f);layerReverbWidth[i]=std::clamp((float)width,0.0f,100.0f);
    float sizeSum=0.0f,dampSum=0.0f,widthSum=0.0f;for(int n=0;n<kLayerCount;++n){sizeSum+=layerReverbSize[(size_t)n];dampSum+=layerReverbDamping[(size_t)n];widthSum+=layerReverbWidth[(size_t)n];}
    reverbDelayMs=20.0f+(sizeSum/kLayerCount)*1.05f;reverbFeedback=0.24f+(dampSum/kLayerCount)*0.0034f;reverbStereoWidth=widthSum/(kLayerCount*100.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetMasterEffects(JNIEnv*, jclass, jfloat reverb, jfloat chorus)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    reverbMix=std::clamp((float)reverb,0.0f,1.0f); chorusMix=std::clamp((float)chorus,0.0f,1.0f);
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
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOn(JNIEnv*, jclass, jint note, jint velocity, jint midiChannel)
{
    if (note < 0 || note > 127 || velocity <= 0 || midiChannel < 0 || midiChannel > 15) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    physicalKeys[(size_t)midiChannel][(size_t)note] = true;
    for (int layer=0;layer<kLayerCount;++layer) {
        const auto li=(size_t)layer;
        if(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=midiChannel)continue;
        if(note<layerLowNote[li]||note>layerHighNote[li])continue;
        const int routedNote=std::clamp(note+layerOctave[li]*12,0,127);
        int routedVelocity=std::clamp((int)velocity,1,127);
        if(layerVelocityCurve[li]==1)routedVelocity=std::clamp((int)std::lround(std::sqrt(routedVelocity/127.0f)*127.0f),1,127);
        else if(layerVelocityCurve[li]==2)routedVelocity=std::clamp((int)std::lround((routedVelocity/127.0f)*(routedVelocity/127.0f)*127.0f),1,127);
        routedNotes[li][(size_t)midiChannel][(size_t)note]=routedNote+1;
        double priorAnalogPitch=69.0;bool hasPriorAnalog=false;
        if(layerMidiMode[li]!=0||(engineTypes[li]==EngineType::analog&&analogLayers[li].monophonic)){
            if(engineTypes[li]==EngineType::sf2&&fonts[li]!=nullptr)for(int old=0;old<128;++old){int encoded=routedNotes[li][(size_t)midiChannel][(size_t)old];if(encoded!=0)tsf_channel_note_off(fonts[li],0,encoded-1);routedNotes[li][(size_t)midiChannel][(size_t)old]=0;}
            for(auto& old:dxLayers[li].voices)old={};for(auto& old:hammondLayers[li].voices)old={};
            for(auto& old:analogLayers[li].voices)if(old.active){priorAnalogPitch=old.pitch;hasPriorAnalog=true;old={};}
            routedNotes[li][(size_t)midiChannel][(size_t)note]=routedNote+1;
        }
        if (engineTypes[li]==EngineType::sf2 && fonts[li]!=nullptr)
            tsf_channel_note_on(fonts[li],0,routedNote,routedVelocity/127.0f);
        else if(engineTypes[(size_t)layer]==EngineType::dx7) {
            auto& dx=dxLayers[(size_t)layer]; DxVoice* target=nullptr;
            for(auto& voice:dx.voices)if(!voice.active){target=&voice;break;}
            if(target==nullptr)target=&dx.voices.front();
            target->active=true; target->note=routedNote; target->channel=midiChannel; target->read=N; target->samples.fill(0);
            target->synth=std::make_unique<Dx7Note>(tuning,nullptr);
            target->synth->init(dx.patches[(size_t)dx.selected].data(),routedNote,routedVelocity,1,&controllers);
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            auto& analog=analogLayers[(size_t)layer]; AnalogVoice* target=nullptr;
            for(auto& voice:analog.voices)if(!voice.active){target=&voice;break;} if(target==nullptr)target=&analog.voices.front();
            *target={}; target->active=true; target->note=routedNote; target->channel=midiChannel;
            target->pitch=(layerMidiMode[li]==2&&hasPriorAnalog)?priorAnalogPitch:routedNote;
            const float semitones[3]={analog.oscillator1Semitones,analog.controls[3],analog.controls[4]};
            for(int osc=0;osc<3;++osc){const double tune=std::pow(2.0,(double)semitones[osc]/12.0);target->targetIncrement[(size_t)osc]=(float)(440.0*std::pow(2.0,(double)(routedNote-69)/12.0)*tune/kSampleRate);target->increment[(size_t)osc]=(float)(440.0*std::pow(2.0,(target->pitch-69.0)/12.0)*tune/kSampleRate);}
        }
        else if(engineTypes[(size_t)layer]==EngineType::hammond) {
            auto& organ=hammondLayers[(size_t)layer]; HammondVoice* target=nullptr;
            for(auto& voice:organ.voices)if(!voice.active){target=&voice;break;} if(target==nullptr)target=&organ.voices.front();
            *target={}; target->active=true; target->note=routedNote; target->channel=midiChannel;
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOff(JNIEnv*, jclass, jint note, jint midiChannel)
{
    if (note < 0 || note > 127 || midiChannel < 0 || midiChannel > 15) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    physicalKeys[(size_t)midiChannel][(size_t)note] = false;
    for (int layer=0;layer<kLayerCount;++layer) {
        const auto li=(size_t)layer;const auto type = engineTypes[li];
        const int encoded=routedNotes[li][(size_t)midiChannel][(size_t)note];routedNotes[li][(size_t)midiChannel][(size_t)note]=0;
        if(encoded==0)continue;const int routedNote=encoded-1;
        if(layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel])continue;
        // Each engine gets its own dispatch path. Do not let a loaded font or
        // another engine's state suppress the custom-engine Note Off.
        if (type == EngineType::sf2 && fonts[li] != nullptr)
            tsf_channel_note_off(fonts[li], 0, routedNote);
        if (type == EngineType::dx7)
            for (auto& voice: dxLayers[li].voices)
                if (voice.active && voice.note == routedNote && voice.channel==midiChannel && voice.synth) voice.synth->keyup();
        // Hammond and Moog are key-gated engines: physical release always
        // closes their voice. Sustain is handled independently by engines
        // that support it, never by leaving these voices latched.
        if (type == EngineType::analog)
            for (auto& voice: analogLayers[(size_t)layer].voices)
                if (voice.active && voice.note == routedNote && voice.channel==midiChannel) voice.releasing=true;
        if (type == EngineType::hammond)
            for (auto& voice: hammondLayers[(size_t)layer].voices)
                if (voice.active && voice.note == routedNote && voice.channel==midiChannel) voice = {};
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeControl(JNIEnv*, jclass, jint controller, jint value, jint midiChannel)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    if(midiChannel<0||midiChannel>15)return;
    if((controller&0x7f)==1){for(int layer=0;layer<kLayerCount;++layer){const auto li=(size_t)layer;if(engineTypes[li]==EngineType::analog&&(layerMidiChannel[li]<0||layerMidiChannel[li]==midiChannel))analogLayers[li].modWheel=std::clamp((float)(value&0x7f)/127.0f,0.0f,1.0f);}}
    if ((controller & 0x7f) == 64) {
        const bool wasDown = sustainDown[(size_t)midiChannel];
        sustainDown[(size_t)midiChannel] = (value & 0x7f) >= 64;
        if (wasDown && !sustainDown[(size_t)midiChannel]) {
            for (int note = 0; note < 128; ++note) if (!physicalKeys[(size_t)midiChannel][(size_t)note]) {
                for (int layer=0; layer<kLayerCount; ++layer) {
                    const auto li=(size_t)layer;
                    if(!layerSustainEnabled[li])continue;
                    const int encoded=routedNotes[li][(size_t)midiChannel][(size_t)note];if(encoded==0)continue;
                    const int routedNote=encoded-1;routedNotes[li][(size_t)midiChannel][(size_t)note]=0;
                    if (engineTypes[li] == EngineType::analog)
                        for (auto& voice: analogLayers[li].voices) if (voice.active && voice.note == routedNote && voice.channel==midiChannel) voice.releasing=true;
                    else if (engineTypes[li] == EngineType::hammond)
                        for (auto& voice: hammondLayers[li].voices) if (voice.active && voice.note == routedNote && voice.channel==midiChannel) voice = {};
                    else if (engineTypes[li] == EngineType::dx7)
                        for (auto& voice: dxLayers[li].voices) if (voice.active && voice.note == routedNote && voice.channel==midiChannel && voice.synth) voice.synth->keyup();
                    else if (engineTypes[li] == EngineType::sf2 && fonts[li] != nullptr)
                        tsf_channel_note_off(fonts[li],0,routedNote);
                }
            }
        }
    }
    for (int layer=0;layer<kLayerCount;++layer)if(fonts[(size_t)layer]!=nullptr&&
        (layerMidiChannel[(size_t)layer]<0||layerMidiChannel[(size_t)layer]==midiChannel))
        tsf_channel_midi_control(fonts[(size_t)layer],0,controller&0x7f,value&0x7f);
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
    std::array<float, kMaxFrames * 2> mix {};
    std::array<float, kMaxFrames * 2> reverbSend {};
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
                    value+=(float)voice.samples[(size_t)voice.read++]/(float)(1<<24)*0.024f;
                }
                const int s=std::clamp((int)(value*32767.0f),-32768,32767);
                scratch[(size_t)sample*2]=(short)s; scratch[(size_t)sample*2+1]=(short)s;
            }
        }
        else if(engineTypes[(size_t)layer]==EngineType::hammond) {
            auto& organ=hammondLayers[(size_t)layer]; const auto& bars=organ.bars;
            const float leslieRate=organ.leslie==2?6.2f:organ.leslie==1?.8f:0.0f;
            for(int sample=0;sample<frames;++sample){float value=0.f;
                for(auto& voice:organ.voices){if(!voice.active)continue;if(++voice.age>kInternalVoiceSafetySamples){voice={};continue;}voice.envelope=std::min(1.f,voice.envelope+std::min(1.f,1.f/(layerAttack[(size_t)layer]*kSampleRate)));
                    const double base=440.0*std::pow(2.0,((double)voice.note-69.0)/12.0);float tone=0.f,total=0.f;
                    for(int d=0;d<9;++d){voice.phase[(size_t)d]+=base*hammondRatios[(size_t)d]/kSampleRate;voice.phase[(size_t)d]-=std::floor(voice.phase[(size_t)d]);tone+=(float)std::sin(voice.phase[(size_t)d]*6.28318530718)*bars[(size_t)d];total+=bars[(size_t)d];}
                    const float percussion=organ.percussion==0?0.0f:(float)std::sin(voice.phase[organ.percussion==1?5:6]*6.28318530718)*std::exp(-(float)voice.age/(kSampleRate*.16f))*.28f;
                    const float keyClick=voice.age<80?(float)std::sin(voice.age*2.39996323)*std::exp(-(float)voice.age/20.0f)*organ.click*.035f:0.0f;
                    tone=tone/std::max(total,.1f)+percussion+keyClick+tone*organ.leakage*.025f;
                    const float rotary=leslieRate==0.0f?1.0f:.82f+.18f*(float)std::sin(voice.phase[2]*leslieRate);
                    value+=std::tanh(tone*(1.0f+organ.drive*5.0f))*voice.envelope*rotary*.30f*organ.level;
                }int s=std::clamp((int)(value*32767.f),-32768,32767);scratch[(size_t)sample*2]=(short)s;scratch[(size_t)sample*2+1]=(short)s;
            }
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            auto& analog=analogLayers[(size_t)layer];const auto& control=analog.controls;
            const float attackSamples=std::max(1.0f,control[9]*.001f*kSampleRate),decaySamples=std::max(1.0f,control[10]*.001f*kSampleRate),releaseMs=std::max(1.0f,control[12]);
            const float releaseFactor=std::exp(-1.0f/(releaseMs*.001f*kSampleRate)),sustain=control[11]*.01f;
            const float baseCutoff=25.0f*std::pow(700.0f,control[6]*.01f),resonance=std::min(.82f,control[7]*.008f),drive=control[16]*.06f;
            const float lfoStep=6.28318530718f*control[13]/kSampleRate;
            for(int sample=0;sample<frames;++sample){float value=0.0f;
                for(auto& voice:analog.voices){if(!voice.active)continue;if(++voice.age>kInternalVoiceSafetySamples){voice={};continue;}
                    if(layerMidiMode[li]==2)for(int osc=0;osc<3;++osc)voice.increment[(size_t)osc]+=(voice.targetIncrement[(size_t)osc]-voice.increment[(size_t)osc])*.0008f;
                    if(voice.releasing)voice.envelope*=releaseFactor;
                    else if((float)voice.age<attackSamples)voice.envelope=std::min(1.0f,(float)voice.age/attackSamples);
                    else if((float)voice.age<attackSamples+decaySamples)voice.envelope=1.0f-(1.0f-sustain)*((float)voice.age-attackSamples)/decaySamples;
                    else voice.envelope=sustain;
                    if(voice.envelope<.00015f){voice={};continue;}
                    voice.lfoPhase+=lfoStep;if(voice.lfoPhase>=6.28318530718)voice.lfoPhase-=6.28318530718;
                    const float lfo=(float)std::sin(voice.lfoPhase),filterEnvelope=std::exp(-(float)voice.age/(std::max(1.0f,decaySamples)*2.0f));
                    float mixed=0.0f,total=0.0f;
                    for(int osc=0;osc<3;++osc){if(!analog.oscillatorEnabled[(size_t)osc])continue;voice.phase[(size_t)osc]+=voice.increment[(size_t)osc];if(voice.phase[(size_t)osc]>=1.0)voice.phase[(size_t)osc]-=std::floor(voice.phase[(size_t)osc]);const float level=control[osc]*.01f;mixed+=analogWave(analog.waves[(size_t)osc],voice.phase[(size_t)osc])*level;total+=level;}
                    if(control[5]>0.01f){voice.noiseState^=voice.noiseState<<13;voice.noiseState^=voice.noiseState>>17;voice.noiseState^=voice.noiseState<<5;const float white=(float)(voice.noiseState&0xffff)/32767.5f-1.0f;float noise=white;if(analog.pinkNoise){voice.pinkState+=(white-voice.pinkState)*.045f;noise=voice.pinkState*2.5f;}mixed+=noise*control[5]*.0018f;}
                    if(total>1.0f)mixed/=total;
                    mixed=std::tanh(mixed*(1.0f+drive));
                    float cutoff=baseCutoff*std::pow(2.0f,(control[8]*.04f*filterEnvelope+(voice.note-60)*control[17]*.012f+control[15]*.06f*lfo+control[18]*.04f*analog.modWheel)/12.0f);
                    cutoff=std::clamp(cutoff,30.0f,18000.0f);const float alpha=std::clamp(6.2831853f*cutoff/kSampleRate,.0001f,.95f);
                    voice.filterState+=alpha*(mixed-voice.filterState*(1.0f+resonance));
                    value+=voice.filterState*voice.envelope*.24f;
                }
                int s=std::clamp((int)(value*32767.0f),-32768,32767);scratch[(size_t)sample*2]=(short)s;scratch[(size_t)sample*2+1]=(short)s;
            }
        }
        const float targetLayer = layerGains[(size_t) layer];
        const float targetMaster = masterGain;
        // Gain changes are smoothed per block to avoid clicks when a fader is
        // moved while notes are sounding.
        const float layerStep = (targetLayer - smoothedLayerGains[(size_t)layer]) / (float)std::max(frames, 1);
        const float panStep=(layerPan[(size_t)layer]-smoothedLayerPan[(size_t)layer])/(float)std::max(frames,1);
        const float masterStep = (targetMaster - smoothedMasterGain) / (float)std::max(frames, 1);
        for (int sample = 0; sample < samples; ++sample)
        {
            if ((sample & 1) == 0) {
                const float input = (float)scratch[(size_t)sample] / 32768.0f;
                const auto eqIndex=(size_t)layer;float shaped=input;
                for(auto& filter:layerEqFilters[eqIndex])shaped=processBiquad(filter,shaped);
                const float cutoffHz=20.0f*std::pow(900.0f,layerCutoff[(size_t)layer]/100.0f);
                const float cutoffAlpha=1.0f-std::exp(-6.2831853f*cutoffHz/(float)kSampleRate);
                lowPassState[(size_t)layer]+=(shaped-lowPassState[(size_t)layer])*cutoffAlpha;
                shaped=lowPassState[(size_t)layer];
                const float magnitude=std::abs(shaped),threshold=compressorThreshold[(size_t)layer];
                const float time=magnitude>compressorEnvelope[(size_t)layer]?compressorAttack[(size_t)layer]:compressorRelease[(size_t)layer];
                const float detector=std::exp(-1.0f/(std::max(0.0001f,time)*(float)kSampleRate));
                compressorEnvelope[(size_t)layer]=detector*compressorEnvelope[(size_t)layer]+(1.0f-detector)*magnitude;
                const float envelope=std::max(0.000001f,compressorEnvelope[(size_t)layer]);
                const float compressed=envelope>threshold?shaped*(threshold+(envelope-threshold)/compressorRatio[(size_t)layer])/envelope:shaped;
                shaped=shaped+(compressed-shaped)*layerCompressorMix[(size_t)layer];
                shaped*=std::pow(10.0f,compressorMakeupDb[(size_t)layer]/20.0f);
                const int cursor=layerChorusCursor[(size_t)layer];
                const float lfo=std::sin(cursor*0.006135923f);
                const int chorusDelay=std::clamp((int)(720.0f+lfo*300.0f),1,kChorusFrames-1);
                const int chorusRead=(cursor+kChorusFrames-chorusDelay)%kChorusFrames;
                const float delayedChorus=layerChorusBuffer[(size_t)layer][(size_t)chorusRead];
                layerChorusBuffer[(size_t)layer][(size_t)cursor]=shaped;
                layerChorusCursor[(size_t)layer]=(cursor+1)%kChorusFrames;
                shaped+=(delayedChorus-shaped)*layerChorusMix[(size_t)layer];
                const short shapedShort=(short)std::clamp((int)(shaped*32768.0f),-32768,32767); scratch[(size_t)sample]=shapedShort; scratch[(size_t)sample+1]=shapedShort;
            }
            const int frame = sample / 2;
            const float gain = (smoothedLayerGains[(size_t)layer] + layerStep * frame) *
                    (smoothedMasterGain + masterStep * frame);
            const float pan=std::clamp(smoothedLayerPan[(size_t)layer]+panStep*frame,-1.0f,1.0f);
            const float sideGain=sample%2==0?std::cos((pan+1.0f)*0.7853981634f):std::sin((pan+1.0f)*0.7853981634f);
            const float layerSample=((float)scratch[(size_t)sample] / 32768.0f) * gain * sideGain;
            mix[(size_t)sample] += layerSample;
            reverbSend[(size_t)sample]+=layerSample*layerReverbSend[(size_t)layer];
            renderedPeaks[(size_t) layer] = std::max(renderedPeaks[(size_t) layer],
                    std::abs((float) scratch[(size_t) sample] * gain) / 32768.0f);
        }
        smoothedLayerGains[(size_t)layer] = targetLayer;
        smoothedLayerPan[(size_t)layer]=layerPan[(size_t)layer];
    }
    smoothedMasterGain = masterGain;
    float renderedMasterPeak = 0.0f;
    for (int sample = 0; sample < samples; ++sample) {
        // Soft limiting prevents the harsh integer clipping heard when several
        // SF2 regions or layers peak at the same time.
        // Leave extra headroom before the soft limiter so several active
        // layers do not hit the limiter hard and sound distorted.
        const int baseDelay=(int)(reverbDelayMs*kSampleRate/1000.0f);
        const int delay=baseDelay+((sample&1)!=0?(int)(reverbStereoWidth*kSampleRate*.018f):0);
        const int read = (effectCursor + (int)reverbBuffer.size() - delay + (int)reverbBuffer.size()) % (int)reverbBuffer.size();
        const float delayed = reverbBuffer[(size_t)read];
        const float chorus = reverbBuffer[(size_t)((effectCursor + (int)reverbBuffer.size() - 960 + (sample & 3) * 24) % (int)reverbBuffer.size())];
        const float dry = mix[(size_t)sample];
        const float send=reverbSend[(size_t)sample]+dry*reverbMix;
        const float effected = dry + delayed * 0.32f + (chorus - dry) * chorusMix * 0.22f;
        reverbBuffer[(size_t)effectCursor] = send + delayed * reverbFeedback;
        effectCursor = (effectCursor + 1) % (int)reverbBuffer.size();
        const float limited = std::tanh(effected * 0.62f);
        output[sample] = (short)std::clamp((int)(limited * 32767.0f), -32768, 32767);
        renderedMasterPeak = std::max(renderedMasterPeak, std::abs((float) output[sample]) / 32768.0f);
    }
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
