#include <jni.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
#include "NordPianoEngine.h"
#include "NsmpEngine.h"

namespace
{
constexpr int kLayerCount = 6;
constexpr int kSampleRate = 44100;
constexpr int kMaxFrames = 2048;
constexpr int kMaximumPolyphony = 256;
constexpr size_t kVoicePoolCapacity = kMaximumPolyphony;
constexpr int kLowPolyphonyEngineLimit = 32;
static_assert(nord_android::maxVoices >= kMaximumPolyphony,
              "NPNO voice storage must cover the shared polyphony ceiling");
constexpr int kHammondDelayFrames = 1204;
constexpr int kHammondVibratoFrames = 1024; // > 10 ms up to 96 kHz
constexpr float kHammondOutputCalibration = 3.25f;

std::array<tsf*, kLayerCount> fonts {};
enum class EngineType : int { empty = 0, sf2 = 1, dx7 = 2, analog = 3, hammond = 4, nord = 7, nsmp = 8 };
std::array<EngineType, kLayerCount> engineTypes {};
std::array<nord_android::Layer,kLayerCount> nordLayers {};
std::array<nsmp_android::Layer,kLayerCount> nsmpLayers {};
// Audition SoundFonts without layer processing or envelope overrides.
std::array<bool, kLayerCount> sf2OriginalSound {false,false,false,false,false,false};
std::array<int, kLayerCount> layerVoiceBudgets {};
std::array<float, kLayerCount> layerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float, kLayerCount> smoothedLayerGains { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
std::array<float,kLayerCount> layerPan{},smoothedLayerPan{};
// Per-layer velocity shaping that matches the desktop routing: incoming gate (VEL. MIN/MAX),
// sensitivity off (fixed 100), user curve (shaped table) and the NPNO timbre floor.
std::array<int,kLayerCount> layerVelocityLow {1,1,1,1,1,1}, layerVelocityHigh {127,127,127,127,127,127}, layerTimbreFloor {1,1,1,1,1,1};
std::array<bool,kLayerCount> layerSensitivityOff {};
std::array<std::array<uint8_t,128>,kLayerCount> layerVelocityTable {};
std::array<bool,kLayerCount> layerCustomVelocity {};
void buildVelocityTable(size_t layer,int minimum,int maximum,float slope){
    if(layerCustomVelocity[layer])return;   // a drawn curve owns the table until it is cleared
    minimum=std::clamp(minimum,1,127);maximum=std::clamp(maximum,minimum,127);slope=std::clamp(slope,-100.0f,100.0f);
    const float exponent=std::exp2(-slope/50.0f);auto& table=layerVelocityTable[layer];table[0]=1;
    for(int value=1;value<128;++value){
        const float input=(value-1.0f)/126.0f;const float output=minimum+(maximum-minimum)*std::pow(input,exponent);
        table[(size_t)value]=(uint8_t)std::clamp((int)std::lround(output),1,127);
    }
}
std::array<int,kLayerCount> layerMidiChannel { -1,-1,-1,-1,-1,-1 }, layerOctave {}, layerLowNote {0,0,0,0,0,0}, layerHighNote {127,127,127,127,127,127}, layerVelocityCurve {};
std::array<int,kLayerCount> layerMidiMode {};
std::array<std::array<int,3>,kLayerCount> nordVelocityCalibration {};
std::array<bool,kLayerCount> layerSustainEnabled {true,true,true,true,true,true};
std::array<bool,kLayerCount> layerModulationEnabled {};
std::array<std::array<std::array<int,128>,16>,kLayerCount> routedNotes {};
// SF2 editor values are additive seconds. Zero means use each region's exact
// SoundFont envelope; never infer a fake 5/50 ms envelope for the preset.
std::array<float, kLayerCount> layerAttack { 0.005f,0.005f,0.005f,0.005f,0.005f,0.005f };
std::array<float, kLayerCount> layerRelease { 0.05f,0.05f,0.05f,0.05f,0.05f,0.05f };
// Additional DECAY / SUSTAIN stage (desktop ADSR). 0 ms bypasses it, so existing sounds are unchanged.
std::array<float, kLayerCount> layerDecayMs {}, layerSustainPercent { 100.0f,100.0f,100.0f,100.0f,100.0f,100.0f };
std::array<float,kLayerCount> layerCutoff {100,100,100,100,100,100},layerReverbSend {},layerCompressorMix {},layerChorusMix {};
std::array<float,kLayerCount> layerReverbSize {55,55,55,55,55,55},layerReverbDamping {45,45,45,45,45,45},layerReverbWidth {100,100,100,100,100,100};
std::array<float,kLayerCount> compressorAttack {0.010f,0.010f,0.010f,0.010f,0.010f,0.010f},compressorRelease {0.120f,0.120f,0.120f,0.120f,0.120f,0.120f},compressorMakeupDb {};
std::array<std::array<float,2>,kLayerCount> lowPassState {};
std::array<float,kLayerCount> compressorEnvelope {};
std::array<float, kLayerCount> eqLow {}, eqMid {}, eqHigh {};
std::array<float,kLayerCount> eqLowFrequency {220,220,220,220,220,220},eqMidFrequency {1200,1200,1200,1200,1200,1200},eqHighFrequency {4200,4200,4200,4200,4200,4200};
std::array<float,kLayerCount> eqLowQ {.707f,.707f,.707f,.707f,.707f,.707f},eqMidQ {1,1,1,1,1,1},eqHighQ {.707f,.707f,.707f,.707f,.707f,.707f};
std::array<float,kLayerCount> eqHighPassHz {20,20,20,20,20,20},eqLowPassHz {20000,20000,20000,20000,20000,20000};
struct Biquad { float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0; };
std::array<std::array<std::array<Biquad,5>,2>,kLayerCount> layerEqFilters{};
std::array<float, kLayerCount> compressorThreshold {.1259f,.1259f,.1259f,.1259f,.1259f,.1259f};
std::array<float, kLayerCount> compressorRatio {4.f,4.f,4.f,4.f,4.f,4.f};
float reverbMix = 0.0f, chorusMix = 0.0f;
float reverbDelayMs = 72.0f,reverbFeedback = 0.48f,reverbStereoWidth = 1.0f;
std::array<float, kSampleRate * 2> reverbBuffer {};
constexpr int kChorusFrames=2048;
std::array<std::array<std::array<float,kChorusFrames>,2>,kLayerCount> layerChorusBuffer{};
std::array<std::array<int,2>,kLayerCount> layerChorusCursor{};
int effectCursor = 0;
// Freeverb-style stereo room reverb (8 damped combs + 4 all-passes per side)
// replaces the old single feedback echo, whose tail was gone in ~0.3 s.
constexpr int kCombLengths[8]={1116,1188,1277,1356,1422,1491,1557,1617};
constexpr int kAllpassLengths[4]={556,441,341,225};
constexpr int kStereoSpread=23;
struct FreeverbSide{
    std::array<std::array<float,1617+kStereoSpread>,8> comb{};std::array<float,8> damped{};std::array<int,8> combPos{};
    std::array<std::array<float,556+kStereoSpread>,4> allpass{};std::array<int,4> allpassPos{};
    void clear(){for(auto& c:comb)c.fill(0.0f);damped.fill(0.0f);combPos.fill(0);for(auto& a:allpass)a.fill(0.0f);allpassPos.fill(0);}
    float process(float input,int spread,float feedback,float damp){
        float out=0.0f;
        for(int c=0;c<8;++c){
            const int length=kCombLengths[c]+spread;auto& line=comb[(size_t)c];int& pos=combPos[(size_t)c];
            const float delayed=line[(size_t)pos];
            damped[(size_t)c]=delayed*(1.0f-damp)+damped[(size_t)c]*damp;
            line[(size_t)pos]=input+damped[(size_t)c]*feedback;
            if(++pos>=length)pos=0;out+=delayed;
        }
        for(int a=0;a<4;++a){
            const int length=kAllpassLengths[a]+spread;auto& line=allpass[(size_t)a];int& pos=allpassPos[(size_t)a];
            const float buffered=line[(size_t)pos];
            line[(size_t)pos]=out+buffered*0.5f;out=buffered-out;
            if(++pos>=length)pos=0;
        }
        return out;
    }
};
FreeverbSide reverbLeft,reverbRight;float pendingWetLeft=0.0f,pendingWetRight=0.0f;int reverbHoldSamples=0;bool reverbWasRunning=false;
std::array<float, kLayerCount> layerPeaks {};
// A layer whose gain is 0 (MUTE, or another layer soloed) renders nothing once
// its release tails had this long to finish, and ignores new notes meanwhile.
constexpr int kSilentGraceSamples = kSampleRate * 3;
std::array<int, kLayerCount> silentSamples {};
std::array<std::atomic<float>, kLayerCount> publishedLayerPeaks {
    0.0f,0.0f,0.0f,0.0f,0.0f,0.0f
};
// Keep each engine in float until the final output conversion. Clipping a
// SoundFont or a processed layer to PCM16 here cannot be undone by the master.
std::array<float, kMaxFrames * 2> scratch {};
float masterGain = 0.8f;
float smoothedMasterGain = 0.8f;
std::atomic<float> switchFadeTarget{1.0f};
std::atomic<int> switchFadeMs{35};
float switchFadeCurrent = 1.0f, blockFade = 1.0f;
float masterPeak = 0.0f;
std::atomic<float> publishedMasterPeak {0.0f};
std::mutex synthMutex;

enum class MidiCommandType : uint8_t { NoteOn, NoteOff, Control, AllNotesOff, PitchBend };
struct MidiCommand { MidiCommandType type; int value1, value2, channel; uint64_t queuedAtNanos = 0; };
constexpr size_t kMidiQueueCapacity = 2048;
std::array<MidiCommand, kMidiQueueCapacity> midiQueue {};
size_t midiQueueRead = 0, midiQueueWrite = 0, midiQueueCount = 0;
std::mutex midiQueueMutex;
std::atomic<int> publishedMidiQueueDepth {0};
std::atomic<int> midiQueueHighWater {0};
std::atomic<int> midiQueueOverflows {0};
std::atomic<int> midiQueueConsumerLockMisses {0};
std::atomic<int> synthRealtimeLockMisses {0};
std::atomic<int> midiQueueMaxWaitMicros {0};
std::atomic<int> masterClippedBlocks {0};
std::atomic<int> nordDiagActive {0}, nordDiagHeld {0}, nordDiagWaiting {0}, nordDiagPedal {0};

uint64_t monotonicNanos()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

void publishMaximum(std::atomic<int>& destination,int value)
{
    int previous=destination.load(std::memory_order_relaxed);
    while(value>previous&&!destination.compare_exchange_weak(
            previous,value,std::memory_order_relaxed)) { }
}

void publishMidiQueueDepth()
{
    const int depth=static_cast<int>(midiQueueCount);
    publishedMidiQueueDepth.store(depth,std::memory_order_relaxed);
    int highWater=midiQueueHighWater.load(std::memory_order_relaxed);
    while(depth>highWater&&!midiQueueHighWater.compare_exchange_weak(
            highWater,depth,std::memory_order_relaxed)) { }
}

void enqueueMidiCommand(MidiCommand command)
{
    command.queuedAtNanos=monotonicNanos();
    std::lock_guard<std::mutex> lock(midiQueueMutex);
    // If a producer outruns audio, discard the stale backlog and panic before
    // accepting the newest event. This is preferable to leaving a stuck note.
    if (midiQueueCount >= kMidiQueueCapacity - 1) {
        midiQueueOverflows.fetch_add(1,std::memory_order_relaxed);
        midiQueueRead = midiQueueWrite = midiQueueCount = 0;
        midiQueue[midiQueueWrite++] = {MidiCommandType::AllNotesOff, 0, 0, 0, monotonicNanos()};
        ++midiQueueCount;
    }
    midiQueue[midiQueueWrite] = command;
    midiQueueWrite = (midiQueueWrite + 1) % kMidiQueueCapacity;
    ++midiQueueCount;
    publishMidiQueueDepth();
}

size_t takeMidiCommands(MidiCommand* destination, size_t capacity)
{
    std::unique_lock<std::mutex> lock(midiQueueMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        midiQueueConsumerLockMisses.fetch_add(1,std::memory_order_relaxed);
        return 0;
    }
    const size_t count = std::min(capacity, midiQueueCount);
    const uint64_t now=monotonicNanos();
    for (size_t i = 0; i < count; ++i) {
        const MidiCommand& command=midiQueue[midiQueueRead];
        if(command.queuedAtNanos>0&&now>=command.queuedAtNanos){
            const uint64_t elapsed=(now-command.queuedAtNanos)/1000;
            publishMaximum(midiQueueMaxWaitMicros,(int)std::min<uint64_t>(elapsed,INT32_MAX));
        }
        destination[i] = command;
        midiQueueRead = (midiQueueRead + 1) % kMidiQueueCapacity;
    }
    midiQueueCount -= count;
    publishMidiQueueDepth();
    return count;
}

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
    const auto i=(size_t)layer;
    for(auto& channel:layerEqFilters[i]){
        configurePeaking(channel[0],eqLowFrequency[i],eqLow[i],eqLowQ[i]);
        configurePeaking(channel[1],eqMidFrequency[i],eqMid[i],eqMidQ[i]);
        configurePeaking(channel[2],eqHighFrequency[i],eqHigh[i],eqHighQ[i]);
        configureHighPass(channel[3],eqHighPassHz[i]);configureLowPass(channel[4],eqLowPassHz[i]);
    }
}
float processBiquad(Biquad& f,float input)
{
    const float output=f.b0*input+f.z1;f.z1=f.b1*input-f.a1*output+f.z2;f.z2=f.b2*input-f.a2*output;return output;
}
// The small built-in synths do not have a MIDI channel object like TSF does.
// Keep their physical key state here so sustain can defer Note Off correctly.
std::array<std::array<bool,128>,16> physicalKeys {};
std::array<bool,16> sustainDown {};
std::atomic<int> globalTranspose{0};
std::atomic<bool> sustainInverted{false};

struct DxVoice
{
    bool active = false;
    uint64_t serial = 0;
    int note = -1, channel = 0;
    std::unique_ptr<Dx7Note> synth;
    float attackGain = 1.0f, releaseGain = 1.0f;   // layer ATTACK / RELEASE knobs on top of the patch envelope
    std::array<int32_t, N> samples {};
    int read = N;
    bool released = false;
    float decayGain = 1.0f;
    uint64_t age = 0;
    // Re-struck key under the pedal: the voice the pedal was holding fades out in 0.2 s.
    bool fading = false;
    float fadeGain = 1.0f;
};
struct DxLayer
{
    uint64_t serial = 0;
    std::array<std::array<uint8_t, 156>, 32> patches {};
    std::array<std::string, 32> names {};
    int count = 0;
    int selected = 0;
    std::array<DxVoice, kVoicePoolCapacity> voices {};
};
std::array<DxLayer, kLayerCount> dxLayers {};
FmCore fmCore;
Controllers controllers;
std::shared_ptr<TuningState> tuning;
std::once_flag dxInitOnce;

// A hard safety limit prevents a malformed/missing MIDI Note Off from leaving
// an oscillator active forever. Normal Note Off still stops it immediately.
constexpr int kInternalVoiceSafetySamples = kSampleRate * 8;
// Removes only the discontinuity when a voice is replaced or retuned (same
// 3 ms smoothstep as the desktop AudioTransition).
struct AnalogTransition {
    float last=0.0f,correction=0.0f; int remaining=0,length=1; bool pending=false;
    void retarget(){length=std::max(1,(int)(kSampleRate*0.003));pending=true;}
    float process(float value){
        if(pending){correction=last-value;remaining=length;pending=false;}
        if(remaining>0){const float t=(float)remaining--/(float)length;value+=correction*t*t*(3.0f-2.0f*t);}
        return last=value;
    }
};
// Same voice model as the desktop "browser 12 dB" Analog engine (approved sound):
// exponential amplitude envelope, Web Audio style 2nd-order low-pass, per-voice LFO.
enum AnalogStage : int { analogAttack=0, analogDecay, analogSustain, analogRelease, analogLegato };
struct AnalogVoice {
    bool active=false,releasing=false; int note=-1,channel=0; int age=0;
    double pitch=69.0;
    float frequency=440.0f,phase1=0.0f,phase2=0.0f,phase3=0.0f,pinkState=0.0f;
    uint32_t noiseState=0x12345678u;
    int ampStage=analogAttack; double ampElapsed=0.0; float ampValue=0.0001f; int releaseElapsed=0;
    double biquadX1=0,biquadX2=0,biquadY1=0,biquadY2=0,lfoPhase=0.0;
    bool legato=false; // mono retune without restarting the envelope (desktop Mono/Legato)
    bool quickRelease=false; // re-struck key under the pedal: fade out in 0.2 s instead of the layer release
    AnalogTransition transition;
};
struct AnalogLayer {
    int preset=0;
    // Same 19 controls, in the order used by the approved desktop editor.
    std::array<float,19> controls {80,55,25,0,-12,0,72,12,35,6,180,78,260,4,0,0,0,0,0};
    float oscillator1Semitones=0.0f;
    std::array<int,3> waves {1,1,0};
    std::array<bool,3> oscillatorEnabled {true,true,true};
    std::array<AnalogVoice,kVoicePoolCapacity> voices {};
    float modWheel=0.0f,lfoPhase=0.0f;
    bool pinkNoise=false,monophonic=false;
    // Last-note priority bookkeeping for the monophonic mode (desktop heldNotes/noteOrder).
    std::array<bool,128> held {};std::array<uint64_t,128> order {};uint64_t sequence=0;
    // Filter ADSR + glide. Defaults reproduce the earlier fixed behaviour exactly:
    // attack 0, sustain 0 and a decay of twice the amp decay (-1 = automatic).
    float filterAttackMs=0.0f,filterDecayMs=-1.0f,filterSustainPercent=0.0f,glideMs=0.0f;
};
std::array<AnalogLayer,kLayerCount> analogLayers {};
constexpr std::array<const char*,32> analogNames {
    "Solo Lead", "Modern Lead", "Classic Minimoog Lead", "Lucky Man", "Unison Lead", "Vintage Lead", "Easy Lead", "Screaming Lead",
    "Waterfall Lead", "Brass Lead", "Taurus Bass", "Funk Bass", "Authentic Minimoog Bass", "Thick Bass", "Analog Bass", "Percussive Bass",
    "Dub Bass", "Pulse Bass", "Crystal Pad", "Dream Pad", "Space Mod", "Atmospheric Pad", "Warm Pad", "Vintage Minimoog Pad",
    "Shimmer Pad", "Velvet Cloud", "Alien Landscape", "Digital Rain", "Submarine Sonar", "Thunder Storm", "Glass Harmonica", "Cosmic Drone"
};
struct HammondVoice {
    bool active=false,down=false,releasing=false;
    int note=-1,channel=0,age=0,releaseAge=0;
    float envelope=0.0f,releaseStart=0.0f,percussion=0.0f,click=0.0f;
    bool percussionSlow=false;
    double percussionPhase=0.0,percussionStep=0.0;
    std::array<double,13> phase{},step{};
};
struct HammondLayer {
    int preset=0, leslie=1, manualLeslie=1, percussion=0; // percussion = harmonic multiplier: 0 off, 2 = 2nd, 3 = 3rd (as on the desktop)
    int vibrato=0; // scanner: 0 off, 1..3 = V1..V3, 4..6 = C1..C3
    bool percussionSoft=false,percussionSlow=false;
    float leslieBalance=.5f; // 0 = drum only, .5 = even, 1 = horn only
    float vibratoDepthSmoothed=0.0f,vibratoWetSmoothed=0.0f,balanceSmoothed=.5f;
    std::array<float,kHammondVibratoFrames> vibratoDelay{};
    int vibratoPosition=0;
    double vibratoPhase=0.0;
    bool modulationWheelOverride=false;
    std::array<float,9> bars{};
    std::array<float,9> smoothedBars{};
    float click=0.15f, leakage=0.12f, drive=0.12f, level=0.8f;
    std::array<HammondVoice,kVoicePoolCapacity> voices{};
    std::array<double,2> rotorPhase{};
    float crossover=0.0f,rotaryDepth=0.0f;
    std::array<std::array<float,kHammondDelayFrames>,2> rotaryDelay{};
    int rotaryDelayPosition=0;
    uint32_t noise=0x1341257u;
};
std::array<HammondLayer,kLayerCount> hammondLayers{};

std::array<int,kLayerCount> calculateLayerVoiceBudgets(
        int overrideLayer = -1, EngineType overrideType = EngineType::empty)
{
    std::array<int,kLayerCount> budgets{};
    std::array<bool,kLayerCount> eligible{};
    int eligibleLayers=0;
    for(int layer=0;layer<kLayerCount;++layer){
        const EngineType type=layer==overrideLayer?overrideType:engineTypes[(size_t)layer];
        eligible[(size_t)layer]=type!=EngineType::empty;
        if(eligible[(size_t)layer])++eligibleLayers;
    }

    // Match the approved build's shared 256-voice pool. SF2 and NPNO piano
    // layers share the same fair quota; lighter oscillator engines remain
    // capped at 32 voices so their unused share can go to sampled instruments.
    int remainingVoices=kMaximumPolyphony;
    while(eligibleLayers>0){
        const int base=remainingVoices/eligibleLayers;
        const int remainder=remainingVoices%eligibleLayers;
        bool cappedAny=false;
        int ordinal=0;
        for(int layer=0;layer<kLayerCount;++layer){
            const auto li=(size_t)layer;
            if(!eligible[li])continue;
            const EngineType type=layer==overrideLayer?overrideType:engineTypes[li];
            const int fairShare=base+(ordinal<remainder?1:0);
            ++ordinal;
            const bool limited=type==EngineType::dx7||type==EngineType::analog||type==EngineType::hammond;
            if(limited&&fairShare>kLowPolyphonyEngineLimit){
                budgets[li]=kLowPolyphonyEngineLimit;
                eligible[li]=false;
                --eligibleLayers;
                remainingVoices-=kLowPolyphonyEngineLimit;
                cappedAny=true;
            }
        }
        if(eligibleLayers==0)break;
        if(cappedAny)continue;

        ordinal=0;
        for(int layer=0;layer<kLayerCount;++layer){
            const auto li=(size_t)layer;
            if(!eligible[li])continue;
            budgets[li]=base+(ordinal<remainder?1:0);
            ++ordinal;
        }
        break;
    }
    return budgets;
}

// Recompute shared per-layer quotas whenever a motor is added or removed.
static std::array<int,kLayerCount> layerPolyphonyCap=[]{std::array<int,kLayerCount> a{};a.fill(128);return a;}();
void distributeLayerVoiceBudgets()
{
    const auto budgets=calculateLayerVoiceBudgets();
    for(int layer=0;layer<kLayerCount;++layer){
        const auto li=(size_t)layer;
        int budget=budgets[li];
        if(layerPolyphonyCap[li]>0)budget=std::min(budget,layerPolyphonyCap[li]);
        if(engineTypes[li]==EngineType::empty){layerVoiceBudgets[li]=0;continue;}
        int effectiveBudget=budget;
        if(engineTypes[li]==EngineType::dx7){
            // Allocate this layer's voices before publishing the DX7 engine;
            // Note On never allocates on the audio thread.
            auto& voices=dxLayers[li].voices;
            for(int i=0;i<budget;++i){
                if(voices[(size_t)i].synth)continue;
                try{voices[(size_t)i].synth=std::make_unique<Dx7Note>(tuning,nullptr);}
                catch(...){effectiveBudget=i;break;}
            }
            for(int i=effectiveBudget;i<kMaximumPolyphony;++i){
                auto& voice=voices[(size_t)i];
                voice.active=false;voice.note=-1;voice.read=N;voice.synth.reset();
            }
        }
        layerVoiceBudgets[li]=effectiveBudget;
        if(engineTypes[li]==EngineType::sf2&&fonts[li]!=nullptr)
            tsf_set_max_voices(fonts[li],effectiveBudget);
        // When another motor is added, trim any now-out-of-budget voices.
        // These fixed pools are preallocated; no allocation occurs here.
        for(int i=budget;i<kMaximumPolyphony;++i){
            auto& dx=dxLayers[li].voices[(size_t)i];dx.active=false;dx.note=-1;dx.read=N;
            analogLayers[li].voices[(size_t)i]={};
            hammondLayers[li].voices[(size_t)i]={};
            nordLayers[li].voices[(size_t)i]={};
            nsmpLayers[li].voices[(size_t)i]={};
        }
    }
}
constexpr std::array<const char*,33> hammondNames { "Jimmy Gospel", "Jazz Ballad", "Rock Organ", "Percussive B3", "Full Drawbar", "Jazz Green Onions", "The House Is Rockin", "The Way It Is", "Whiter Shade", "Gospel Fullness", "Blues 888000000", "Cinema Swell", "Cathedral Soft", "The Ballad", "Soul 888", "Jazz Rotary", "Deep Warmth", "Bright Gospel", "Soft Strings", "Power Chords", "Blues Lead", "The Boxer", "Nights In White", "Church Full", "Gritty Rock", "Funky Clav Organ", "Late Night Jazz", "Slow Leslie", "Fast Leslie", "Percussive Jazz", "Vintage Combo", "Big Theatre", "Minimalist" };
constexpr std::array<std::array<float,9>,33> hammondBars {{
    {8.0f/8.0f,8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,8.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f},
    {8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,5.0f/8.0f,6.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,4.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,7.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,5.0f/8.0f,8.0f/8.0f,7.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,4.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,5.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,5.0f/8.0f,4.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,3.0f/8.0f,7.0f/8.0f,5.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,4.0f/8.0f,6.0f/8.0f,7.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,4.0f/8.0f,2.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,5.0f/8.0f,8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,4.0f/8.0f,2.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,7.0f/8.0f,5.0f/8.0f,3.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,6.0f/8.0f,5.0f/8.0f,4.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,4.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,5.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,2.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,7.0f/8.0f,6.0f/8.0f,8.0f/8.0f,7.0f/8.0f,4.0f/8.0f,2.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,5.0f/8.0f,7.0f/8.0f,6.0f/8.0f,4.0f/8.0f,3.0f/8.0f,1.0f/8.0f,0.0f/8.0f,0.0f/8.0f},
    {8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,8.0f/8.0f,7.0f/8.0f,5.0f/8.0f,3.0f/8.0f,1.0f/8.0f},
    {8.0f/8.0f,0.0f/8.0f,8.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f,0.0f/8.0f}
}};
constexpr std::array<double,13> hammondRatios {.5,1.5,1.,2.,3.,4.,5.,6.,8.,.9438743126816935,1.0594630943592953,1.4983070768766815,2.0};
const std::array<float,4097> hammondSineTable=[] {
    std::array<float,4097> values{};
    for(size_t i=0;i<values.size();++i)values[i]=(float)std::sin(6.283185307179586*(double)i/4096.0);
    return values;
}();
float hammondSine(double phase)
{
    phase-=std::floor(phase);
    const double position=phase*4096.0;
    const auto index=std::min((size_t)position,(size_t)4095);
    const float fraction=(float)(position-(double)index);
    return hammondSineTable[index]+(hammondSineTable[index+1]-hammondSineTable[index])*fraction;
}
void releaseHammondVoice(HammondVoice& voice)
{
    if(!voice.active||voice.releasing)return;
    voice.down=false;voice.releasing=true;voice.releaseAge=0;voice.releaseStart=voice.envelope;
}

// Band-limited oscillators and noise exactly as in the desktop AnalogSynthEngine.
inline float analogWrap(float value){return value-std::floor(value);}
inline float analogNoise(uint32_t& state){
    state=state*1664525u+1013904223u;
    return (float)((state>>8)&0x00ffffffu)/8388607.5f-1.0f;
}
inline float analogPolyBlep(float phase,float increment){
    if(increment<=0.0f)return 0.0f;
    if(phase<increment){const float t=phase/increment;return t+t-t*t-1.0f;}
    if(phase>1.0f-increment){const float t=(phase-1.0f)/increment;return t*t+t+t+1.0f;}
    return 0.0f;
}
// type: 0 triangle, 1 saw, 2 square, 3 pulse, 4 sine (5, an old reverse saw, plays as saw).
inline float analogBlepWave(int type,float phase,float increment){
    constexpr float twoPi=6.28318530718f;
    if(type==1||type==5)phase=analogWrap(phase+0.5f); // Web Audio's saw is centred on the zero crossing
    phase=analogWrap(phase);
    switch(type){
        case 0:return std::asin(std::sin(twoPi*phase))*(2.0f/3.14159265359f);
        case 4:return std::sin(twoPi*phase);
        case 2:{float out=phase<0.5f?1.0f:-1.0f;out+=analogPolyBlep(phase,increment);out-=analogPolyBlep(analogWrap(phase+0.5f),increment);return out;}
        case 3:{constexpr float duty=0.28f;float out=phase<duty?1.0f:-1.0f;out+=analogPolyBlep(phase,increment);out-=analogPolyBlep(analogWrap(phase-duty),increment);return out;}
        default:return 2.0f*phase-1.0f-analogPolyBlep(phase,increment);
    }
}

float analogWave(int type,double phase)
{
    phase-=std::floor(phase);
    switch(type){case 0:return (float)(1.0-4.0*std::abs(phase-.5));case 1:return (float)(2.0*phase-1.0);
        case 2:return phase<.5?1.0f:-1.0f;case 3:return phase<.25?1.0f:-1.0f;case 5:return (float)(1.0-2.0*phase);default:return (float)std::sin(phase*6.28318530718);}
}

void initialiseDx()
{
    std::call_once(dxInitOnce, [] {
        Exp2::init(); Sin::init(); Freqlut::init(kSampleRate); Env::init_sr(kSampleRate);
        PitchEnv::init(kSampleRate); Porta::init_sr(kSampleRate);
        tuning = createStandardTuning();
        controllers.core = &fmCore;
        controllers.refresh();
    });
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
    // A layer unload is also a hard note boundary. Clear per-layer key routing
    // so a later Note Off cannot affect a newly restored motor on the same key.
    for(auto& channel:routedNotes[(size_t)layer])channel.fill(0);
    if (fonts[(size_t) layer] != nullptr)
    {
        tsf_close(fonts[(size_t) layer]);
        fonts[(size_t) layer] = nullptr;
    }
    clearDxLayer(layer);
    analogLayers[(size_t)layer]={};
    hammondLayers[(size_t)layer]={};
    nordLayers[(size_t)layer]={};
    nsmpLayers[(size_t)layer]={};
    layerModulationEnabled[(size_t)layer]=false;
    engineTypes[(size_t)layer] = EngineType::empty;
}

void sendAllNotesOff()
{
    for(auto& channel:sustainDown)channel=false;
    for(auto& channel:physicalKeys)channel.fill(false);
    for(auto& layer:routedNotes)for(auto& channel:layer)channel.fill(0);
    for (int layer=0;layer<kLayerCount;++layer) {
        if(fonts[(size_t)layer]!=nullptr)for(int ch=0;ch<16;++ch){tsf_channel_set_sustain(fonts[(size_t)layer],ch,0);tsf_channel_note_off_all(fonts[(size_t)layer],ch);}
        for(auto& voice:dxLayers[(size_t)layer].voices)if(voice.active&&voice.synth){voice.synth->keyup();voice.released=true;}
        // Panic must be immediate; a lost MIDI note-off must never leave an
        // oscillator running while changing screens/devices.
        for(auto& voice:analogLayers[(size_t)layer].voices)voice={};
        for(auto& voice:hammondLayers[(size_t)layer].voices)voice={};
        for(auto& voice:nordLayers[(size_t)layer].voices)voice={};
        for(auto& voice:nsmpLayers[(size_t)layer].voices)voice={};
        nordLayers[(size_t)layer].pedal.fill(false);nordLayers[(size_t)layer].sostenutoDown.fill(false);
    }
}
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeMidiQueueDiagnostics(JNIEnv* env,jclass)
{
    const jint values[11]={
        publishedMidiQueueDepth.load(std::memory_order_relaxed),
        midiQueueHighWater.load(std::memory_order_relaxed),
        midiQueueOverflows.load(std::memory_order_relaxed),
        midiQueueConsumerLockMisses.load(std::memory_order_relaxed),
        synthRealtimeLockMisses.load(std::memory_order_relaxed),
        midiQueueMaxWaitMicros.load(std::memory_order_relaxed),
        masterClippedBlocks.load(std::memory_order_relaxed),
        nordDiagActive.load(std::memory_order_relaxed),
        nordDiagHeld.load(std::memory_order_relaxed),
        nordDiagWaiting.load(std::memory_order_relaxed),
        nordDiagPedal.load(std::memory_order_relaxed)
    };
    auto result=env->NewIntArray(11);
    if(result)env->SetIntArrayRegion(result,0,11,values);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadLayer(
        JNIEnv* env, jclass, jint layer, jstring path)
{
    if (layer < 0 || layer >= kLayerCount || path == nullptr) return JNI_FALSE;
    const char* utf8Path = env->GetStringUTFChars(path, nullptr);
    if (utf8Path == nullptr) return JNI_FALSE;

    auto* loaded = tsf_load_filename(utf8Path);
    env->ReleaseStringUTFChars(path, utf8Path);
    if (loaded == nullptr) return JNI_FALSE;

    // Keep the SF2 engine close to the desktop reference level while leaving
    // a little headroom for layered chords and PCM output clipping.
    // Four more dB of headroom keeps dense layered chords clean.
    tsf_set_output(loaded, TSF_STEREO_INTERLEAVED, kSampleRate, -6.0f);
    // Reserve the full shared pool before publishing the font. Its active
    // quota is trimmed to the fair layer budget after publication; SF2 voices
    // count sample regions, so layered presets may use several per MIDI key.
    if (!tsf_set_max_voices(loaded, kMaximumPolyphony)) {
        tsf_close(loaded);
        return JNI_FALSE;
    }
    // Materialize every channel before the real-time callback: TinySoundFont
    // may allocate channel state on first use, which must never happen there.
    for(int ch=0;ch<16;++ch){
        if(!tsf_channel_set_presetindex(loaded,ch,0)){
            tsf_close(loaded);
            return JNI_FALSE;
        }
    }
    // File I/O and sample allocation must not block the audio renderer.
    // Publish only a fully prepared font; failed imports preserve the old layer.
    std::lock_guard<std::mutex> lock(synthMutex);
    layerAttack[(size_t)layer]=0.0f;
    layerRelease[(size_t)layer]=0.0f;
    for(int ch=0;ch<16;++ch){tsf_channel_set_amp_envelope_offsets(loaded,ch,0.0f,0.0f);tsf_channel_set_amp_decay(loaded,ch,layerDecayMs[(size_t)layer],layerSustainPercent[(size_t)layer]);}
    releaseLayer(layer);
    fonts[(size_t) layer] = loaded;
    engineTypes[(size_t) layer] = EngineType::sf2;
    distributeLayerVoiceBudgets();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadNord(
        JNIEnv* env,jclass,jint layer,jstring path,jlong budget,jboolean finitePiano)
{
    if(layer<0||layer>=kLayerCount||path==nullptr||budget<=0)
        return env->NewStringUTF("Layer, caminho ou memória NPNO inválidos");
    const char* utf8=env->GetStringUTFChars(path,nullptr);
    if(!utf8)return env->NewStringUTF("Caminho NPNO indisponível");
    std::string file(utf8);env->ReleaseStringUTFChars(path,utf8);
    std::string error;
    try {
        auto instrument=std::make_shared<nord_android::Instrument>();
        if(!nord_android::load(file,(size_t)budget,*instrument,error))
            return env->NewStringUTF(error.c_str());
        std::lock_guard<std::mutex> lock(synthMutex);
        releaseLayer(layer);
        auto& nord=nordLayers[(size_t)layer];
        nord.instrument=std::move(instrument);
        nord.finitePiano=finitePiano==JNI_TRUE;
        nord.eqMode=0;   // SOFT voicing, like the desktop default for new pianos
        nord_android::prepareResonance(nord);
        layerAttack[(size_t)layer]=0.003f;
        // The long piano decay is part of the sample's natural held-note
        // envelope; key release itself must remain short and independent.
        // Desktop layer Release=30 maps to a 300 ms fade-to-silence time.
        layerRelease[(size_t)layer]=nord.finitePiano?0.65f:.01f;
        engineTypes[(size_t)layer]=EngineType::nord;
        distributeLayerVoiceBudgets();
    }catch(const std::bad_alloc&){return env->NewStringUTF("Memória insuficiente para decodificar o NPNO");}
    catch(...){return env->NewStringUTF("Falha inesperada ao decodificar NPNO");}
    return nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadNsmp(
        JNIEnv* env,jclass,jint layer,jstring path,jlong budget)
{
    if(layer<0||layer>=kLayerCount||path==nullptr||budget<=0)
        return env->NewStringUTF("Layer, caminho ou memória NSMP inválidos");
    const char* utf8=env->GetStringUTFChars(path,nullptr);
    if(!utf8)return env->NewStringUTF("Caminho NSMP indisponível");
    std::string file(utf8);env->ReleaseStringUTFChars(path,utf8);
    std::string error;
    try {
        std::shared_ptr<nsmp_android::Instrument> instrument;
        if(!nsmp_android::load(file,(std::uint64_t)budget,instrument,error))
            return env->NewStringUTF(error.c_str());
        std::lock_guard<std::mutex> lock(synthMutex);
        releaseLayer(layer);
        nsmpLayers[(size_t)layer].instrument=std::move(instrument);
        // Desktop defaults for a sample layer: 5 ms attack, Release 30 (300 ms).
        layerAttack[(size_t)layer]=0.005f;
        layerRelease[(size_t)layer]=0.30f;
        engineTypes[(size_t)layer]=EngineType::nsmp;
        distributeLayerVoiceBudgets();
    }catch(const std::bad_alloc&){return env->NewStringUTF("Memória insuficiente para decodificar o NSMP");}
    catch(...){return env->NewStringUTF("Falha inesperada ao decodificar NSMP");}
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadNsmpTestTone(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return;
    auto instrument=nsmp_android::makeTestToneInstrument();
    std::lock_guard<std::mutex> lock(synthMutex);
    releaseLayer(layer);
    nsmpLayers[(size_t)layer].instrument=std::move(instrument);
    layerAttack[(size_t)layer]=0.005f;
    layerRelease[(size_t)layer]=0.30f;
    engineTypes[(size_t)layer]=EngineType::nsmp;
    distributeLayerVoiceBudgets();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNsmpLoadInfo(JNIEnv* env,jclass)
{
    return env->NewStringUTF(nsmp_android::lastLoadInfo().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNordLoadInfo(JNIEnv* env,jclass)
{
    return env->NewStringUTF(nord_android::lastLoadInfo().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetNordEqMode(JNIEnv*,jclass,jint layer,jint mode)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    nordLayers[(size_t)layer].eqMode=std::clamp((int)mode,-1,2);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerPolyphony(JNIEnv*,jclass,jint layer,jint cap)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    layerPolyphonyCap[(size_t)layer]=std::clamp((int)cap,1,128);
    distributeLayerVoiceBudgets();
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetNordOptions(JNIEnv*,jclass,jint layer,jint dynamic,jboolean soft,jboolean reflections,jfloat decaySeconds)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    auto& l=nordLayers[(size_t)layer];
    l.dynamicCompression=std::clamp((int)dynamic,0,3);l.softRelease=soft==JNI_TRUE;l.reflections=reflections==JNI_TRUE;
    l.decaySeconds=std::clamp((float)decaySeconds,3.f,15.f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetNordExtras(JNIEnv*,jclass,jint layer,jboolean longRelease,jboolean stringRes)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    auto& l=nordLayers[(size_t)layer];
    l.longRelease=longRelease==JNI_TRUE;l.stringRes=stringRes==JNI_TRUE;
    if(l.stringRes)nord_android::prepareResonance(l);
}

// mode 0: fade the master out, 1: fade it in, 2: start silent and fade in. The fade lives in the
// process, so it survives the Activity being recreated by a Live Set change.
extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSwitchFade(JNIEnv*,jclass,jint mode,jint milliseconds)
{
    switchFadeMs.store(std::clamp((int)milliseconds,1,2000),std::memory_order_relaxed);
    if(mode==2)switchFadeCurrent=0.0f;
    switchFadeTarget.store(mode==0?0.0f:1.0f,std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativePitchBend(JNIEnv*,jclass,jint value14,jint channel)
{
    if(channel<0||channel>15)return;
    enqueueMidiCommand({MidiCommandType::PitchBend,value14,0,channel});
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetNordVelocityCalibration(
        JNIEnv*,jclass,jint layer,jint soft,jint mid,jint hard)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    if(soft>=1&&mid-soft>=8&&hard-mid>=8&&hard<=127)
        nordVelocityCalibration[(size_t)layer]={(int)soft,(int)mid,(int)hard};
    else nordVelocityCalibration[(size_t)layer]={0,0,0};
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLoadDx7(
        JNIEnv* env, jclass, jint layer, jstring path)
{
    if (layer < 0 || layer >= kLayerCount || path == nullptr) return JNI_FALSE;
    const char* utf8Path = env->GetStringUTFChars(path, nullptr);
    if (utf8Path == nullptr) return JNI_FALSE;
    struct UtfCharsGuard {
        JNIEnv* env;
        jstring path;
        const char* chars;
        void release() {
            if (chars != nullptr) {
                env->ReleaseStringUTFChars(path, chars);
                chars = nullptr;
            }
        }
        ~UtfCharsGuard() { release(); }
    } pathGuard{env, path, utf8Path};
    try {
        std::ifstream input(pathGuard.chars, std::ios::binary);
        pathGuard.release();
        if (!input) return JNI_FALSE;
        input.seekg(0, std::ios::end);
        const std::streamoff fileSize=input.tellg();
        // SysEx voice banks are tiny; reject malformed/unrelated multi-megabyte
        // files before copying them into memory alongside resident SF2 samples.
        constexpr std::streamoff kMaxDx7FileBytes=16*1024*1024;
        if(fileSize<=0||fileSize>kMaxDx7FileBytes)return JNI_FALSE;
        input.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        // A DX7 32-voice bank is normally a 4104-byte SysEx message, but files
        // exported by different editors may contain a leading header, multiple
        // messages, or a trailing checksum/F7. Locate the bulk-data header and
        // only require the 4096 bytes containing the 32 packed voices.
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

        // Build every allocation-prone part before replacing the current
        // layer. Low-memory Android devices may have several large SF2 sample
        // banks resident; no C++ allocation failure may escape through JNI
        // and abort the entire app.
        std::array<std::array<uint8_t,156>,32> preparedPatches{};
        std::array<std::string,32> preparedNames{};
        for (int patch=0; patch<32; ++patch) {
            const auto* packed=bytes.data()+start+patch*128;
            expandDxPatch(packed,preparedPatches[(size_t)patch]);
            preparedNames[(size_t)patch]=dxName(packed+118,10);
            if (preparedNames[(size_t)patch].empty())
                preparedNames[(size_t)patch]="Timbre DX7 " + std::to_string(patch+1);
        }

        // The DX7 tables and voice objects can take longer than one audio
        // buffer to prepare. Do not hold the renderer's mutex while doing so.
        initialiseDx();
        std::array<std::unique_ptr<Dx7Note>,kVoicePoolCapacity> preparedVoices{};
        for(int i=0;i<kLowPolyphonyEngineLimit;++i)
            preparedVoices[(size_t)i]=std::make_unique<Dx7Note>(tuning,nullptr);

        std::lock_guard<std::mutex> lock(synthMutex);
        const int budget=calculateLayerVoiceBudgets(layer,EngineType::dx7)[(size_t)layer];
        releaseLayer(layer);
        auto& dx = dxLayers[(size_t)layer];
        dx.patches=std::move(preparedPatches);
        dx.names=std::move(preparedNames);
        dx.count=32; dx.selected=0;
        // Keep exactly this layer's voice quota prepared before publishing it.
        // Note On only resets an existing object and never allocates on audio thread.
        for(int i=0;i<budget;++i)dx.voices[(size_t)i].synth=std::move(preparedVoices[(size_t)i]);
        engineTypes[(size_t)layer]=EngineType::dx7;
        distributeLayerVoiceBudgets();
        return JNI_TRUE;
    } catch (...) {
        // In particular, contain std::bad_alloc from the DX7 patch/name/voice
        // setup when multiple high-memory SF2 banks are already loaded.
        return JNI_FALSE;
    }
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
    for(auto& voice:dx.voices){
        // The DX7 pool is allocated before any note is played. A fresh
        // Dx7Note has uninitialized envelopes, so keyup() is only valid for
        // voices whose note-on has initialized those envelopes.
        if(voice.active&&voice.synth){voice.synth->keyup();voice.released=true;}
        voice.active=false;voice.note=-1;voice.read=N;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeActivateAnalog(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return; std::lock_guard<std::mutex> lock(synthMutex); releaseLayer(layer); engineTypes[(size_t)layer]=EngineType::analog; distributeLayerVoiceBudgets();
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
    if(layer<0||layer>=kLayerCount||values==nullptr||flags==nullptr||env->GetArrayLength(values)<24||env->GetArrayLength(flags)<8)return;
    jfloat controls[24]{};jint options[8]{};env->GetFloatArrayRegion(values,0,24,controls);env->GetIntArrayRegion(flags,0,8,options);
    std::lock_guard<std::mutex> lock(synthMutex);auto& analog=analogLayers[(size_t)layer];
    constexpr float minimum[19]={0,0,0,-24,-24,0,0,0,0,1,1,0,1,.05f,0,0,0,0,0};
    constexpr float maximum[19]={100,100,100,24,24,100,100,100,100,2000,5000,100,5000,20,12,100,100,100,100};
    for(int i=0;i<19;++i)analog.controls[(size_t)i]=std::clamp((float)controls[i],minimum[i],maximum[i]);
    analog.oscillator1Semitones=std::clamp((float)controls[19],-24.0f,24.0f);
    for(int i=0;i<3;++i)analog.waves[(size_t)i]=std::clamp((int)options[i],0,5);
    for(int i=0;i<3;++i)analog.oscillatorEnabled[(size_t)i]=options[i+3]!=0;
    analog.pinkNoise=options[6]!=0;analog.monophonic=options[7]!=0;
    analog.filterAttackMs=std::clamp((float)controls[20],0.0f,2000.0f);
    analog.filterDecayMs=controls[21]<=0.0f?-1.0f:std::clamp((float)controls[21],1.0f,5000.0f);
    analog.filterSustainPercent=std::clamp((float)controls[22],0.0f,100.0f);
    analog.glideMs=std::clamp((float)controls[23],0.0f,2000.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeActivateHammond(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return; std::lock_guard<std::mutex> lock(synthMutex); releaseLayer(layer); engineTypes[(size_t)layer]=EngineType::hammond; distributeLayerVoiceBudgets();
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
    organ.leslie=preset==28?2:1; organ.manualLeslie=organ.leslie;organ.modulationWheelOverride=false;
    organ.percussion=preset==3||preset==29?3:0;organ.percussionSoft=false;organ.percussionSlow=false; organ.voices={}; return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetHammondControls(JNIEnv* env,jclass,jint layer,jintArray bars,jint leslie,jboolean updateLeslie,jint percussion,jfloat click,jfloat leakage,jfloat drive,jfloat level,jint vibrato,jboolean percussionSoft,jboolean percussionSlow,jfloat leslieBalance)
{
    if(layer<0||layer>=kLayerCount||bars==nullptr||env->GetArrayLength(bars)<9)return;
    jint values[9]{};env->GetIntArrayRegion(bars,0,9,values);std::lock_guard<std::mutex> lock(synthMutex);
    auto& organ=hammondLayers[(size_t)layer];for(int i=0;i<9;++i)organ.bars[(size_t)i]=std::clamp(values[i],0,8)/8.0f;
    if(updateLeslie==JNI_TRUE){organ.leslie=std::clamp((int)leslie,0,2);organ.manualLeslie=organ.leslie;organ.modulationWheelOverride=false;}
    organ.percussion=percussion==2||percussion==3?(int)percussion:0;
    organ.vibrato=std::clamp((int)vibrato,0,6);organ.percussionSoft=percussionSoft==JNI_TRUE;organ.percussionSlow=percussionSlow==JNI_TRUE;
    organ.leslieBalance=std::isfinite((float)leslieBalance)?std::clamp((float)leslieBalance,0.0f,1.0f):.5f;
    organ.click=std::clamp((float)click,0.0f,1.0f);organ.leakage=std::clamp((float)leakage,0.0f,1.0f);
    organ.drive=std::clamp((float)drive,0.0f,1.0f);organ.level=std::clamp((float)level,0.0f,1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeUnloadAll(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(synthMutex);
    {
        std::lock_guard<std::mutex> queueLock(midiQueueMutex);
        midiQueueRead=midiQueueWrite=midiQueueCount=0;
        publishMidiQueueDepth();
    }
    for (int layer = 0; layer < kLayerCount; ++layer) releaseLayer(layer);
    distributeLayerVoiceBudgets();
    layerPeaks.fill(0.0f);
    for(auto& peak:publishedLayerPeaks)peak.store(0.0f,std::memory_order_relaxed);
    masterPeak = 0.0f;
    publishedMasterPeak.store(0.0f,std::memory_order_relaxed);
}
extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeClearLayer(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);releaseLayer(layer);distributeLayerVoiceBudgets();
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
    layerHighNote[i]=std::clamp((int)high,layerLowNote[i],127);layerVelocityCurve[i]=std::clamp((int)velocityCurve,0,4);
    layerSustainEnabled[i]=sustain==JNI_TRUE;layerMidiMode[i]=std::clamp((int)mode,0,2);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerVelocity(JNIEnv*,jclass,jint layer,jint low,jint high,jboolean sensitivityOff,jint outMin,jint outMax,jfloat slope,jint timbreFloor)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;layerVelocityLow[i]=std::clamp((int)low,1,127);layerVelocityHigh[i]=std::clamp((int)high,layerVelocityLow[i],127);
    layerSensitivityOff[i]=sensitivityOff==JNI_TRUE;layerTimbreFloor[i]=std::clamp((int)timbreFloor,1,127);
    buildVelocityTable(i,outMin,outMax,slope);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetVelocityTable(JNIEnv* env,jclass,jint layer,jintArray values)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;
    if(values==nullptr||env->GetArrayLength(values)<128){layerCustomVelocity[i]=false;return;}
    jint raw[128];env->GetIntArrayRegion(values,0,128,raw);
    for(int v=0;v<128;++v)layerVelocityTable[i][(size_t)v]=(uint8_t)std::clamp((int)raw[v],1,127);
    layerCustomVelocity[i]=true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerModulationEnabled(JNIEnv*,jclass,jint layer,jboolean enabled)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;
    layerModulationEnabled[i]=enabled==JNI_TRUE;
    if(layerModulationEnabled[i])return;
    analogLayers[i].modWheel=0.0f;
    if(engineTypes[i]==EngineType::hammond&&hammondLayers[i].modulationWheelOverride){
        hammondLayers[i].leslie=hammondLayers[i].manualLeslie;
        hammondLayers[i].modulationWheelOverride=false;
    }
    if(fonts[i]!=nullptr)for(int channel=0;channel<16;++channel)
        tsf_channel_midi_control(fonts[i],channel,1,0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerEnvelope(JNIEnv*, jclass, jint layer, jfloat attack, jfloat release)
{
    if (layer < 0 || layer >= kLayerCount) return;
    std::lock_guard<std::mutex> lock(synthMutex);
    layerAttack[(size_t)layer] = std::clamp((float)attack, 0.0f, 2.0f);
    layerRelease[(size_t)layer] = std::clamp((float)release, 0.0f, 5.0f);
    if(fonts[(size_t)layer]!=nullptr)for(int channel=0;channel<16;++channel)
        tsf_channel_set_amp_envelope_offsets(fonts[(size_t)layer],channel,
                sf2OriginalSound[(size_t)layer]?0.0f:layerAttack[(size_t)layer],
                sf2OriginalSound[(size_t)layer]?0.0f:layerRelease[(size_t)layer]);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetLayerAdsr(JNIEnv*,jclass,jint layer,jfloat decayMs,jfloat sustainPercent)
{
    if(layer<0||layer>=kLayerCount)return;std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;layerDecayMs[i]=std::clamp((float)decayMs,0.0f,5000.0f);layerSustainPercent[i]=std::clamp((float)sustainPercent,0.0f,100.0f);
    if(fonts[i]!=nullptr)for(int channel=0;channel<16;++channel)tsf_channel_set_amp_decay(fonts[i],channel,layerDecayMs[i],layerSustainPercent[i]);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetSf2OriginalSound(JNIEnv*, jclass, jint layer, jboolean original)
{
    if(layer<0||layer>=kLayerCount)return;
    std::lock_guard<std::mutex> lock(synthMutex);
    const auto i=(size_t)layer;
    sf2OriginalSound[i]=original==JNI_TRUE;
    if(fonts[i]!=nullptr)for(int channel=0;channel<16;++channel)
        tsf_channel_set_amp_envelope_offsets(fonts[i],channel,
                sf2OriginalSound[i]?0.0f:layerAttack[i],
                sf2OriginalSound[i]?0.0f:layerRelease[i]);
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
    float weight=0.0f,sizeSum=0.0f,dampSum=0.0f,widthSum=0.0f;
    for(int n=0;n<kLayerCount;++n){
        const float send=layerReverbSend[(size_t)n];
        if(send<=0.0f)continue;
        weight+=send;sizeSum+=layerReverbSize[(size_t)n]*send;
        dampSum+=layerReverbDamping[(size_t)n]*send;widthSum+=layerReverbWidth[(size_t)n]*send;
    }
    if(weight<=0.0f){weight=1.0f;sizeSum=layerReverbSize[i];dampSum=layerReverbDamping[i];widthSum=layerReverbWidth[i];}
    reverbDelayMs=20.0f+(sizeSum/weight)*1.05f;
    reverbFeedback=0.24f+(dampSum/weight)*0.0034f;
    reverbStereoWidth=widthSum/(weight*100.0f);
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
    for(int ch=0;ch<16;++ch){tsf_channel_note_off_all(font,ch);tsf_channel_set_presetindex(font,ch,preset);}
    return JNI_TRUE;
}

// Desktop Analog (browser-compatible mono): a key pressed while another is held
// retunes the running voice at once and keeps its envelope; releasing the newest
// key returns to the previous held key; only the last release closes the envelope.
static void analogRetune(AnalogLayer& analog,AnalogVoice& voice,int routedNote,int channel,bool glide)
{
    (void)analog;(void)glide; // the desktop Analog has no glide: the retune is immediate
    voice.note=routedNote;voice.channel=channel;voice.releasing=false;voice.legato=true;
    voice.frequency=440.0f*std::pow(2.0f,((float)routedNote-69.0f)/12.0f);
    voice.pitch=routedNote;
    voice.transition.retarget();
}
static void analogNoteOff(size_t li,int routedNote,int channel)
{
    auto& analog=analogLayers[li];
    const bool glide=layerMidiMode[li]==2||analog.glideMs>0.0f;
    if(!analog.monophonic||layerMidiMode[li]!=0){
        for(auto& voice:analog.voices)
            if(voice.active&&voice.note==routedNote&&voice.channel==channel)voice.releasing=true;
        return;
    }
    const int index=std::clamp(routedNote,0,127);
    analog.held[(size_t)index]=false;
    int replacement=-1;uint64_t newest=0;
    for(int candidate=0;candidate<128;++candidate)
        if(analog.held[(size_t)candidate]&&analog.order[(size_t)candidate]>=newest){newest=analog.order[(size_t)candidate];replacement=candidate;}
    AnalogVoice* running=nullptr;
    for(auto& voice:analog.voices)if(voice.active&&!voice.releasing){running=&voice;break;}
    if(replacement>=0){
        if(running&&running->note!=replacement)analogRetune(analog,*running,replacement,channel,glide);
        return;
    }
    for(auto& voice:analog.voices)if(voice.active)voice.releasing=true;
}

static void applyNoteOn(jint note, jint velocity, jint midiChannel)
{
    if (note < 0 || note > 127 || velocity <= 0 || midiChannel < 0 || midiChannel > 15) return;
    physicalKeys[(size_t)midiChannel][(size_t)note] = true;
    for (int layer=0;layer<kLayerCount;++layer) {
        const auto li=(size_t)layer;
        const int voiceBudget=layerVoiceBudgets[li];
        if(voiceBudget<=0)continue;
        if(layerGains[li]<=0.0f&&silentSamples[li]>=kSilentGraceSamples)continue;
        if(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=midiChannel)continue;
        if(note<layerLowNote[li]||note>layerHighNote[li])continue;
        const int routedNote=std::clamp(note+layerOctave[li]*12+(midiChannel==9?0:globalTranspose.load(std::memory_order_relaxed)),0,127);
        if((int)velocity<layerVelocityLow[li]||(int)velocity>layerVelocityHigh[li])continue;
        int routedVelocity=std::clamp((int)velocity,1,127);
        if(layerSensitivityOff[li])routedVelocity=100;
        else if(layerVelocityCurve[li]==4)routedVelocity=layerVelocityTable[li][(size_t)routedVelocity];
        else if(layerVelocityCurve[li]==1)routedVelocity=std::clamp((int)std::lround(std::sqrt(routedVelocity/127.0f)*127.0f),1,127);
        else if(layerVelocityCurve[li]==2)routedVelocity=std::clamp((int)std::lround((routedVelocity/127.0f)*(routedVelocity/127.0f)*127.0f),1,127);
        else if(layerVelocityCurve[li]==3&&engineTypes[li]==EngineType::nord){
            const auto& c=nordVelocityCalibration[li];
            if(c[0]>0){
                const int input[5]={1,c[0],c[1],c[2],127};
                const int output[5]={1,24,64,c[2]>=120?127:112,127};
                for(int point=1;point<5;++point)if(routedVelocity<=input[point]){
                    const float fraction=(float)(routedVelocity-input[point-1])/(float)std::max(1,input[point]-input[point-1]);
                    routedVelocity=std::clamp((int)std::lround(output[point-1]+fraction*(output[point]-output[point-1])),1,127);
                    break;
                }
            }
        }
        routedNotes[li][(size_t)midiChannel][(size_t)note]=routedNote+1;
        double priorAnalogPitch=69.0;bool hasPriorAnalog=false;
        if(layerMidiMode[li]!=0||(engineTypes[li]==EngineType::analog&&analogLayers[li].monophonic)){
            if(engineTypes[li]==EngineType::sf2&&fonts[li]!=nullptr){tsf_channel_note_off_all(fonts[li],midiChannel);routedNotes[li][(size_t)midiChannel].fill(0);}
            for(auto& old:dxLayers[li].voices){
                if(old.active&&old.synth){old.synth->keyup();old.released=true;}
                old.active=false;old.note=-1;old.read=N;
            }
            for(auto& old:hammondLayers[li].voices)old={};
            for(auto& old:nordLayers[li].voices)old={};
            for(auto& old:nsmpLayers[li].voices)old={};
            const bool analogLegato=engineTypes[li]==EngineType::analog&&analogLayers[li].monophonic&&layerMidiMode[li]==0;
            if(!analogLegato)for(auto& old:analogLayers[li].voices)if(old.active){priorAnalogPitch=old.pitch;hasPriorAnalog=true;old={};}
            routedNotes[li][(size_t)midiChannel][(size_t)note]=routedNote+1;
        }
        if (engineTypes[li]==EngineType::sf2 && fonts[li]!=nullptr)
        {
            tsf_channel_set_sustain(fonts[li],midiChannel,layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel]);
            tsf_channel_note_on(fonts[li],midiChannel,routedNote,routedVelocity/127.0f);
        }
        else if(engineTypes[(size_t)layer]==EngineType::dx7) {
            auto& dx=dxLayers[(size_t)layer]; DxVoice* target=nullptr;
            // Re-striking a note that still sounds (pedal, long release) never stacks a second copy.
            for(int i=0;i<voiceBudget;++i){auto& old=dx.voices[(size_t)i];
                    if(old.active&&!old.fading&&!old.released&&old.note==routedNote&&old.channel==midiChannel&&old.synth){
                        old.fading=true;old.fadeGain=1.0f;if(!old.released){old.synth->keyup();old.released=true;}
                    }}
            for(int i=0;i<voiceBudget;++i)if(!dx.voices[(size_t)i].active){target=&dx.voices[(size_t)i];break;}
            if(target==nullptr){target=&dx.voices[0];for(int i=1;i<voiceBudget;++i){auto& voice=dx.voices[(size_t)i];const bool releasing=voice.released||voice.fading,best=target->released||target->fading;if((releasing&&!best)||(releasing==best&&voice.serial<target->serial))target=&voice;}}
            target->serial=++dx.serial;
            target->active=true; target->note=routedNote; target->channel=midiChannel; target->read=N; target->samples.fill(0); target->released=false; target->decayGain=1.0f; target->age=0; target->fading=false; target->fadeGain=1.0f; target->attackGain=layerAttack[li]>0.0005f?0.0f:1.0f; target->releaseGain=1.0f;
            if(!target->synth){target->active=false;continue;}
            target->synth->init(dx.patches[(size_t)dx.selected].data(),routedNote,routedVelocity,1,&controllers);
        }
        else if(engineTypes[li]==EngineType::nsmp) {
            nsmp_android::noteOn(nsmpLayers[li],routedNote,routedVelocity,midiChannel,
                    layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel],
                    voiceBudget,layerAttack[li],layerRelease[li]);
        }
        else if(engineTypes[li]==EngineType::nord) {
            nord_android::noteOn(nordLayers[li],routedNote,routedVelocity,midiChannel,
                    layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel],
                    voiceBudget,layerAttack[li],layerTimbreFloor[li]);
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            auto& analog=analogLayers[(size_t)layer]; AnalogVoice* target=nullptr;float previousAnalogSample=0.0f;
            if(analog.monophonic&&layerMidiMode[li]==0){
                const int index=std::clamp(routedNote,0,127);
                AnalogVoice* running=nullptr;
                for(auto& voice:analog.voices)if(voice.active&&!voice.releasing){running=&voice;break;}
                if(!running){
                    analog.held.fill(false);
                    // A tail from the previous note is cut, like the desktop's single mono voice;
                    // its last sample is remembered so the new note does not click.
                    for(auto& voice:analog.voices)if(voice.active){previousAnalogSample=voice.transition.last;voice={};}
                }
                analog.held[(size_t)index]=true;analog.order[(size_t)index]=++analog.sequence;
                if(running){
                    // Desktop treats the first 80 ms as one simultaneous chord: keep the voice as is.
                    if(running->age>=(int)(kSampleRate*0.08))
                        analogRetune(analog,*running,routedNote,midiChannel,analog.glideMs>0.0f);
                    continue;
                }
            }
            if(layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel])
                for(int i=0;i<voiceBudget;++i){auto& old=analog.voices[(size_t)i];
                    if(old.active&&!old.releasing&&old.note==routedNote&&old.channel==midiChannel){old.releasing=true;old.quickRelease=true;}}
            for(int i=0;i<voiceBudget;++i)if(!analog.voices[(size_t)i].active){target=&analog.voices[(size_t)i];break;}
            if(target==nullptr){target=&analog.voices[0];for(int i=1;i<voiceBudget;++i){auto& voice=analog.voices[(size_t)i];if((voice.releasing&&!target->releasing)||(voice.releasing==target->releasing&&voice.age>target->age))target=&voice;}}
            *target={}; target->active=true; target->note=routedNote; target->channel=midiChannel;
            (void)priorAnalogPitch;(void)hasPriorAnalog; // no glide in the desktop Analog
            target->pitch=routedNote;
            target->frequency=440.0f*std::pow(2.0f,((float)routedNote-69.0f)/12.0f);
            target->noiseState=0x12345678u^(uint32_t)(routedNote*1664525);
            target->ampStage=analogAttack;target->ampValue=0.0001f;target->ampElapsed=0.0;
            target->transition.last=previousAnalogSample;target->transition.retarget();
        }
        else if(engineTypes[(size_t)layer]==EngineType::hammond) {
            auto& organ=hammondLayers[(size_t)layer];
            bool first=true;
            for(int i=0;i<voiceBudget;++i){auto& voice=organ.voices[(size_t)i];if(voice.active){
                if(voice.down)first=false;
                if(voice.note==routedNote&&voice.channel==midiChannel)releaseHammondVoice(voice);
            }}
            HammondVoice* target=nullptr;
            for(int i=0;i<voiceBudget;++i)if(!organ.voices[(size_t)i].active){target=&organ.voices[(size_t)i];break;}
            if(target==nullptr){
                target=&organ.voices.front();
                for(int i=1;i<voiceBudget;++i){auto& voice=organ.voices[(size_t)i];if((voice.releasing&&!target->releasing)||(voice.releasing==target->releasing&&voice.envelope<target->envelope))target=&voice;}
            }
            *target={};target->active=target->down=true;target->note=routedNote;target->channel=midiChannel;
            const double base=440.0*std::pow(2.0,((double)routedNote-69.0)/12.0);
            const double highest=std::min(440.0*std::pow(2.0,(114.0-69.0)/12.0),kSampleRate*.45);
            const double lowest=440.0*std::pow(2.0,(24.0-69.0)/12.0);
            for(size_t partial=0;partial<hammondRatios.size();++partial){
                double frequency=base*hammondRatios[partial];
                while(frequency>highest)frequency*=.5;
                while(frequency<lowest)frequency*=2.0;
                target->step[partial]=frequency/kSampleRate;
            }
            if(first&&organ.percussion>0){
                target->percussion=organ.percussionSoft?.072f:.12f;
                target->percussionSlow=organ.percussionSlow;
                target->percussionStep=std::min(base*organ.percussion,kSampleRate*.45)/kSampleRate;
            }
            target->click=organ.click*.06f;
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOn(JNIEnv*,jclass,jint note,jint velocity,jint channel)
{
    if(note<0||note>127||velocity<=0||channel<0||channel>15)return;
    enqueueMidiCommand({MidiCommandType::NoteOn,note,velocity,channel});
}

static void applyNoteOff(jint note, jint midiChannel)
{
    if (note < 0 || note > 127 || midiChannel < 0 || midiChannel > 15) return;
    physicalKeys[(size_t)midiChannel][(size_t)note] = false;
    for (int layer=0;layer<kLayerCount;++layer) {
        const auto li=(size_t)layer;const auto type = engineTypes[li];
        const int encoded=routedNotes[li][(size_t)midiChannel][(size_t)note];
        if(encoded==0)continue;const int routedNote=encoded-1;
        // TSF owns its pedal-held voices. Deliver every key-up, including
        // repeated pitches; deferring here loses all but one retrigger.
        if(type==EngineType::sf2&&fonts[li]!=nullptr){
            tsf_channel_note_off(fonts[li],midiChannel,routedNote);
            routedNotes[li][(size_t)midiChannel][(size_t)note]=0;
            continue;
        }
        if(layerSustainEnabled[li]&&sustainDown[(size_t)midiChannel]){
            if(type==EngineType::nord)
                nord_android::noteOff(nordLayers[li],routedNote,midiChannel,true,layerRelease[li]);
            if(type==EngineType::nsmp)
                nsmp_android::noteOff(nsmpLayers[li],routedNote,midiChannel,true,layerRelease[li]);
            if(type==EngineType::hammond)
                for(auto& voice:hammondLayers[li].voices)
                    if(voice.active&&voice.note==routedNote&&voice.channel==midiChannel)voice.down=false;
            continue;
        }
        // Retain routing while the pedal holds a released key, so pedal-up
        // can deliver Note Off to the original transposed note and engine.
        routedNotes[li][(size_t)midiChannel][(size_t)note]=0;
        // Each engine gets its own dispatch path. Do not let a loaded font or
        // another engine's state suppress the custom-engine Note Off.
        if (type == EngineType::sf2 && fonts[li] != nullptr)
            tsf_channel_note_off(fonts[li], 0, routedNote);
        if (type == EngineType::dx7)
            for (auto& voice: dxLayers[li].voices)
                if (voice.active && voice.note == routedNote && voice.channel==midiChannel && voice.synth) {voice.synth->keyup();voice.released=true;}
        // With no sustain hold, close the custom-engine envelope normally.
        if (type == EngineType::analog) analogNoteOff(li,routedNote,midiChannel);
        if (type == EngineType::hammond)
            for (auto& voice: hammondLayers[(size_t)layer].voices)
                if (voice.active && voice.note == routedNote && voice.channel==midiChannel) releaseHammondVoice(voice);
        if (type == EngineType::nord)
            nord_android::noteOff(nordLayers[li],routedNote,midiChannel,false,layerRelease[li]);
        if (type == EngineType::nsmp)
            nsmp_android::noteOff(nsmpLayers[li],routedNote,midiChannel,false,layerRelease[li]);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeNoteOff(JNIEnv*,jclass,jint note,jint channel)
{
    if(note<0||note>127||channel<0||channel>15)return;
    enqueueMidiCommand({MidiCommandType::NoteOff,note,0,channel});
}

static void applyControl(jint controller, jint value, jint midiChannel)
{
    if(midiChannel<0||midiChannel>15)return;
    if((controller&0x7f)==1){
        const float wheel=std::clamp((float)(value&0x7f)/127.0f,0.0f,1.0f);
        for(int layer=0;layer<kLayerCount;++layer){
            const auto li=(size_t)layer;
            if(!layerModulationEnabled[li]
                    ||(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=midiChannel))continue;
            if(engineTypes[li]==EngineType::analog)analogLayers[li].modWheel=wheel;
            else if(engineTypes[li]==EngineType::hammond){
                auto& organ=hammondLayers[li];
                organ.leslie=(value&0x7f)>=64?2:1;
                organ.modulationWheelOverride=true;
            }
        }
    }
    if(const int cc=controller&0x7f;cc==66||cc==67||cc==7||cc==11){
        for(int layer=0;layer<kLayerCount;++layer){
            const auto li=(size_t)layer;
            if(engineTypes[li]!=EngineType::nord||(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=midiChannel))continue;
            nord_android::control(nordLayers[li],midiChannel,cc,value&0x7f);
        }
    }
    if ((controller & 0x7f) == 64) {
        for(int layer=0;layer<kLayerCount;++layer)if(engineTypes[(size_t)layer]==EngineType::nord)
            nordLayers[(size_t)layer].pedal[(size_t)midiChannel]=((value&0x7f)>=64)!=sustainInverted.load(std::memory_order_relaxed)&&layerSustainEnabled[(size_t)layer];
        const bool wasDown = sustainDown[(size_t)midiChannel];
        sustainDown[(size_t)midiChannel] = ((value & 0x7f) >= 64) != sustainInverted.load(std::memory_order_relaxed);
        if (wasDown && !sustainDown[(size_t)midiChannel]) {
            // Match the desktop sample engine: release only NPNO voices that
            // were explicitly marked as waiting for this pedal-up. Do this
            // once per layer, not once per MIDI note; still-held/retriggered
            // voices must keep their own key-down state.
            for (int layer=0;layer<kLayerCount;++layer) {
                const auto li=(size_t)layer;
                if(!layerSustainEnabled[li]
                        ||(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=midiChannel))continue;
                if(engineTypes[li]==EngineType::nord)
                    nord_android::pedalUp(nordLayers[li],midiChannel,layerRelease[li]);
                if(engineTypes[li]==EngineType::nsmp)
                    nsmp_android::pedalUp(nsmpLayers[li],midiChannel,layerRelease[li]);
            }
            for (int note = 0; note < 128; ++note) if (!physicalKeys[(size_t)midiChannel][(size_t)note]) {
                for (int layer=0; layer<kLayerCount; ++layer) {
                    const auto li=(size_t)layer;
                    if(!layerSustainEnabled[li])continue;
                    const int encoded=routedNotes[li][(size_t)midiChannel][(size_t)note];if(encoded==0)continue;
                    const int routedNote=encoded-1;routedNotes[li][(size_t)midiChannel][(size_t)note]=0;
                    // NPNO was handled by its per-voice sustain flags; SF2
                    // handles CC64 internally. Never synthesize a second
                    // Note Off here for either sample engine.
                    if(engineTypes[li]==EngineType::nord||engineTypes[li]==EngineType::nsmp||engineTypes[li]==EngineType::sf2)continue;
                    if (engineTypes[li] == EngineType::analog) {
                        analogNoteOff(li,routedNote,midiChannel);
                    } else if (engineTypes[li] == EngineType::hammond) {
                        for (auto& voice: hammondLayers[li].voices)
                            if (voice.active && voice.note == routedNote && voice.channel==midiChannel) releaseHammondVoice(voice);
                    } else if (engineTypes[li] == EngineType::dx7) {
                        for (auto& voice: dxLayers[li].voices)
                            if (voice.active && voice.note == routedNote && voice.channel==midiChannel && voice.synth) {voice.synth->keyup();voice.released=true;}
                    }
                }
            }
        }
    }
    for (int layer=0;layer<kLayerCount;++layer)if(fonts[(size_t)layer]!=nullptr&&
        (layerMidiChannel[(size_t)layer]<0||layerMidiChannel[(size_t)layer]==midiChannel)){
        const auto li=(size_t)layer;
        const int cc=controller&0x7f;
        const int routedValue=(cc==64&&!layerSustainEnabled[li])||
                (cc==1&&!layerModulationEnabled[li])?0:value&0x7f;
        tsf_channel_midi_control(fonts[li],midiChannel,cc,routedValue);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeControl(JNIEnv*,jclass,jint controller,jint value,jint channel)
{
    if(channel<0||channel>15)return;
    enqueueMidiCommand({MidiCommandType::Control,controller,value,channel});
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetGlobalTranspose(JNIEnv*,jclass,jint semitones)
{
    globalTranspose.store(std::clamp((int)semitones,-12,12),std::memory_order_relaxed);
    enqueueMidiCommand({MidiCommandType::AllNotesOff,0,0,0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetSustainInverted(JNIEnv*,jclass,jboolean inverted)
{
    sustainInverted.store(inverted==JNI_TRUE,std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeAllNotesOff(JNIEnv*, jclass)
{
    enqueueMidiCommand({MidiCommandType::AllNotesOff,0,0,0});
}

static void applyMidiCommand(const MidiCommand& command)
{
    switch(command.type){
        case MidiCommandType::NoteOn: applyNoteOn(command.value1,command.value2,command.channel); break;
        case MidiCommandType::NoteOff: applyNoteOff(command.value1,command.channel); break;
        case MidiCommandType::Control: applyControl(command.value1,command.value2,command.channel); break;
        case MidiCommandType::AllNotesOff: sendAllNotesOff(); break;
        case MidiCommandType::PitchBend:
            for(int layer=0;layer<kLayerCount;++layer){
                const auto li=(size_t)layer;
                if(engineTypes[li]!=EngineType::nord||(layerMidiChannel[li]>=0&&layerMidiChannel[li]!=command.channel))continue;
                nord_android::pitchBend(nordLayers[li],command.channel,command.value1);
            }
            break;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeActiveVoices(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return 0;
    std::lock_guard<std::mutex> lock(synthMutex);
    int count=0;
    if(engineTypes[layer]==EngineType::sf2&&fonts[layer])return tsf_active_voice_count(fonts[layer]);
    if(engineTypes[layer]==EngineType::dx7)for(auto& voice:dxLayers[layer].voices)if(voice.active)++count;
    if(engineTypes[layer]==EngineType::analog)for(auto& voice:analogLayers[layer].voices)if(voice.active)++count;
    if(engineTypes[layer]==EngineType::hammond)for(auto& voice:hammondLayers[layer].voices)if(voice.active)++count;
    if(engineTypes[layer]==EngineType::nord)return nord_android::activeVoices(nordLayers[(size_t)layer]);
    if(engineTypes[layer]==EngineType::nsmp)return nsmp_android::activeVoices(nsmpLayers[(size_t)layer]);
    return count;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeVoiceBudget(JNIEnv*,jclass,jint layer)
{
    if(layer<0||layer>=kLayerCount)return 0;
    std::lock_guard<std::mutex> lock(synthMutex);
    return layerVoiceBudgets[(size_t)layer];
}

// Audio callback load: time spent rendering a block as a share of the block's
// own duration (100% = the callback uses its whole budget and will underrun).
static std::atomic<float> renderLoadAverage{0.0f},renderLoadPeak{0.0f};
static bool renderClassicPlayerPcmImpl(int16_t* output,int frames,bool realtime);
bool renderClassicPlayerPcm(int16_t* output,int frames,bool realtime)
{
    const auto begin=std::chrono::steady_clock::now();
    const bool rendered=renderClassicPlayerPcmImpl(output,frames,realtime);
    if(rendered&&frames>0){
        const double spent=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        const double budget=1000.0*std::min(frames,kMaxFrames)/kSampleRate;
        const float percent=(float)(100.0*spent/budget);
        renderLoadAverage.store(renderLoadAverage.load(std::memory_order_relaxed)*0.97f+percent*0.03f,std::memory_order_relaxed);
        renderLoadPeak.store(std::max(percent,renderLoadPeak.load(std::memory_order_relaxed)*0.998f),std::memory_order_relaxed);
    }
    return rendered;
}
extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeRenderLoadAverage(JNIEnv*,jclass){return renderLoadAverage.load(std::memory_order_relaxed);}
extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeRenderLoadPeak(JNIEnv*,jclass){return renderLoadPeak.load(std::memory_order_relaxed);}

static bool renderClassicPlayerPcmImpl(int16_t* output,int frames,bool realtime)
{
    if (output == nullptr || frames <= 0) return false;
    frames = std::min(frames, kMaxFrames);
    const auto samples = frames * 2;

    // The callback never waits behind UI/settings operations. MIDI producers
    // publish short commands separately; apply them only after the audio thread
    // owns synth state, avoiding the old try-lock silence on every Note On/Off.
    std::unique_lock<std::mutex> lock(synthMutex,std::defer_lock);
    if(realtime){if(!lock.try_lock()){
        synthRealtimeLockMisses.fetch_add(1,std::memory_order_relaxed);
        return false;
    }}else lock.lock();
    std::array<MidiCommand,64> commands{};
    const size_t commandCount=takeMidiCommands(commands.data(),commands.size());
    for(size_t i=0;i<commandCount;++i)applyMidiCommand(commands[i]);
    {   // Program switch fade (TEMPO DA TROCA): the master ramps down before a Live Set change and up after it.
        const float fadeTarget=switchFadeTarget.load(std::memory_order_relaxed);
        const float fadeStep=(float)frames/std::max(1.0f,(float)kSampleRate*(float)std::max(1,switchFadeMs.load(std::memory_order_relaxed))*0.001f);
        switchFadeCurrent+=std::clamp(fadeTarget-switchFadeCurrent,-fadeStep,fadeStep);
        blockFade=switchFadeCurrent;
    }
    std::array<float, kLayerCount> renderedPeaks {};
    // These arrays have room for the largest supported callback, but the
    // current block is normally much shorter. Clear only the region used by
    // this block; clearing all 2048 frames for each USB sub-block wasted CPU.
    std::array<float, kMaxFrames * 2> mix;
    std::array<float, kMaxFrames * 2> reverbSend;
    std::array<float,kMaxFrames> hammondSide;
    std::fill_n(mix.data(),samples,0.0f);
    std::fill_n(reverbSend.data(),samples,0.0f);
    for (int layer = 0; layer < kLayerCount; ++layer)
    {
        const auto li=(size_t)layer;
        auto* font = fonts[(size_t) layer];
        if (engineTypes[(size_t)layer]==EngineType::empty) continue;
        {
            const bool silentLayer=layerGains[li]<=0.0f&&smoothedLayerGains[li]<=0.00001f;
            if(!silentLayer)silentSamples[li]=0;
            else{
                if(silentSamples[li]==0){
                    // Start of the silence: release what is sounding so the tails end inside the grace window.
                    if(font!=nullptr)tsf_note_off_all(font);
                    for(auto& old:nordLayers[li].voices)old={};
                    for(auto& old:nsmpLayers[li].voices)old={};
                    for(auto& old:analogLayers[li].voices)old={};
                    for(auto& old:hammondLayers[li].voices)old={};
                    for(auto& old:dxLayers[li].voices){
                        if(old.active&&old.synth){old.synth->keyup();old.released=true;}
                        old.active=false;old.note=-1;old.read=N;
                    }
                }
                if(silentSamples[li]>=kSilentGraceSamples)continue;
                silentSamples[li]+=std::max(frames,1);
            }
        }
        const bool originalSf2=engineTypes[li]==EngineType::sf2&&sf2OriginalSound[li];
        std::memset(scratch.data(), 0, (size_t) samples * sizeof(float));
        if(engineTypes[(size_t)layer]==EngineType::sf2 && font!=nullptr) tsf_render_float(font, scratch.data(), frames, TSF_FALSE);
        else if(engineTypes[li]==EngineType::nsmp){
            nsmp_android::render(nsmpLayers[li],scratch.data(),frames,kSampleRate,
                    layerVoiceBudgets[li],layerDecayMs[li],layerSustainPercent[li]);
        }
        else if(engineTypes[li]==EngineType::nord){
            nord_android::render(nordLayers[li],scratch.data(),frames,kSampleRate,
                    layerVoiceBudgets[li],layerAttack[li],layerDecayMs[li],layerSustainPercent[li]);
            int active=0,held=0,waiting=0;
            for(const auto& v:nordLayers[li].voices)if(v.active){++active;if(v.keyDown)++held;if(v.waitingForSustain)++waiting;}
            nordDiagActive.store(active,std::memory_order_relaxed);nordDiagHeld.store(held,std::memory_order_relaxed);
            nordDiagWaiting.store(waiting,std::memory_order_relaxed);nordDiagPedal.store(sustainDown[0]?1:0,std::memory_order_relaxed);
        }
        else if(engineTypes[(size_t)layer]==EngineType::dx7) {
            auto& dx=dxLayers[(size_t)layer];
            // Traverse the voice pool once per block instead of once per
            // sample. Mix in the same voice order and preserve MSFA's N-frame
            // buffering, including envelope termination at compute boundaries.
            const float dxDecayTarget=std::clamp(layerSustainPercent[li]*.01f,0.0f,1.0f);
            const float dxDecayStep=layerDecayMs[li]>0.0f?(1.0f-dxDecayTarget)/std::max(1.0f,layerDecayMs[li]*.001f*kSampleRate):0.0f;
            const uint64_t dxAttackSamples=(uint64_t)std::max(0.0f,layerAttack[li]*kSampleRate);
            const float dxFadeCoefficient=std::exp(std::log(0.0001f)/(0.2f*kSampleRate)); // -80 dB in 0.2 s
            const float dxReleaseCoefficient=layerRelease[li]>0.001f?std::exp(std::log(0.0001f)/(layerRelease[li]*kSampleRate)):1.0f;
            const float dxAttackStep=layerAttack[li]>0.0005f?1.0f/(layerAttack[li]*kSampleRate):1.0f;
            for(int voiceIndex=0;voiceIndex<layerVoiceBudgets[li];++voiceIndex) {
                auto& voice=dx.voices[(size_t)voiceIndex];
                if(!voice.active||!voice.synth)continue;
                for(int sample=0;sample<frames;++sample) {
                    if(voice.read>=N) {
                        if(!voice.synth->isPlaying()){voice.active=false;voice.read=N;break;}
                        voice.samples.fill(0); voice.synth->compute(voice.samples.data(),1<<23,1<<24,&controllers); voice.read=0;
                    }
                    if(dxDecayStep>0.0f&&!voice.released&&voice.age>=dxAttackSamples)voice.decayGain=std::max(dxDecayTarget,voice.decayGain-dxDecayStep);
                    ++voice.age;
                    if(voice.fading){
                        voice.fadeGain*=dxFadeCoefficient;
                        if(voice.fadeGain<0.0001f){voice.active=false;voice.read=N;voice.fading=false;break;}
                    }
                    voice.attackGain=std::min(1.0f,voice.attackGain+dxAttackStep);
                    if(voice.released&&dxReleaseCoefficient<1.0f){voice.releaseGain*=dxReleaseCoefficient;if(voice.releaseGain<0.0001f){voice.active=false;voice.read=N;voice.releaseGain=1.0f;break;}}
                    const float value=(float)voice.samples[(size_t)voice.read++]/(float)(1<<24)*0.024f*voice.decayGain*voice.fadeGain*voice.attackGain*voice.releaseGain;
                    scratch[(size_t)sample*2]+=value;
                }
            }
            for(int sample=0;sample<frames;++sample) scratch[(size_t)sample*2+1]=scratch[(size_t)sample*2];
        }
        else if(engineTypes[(size_t)layer]==EngineType::hammond) {
            auto& organ=hammondLayers[(size_t)layer];
            const int attackSamples=std::max(1,(int)(layerAttack[li]*kSampleRate));
            const int releaseSamples=std::max(1,(int)(std::max(15.0f,layerRelease[li]*1000.0f)*.001f*kSampleRate));
            const float barSmoothing=1.0f-std::exp(-1.0f/(.015f*kSampleRate));
            const float percussionDecay=std::exp(std::log(.0001f/.12f)/(.08f*kSampleRate));
            const float percussionDecaySlow=std::exp(std::log(.0001f/.12f)/(.28f*kSampleRate));
            static constexpr float vibratoDepthMs[3]={.12f,.25f,.40f};
            const float vibratoDepthTarget=organ.vibrato==0?0.0f:vibratoDepthMs[(organ.vibrato-1)%3];
            const float vibratoWetTarget=organ.vibrato==0?0.0f:organ.vibrato<=3?1.0f:.5f;
            const float clickDecay=std::exp(-8.0f/(kSampleRate*.015f));
            const float crossoverCoefficient=1.0f-std::exp(-6.2831853f*800.0f/kSampleRate);
            const float rotaryDepthSmoothing=1.0f-std::exp(-1.0f/(.02f*kSampleRate));
            const float drive=std::clamp(organ.drive,0.0f,1.0f);
            std::array<float,9> barTargets{};
            for(size_t d=0;d<barTargets.size();++d){
                const float drawbar=(d==8&&organ.percussion>0)?0.0f:organ.bars[d]*8.0f;
                barTargets[d]=drawbar>0.0f?.085f*std::pow(10.0f,(drawbar-8.0f)*3.0f/20.0f):0.0f;
            }
            for(int sample=0;sample<frames;++sample){
                for(size_t d=0;d<organ.smoothedBars.size();++d)
                    organ.smoothedBars[d]+=(barTargets[d]-organ.smoothedBars[d])*barSmoothing;
                float value=0.0f;
                for(int voiceIndex=0;voiceIndex<layerVoiceBudgets[li];++voiceIndex){
                    auto& voice=organ.voices[(size_t)voiceIndex];
                    if(!voice.active)continue;
                    if(voice.releasing){
                        if(++voice.releaseAge>=releaseSamples){voice={};continue;}
                        const float phase=(float)voice.releaseAge/(float)releaseSamples;
                        voice.envelope=voice.releaseStart*.5f*(1.0f+std::cos(3.14159265359f*phase));
                    }else{
                        if(++voice.age>kInternalVoiceSafetySamples){voice={};continue;}
                        voice.envelope=.24f*std::min(1.0f,(float)voice.age/(float)attackSamples);
                    }
                    float tone=0.0f;
                    for(size_t partial=0;partial<hammondRatios.size();++partial){
                        float amplitude=organ.leakage*.002f;
                        if(partial<9)amplitude=organ.smoothedBars[partial];
                        voice.phase[partial]+=voice.step[partial];
                        voice.phase[partial]-=std::floor(voice.phase[partial]);
                        tone+=hammondSine(voice.phase[partial])*amplitude;
                    }
                    if(voice.percussion>1.0e-6f){
                        tone+=hammondSine(voice.percussionPhase)*voice.percussion;
                        voice.percussionPhase+=voice.percussionStep;
                        voice.percussionPhase-=std::floor(voice.percussionPhase);
                        voice.percussion*=voice.percussionSlow?percussionDecaySlow:percussionDecay;
                    }
                    if(voice.click>1.0e-7f){
                        organ.noise=organ.noise*1664525u+1013904223u;
                        tone+=((float)(organ.noise>>8)/8388608.0f-1.0f)*voice.click;
                        voice.click*=clickDecay;
                    }
                    value+=tone*voice.envelope;
                }
                value=value*(1.0f-drive)+std::tanh(2.0f*value*(1.0f+drive*8.0f))/std::tanh(2.0f)*drive/(1.0f+drive*2.0f);
                value*=organ.level;
                {
                    // Scanner vibrato/chorus (same model as the desktop engine).
                    organ.vibratoDepthSmoothed+=(vibratoDepthTarget-organ.vibratoDepthSmoothed)*rotaryDepthSmoothing;
                    organ.vibratoWetSmoothed+=(vibratoWetTarget-organ.vibratoWetSmoothed)*rotaryDepthSmoothing;
                    organ.vibratoDelay[(size_t)organ.vibratoPosition]=value;
                    organ.vibratoPhase+=6.9/kSampleRate;organ.vibratoPhase-=std::floor(organ.vibratoPhase);
                    const float wet=organ.vibratoWetSmoothed;
                    if(wet>.0001f){
                        const double offset=(.0012+.001*organ.vibratoDepthSmoothed*(1.0+hammondSine(organ.vibratoPhase)))*kSampleRate;
                        const int whole=(int)std::floor(offset),size=kHammondVibratoFrames;
                        const int a=(organ.vibratoPosition-whole+size)%size,b=(a+size-1)%size;
                        const float fraction=(float)(offset-whole);
                        const float modulated=organ.vibratoDelay[(size_t)a]+fraction*(organ.vibratoDelay[(size_t)b]-organ.vibratoDelay[(size_t)a]);
                        value=value*(1.0f-wet)+modulated*wet;
                    }
                    organ.vibratoPosition=(organ.vibratoPosition+1)%kHammondVibratoFrames;
                }
                organ.balanceSmoothed+=(organ.leslieBalance-organ.balanceSmoothed)*rotaryDepthSmoothing;
                const float bandGain[2]={std::min(2.0f,2.0f*(1.0f-organ.balanceSmoothed)),std::min(2.0f,2.0f*organ.balanceSmoothed)};
                organ.crossover+=crossoverCoefficient*(value-organ.crossover);
                const float bands[2]={organ.crossover,value-organ.crossover};
                const float targetDepth=organ.leslie>0?1.0f:0.0f;
                organ.rotaryDepth+=(targetDepth-organ.rotaryDepth)*rotaryDepthSmoothing;
                float left=0.0f,right=0.0f;
                for(int rotor=0;rotor<2;++rotor){
                    const double rate=organ.leslie==2?(rotor?6.2:5.4):organ.leslie==1?(rotor?.8:.65):0.0;
                    organ.rotorPhase[(size_t)rotor]+=rate/kSampleRate;
                    organ.rotorPhase[(size_t)rotor]-=std::floor(organ.rotorPhase[(size_t)rotor]);
                    const float modulation=hammondSine(organ.rotorPhase[(size_t)rotor])*organ.rotaryDepth;
                    auto& delay=organ.rotaryDelay[(size_t)rotor];
                    delay[(size_t)organ.rotaryDelayPosition]=bands[rotor];
                    const double offset=(.008+modulation*(rotor?.00045:.00015))*kSampleRate;
                    const int whole=(int)std::floor(offset),size=(int)delay.size();
                    const int a=(organ.rotaryDelayPosition-whole+size)%size,b=(a+size-1)%size;
                    const float fraction=(float)(offset-whole);
                    const float rotated=(delay[(size_t)a]+fraction*(delay[(size_t)b]-delay[(size_t)a]))
                            *(.7f+modulation*(rotor?.12f:.08f))*bandGain[rotor];
                    const float pan=modulation*(rotor?.65f:.4f);
                    left+=rotated*std::sqrt(.5f*(1.0f-pan));
                    right+=rotated*std::sqrt(.5f*(1.0f+pan));
                }
                organ.rotaryDelayPosition=(organ.rotaryDelayPosition+1)%kHammondDelayFrames;
                scratch[(size_t)sample*2]=(left+right)*.5f;
                scratch[(size_t)sample*2+1]=scratch[(size_t)sample*2];
                hammondSide[(size_t)sample]=(left-right)*.5f;
            }
        }
        else if(engineTypes[(size_t)layer]==EngineType::analog) {
            // Same model as the desktop AnalogSynthEngine in its approved "browser 12 dB" mode.
            auto& analog=analogLayers[(size_t)layer];const auto& control=analog.controls;
            const double sr=kSampleRate;
            const float ratios[3]={std::exp2(analog.oscillator1Semitones/12.0f),std::exp2(control[3]/12.0f),std::exp2(control[4]/12.0f)};
            const float levels[3]={control[0]*.01f,control[1]*.01f,control[2]*.01f};
            const double attackSamples=std::max(1.0,sr*control[9]*0.001),decaySamples=std::max(1.0,sr*control[10]*0.001);
            const float sustain=std::max(0.001f,0.34f*(control[11]*.01f));
            const double releaseSeconds=std::max(0.015,(double)control[12]*0.001);
            const float releaseCoefficient=(float)std::exp(-1.0/(sr*std::max(0.003,releaseSeconds/5.0)));
            const int releaseLimit=(int)((releaseSeconds+0.03)*sr);
            const float quickReleaseCoefficient=(float)std::exp(-1.0/(sr*0.2/5.0));
            const int quickReleaseLimit=(int)((0.2+0.03)*sr);
            const float legatoCoefficient=(float)std::exp(-1.0/(sr*0.008));
            const float baseCutoff=25.0f*std::pow(700.0f,std::clamp(control[6],0.0f,100.0f)/100.0f);
            const float lfoToFilterHz=control[15]*4.2f;      // percent of the original 420 Hz depth
            const double lfoStep=(double)control[13]/sr;
            const float resonanceDb=control[7]*0.2f;          // Q in dB, as Web Audio
            const float noiseLevel=control[5]*.01f,mixerDrive=std::clamp(control[16]*.01f,0.0f,1.0f);
            const float mixDrive=1.0f+mixerDrive*3.0f;
            const float lfoToPitch=control[14];
            // The pitch LFO belongs to the layer (one phase shared by all voices).
            std::array<float,kMaxFrames> layerLfo;
            if(lfoToPitch!=0.0f)for(int sample=0;sample<frames;++sample){
                analog.lfoPhase+=control[13]/(float)sr;analog.lfoPhase-=std::floor(analog.lfoPhase);
                layerLfo[(size_t)sample]=std::sin(analog.lfoPhase*6.28318530718f);
            }
            // Filter coefficients are constant over the block unless the per-voice LFO moves the cutoff.
            const auto biquad=[&](float cutoffHz,double& b0,double& b1,double& a1,double& a2){
                const double frequency=std::clamp((double)cutoffHz,0.01,sr*0.4999);
                const double omega=6.283185307179586*frequency/sr,cosine=std::cos(omega);
                const double alpha=std::sin(omega)/(2.0*std::pow(10.0,(double)resonanceDb/20.0));
                const double a0=1.0+alpha;
                b0=(1.0-cosine)*0.5/a0;b1=(1.0-cosine)/a0;a1=-2.0*cosine/a0;a2=(1.0-alpha)/a0;
            };
            double fixedB0=0,fixedB1=0,fixedA1=0,fixedA2=0;
            if(lfoToFilterHz==0.0f)biquad(baseCutoff,fixedB0,fixedB1,fixedA1,fixedA2);
            for(int voiceIndex=0;voiceIndex<layerVoiceBudgets[li];++voiceIndex){
                auto& voice=analog.voices[(size_t)voiceIndex];
                if(!voice.active)continue;
                for(int sample=0;sample<frames;++sample){
                    if(voice.releasing&&voice.ampStage!=analogRelease){voice.ampStage=analogRelease;voice.releaseElapsed=0;}
                    else if(voice.legato&&voice.ampStage!=analogRelease){voice.ampStage=analogLegato;voice.legato=false;}
                    switch(voice.ampStage){
                        case analogAttack:
                            voice.ampValue=(float)(0.0001*std::pow(3400.0,std::min(1.0,voice.ampElapsed/attackSamples)));
                            if(++voice.ampElapsed>=attackSamples){voice.ampStage=analogDecay;voice.ampElapsed=0.0;}
                            break;
                        case analogDecay:
                            voice.ampValue=(float)(0.34*std::pow((double)sustain/0.34,std::min(1.0,voice.ampElapsed/decaySamples)));
                            if(++voice.ampElapsed>=decaySamples){voice.ampStage=analogSustain;voice.ampElapsed=0.0;}
                            break;
                        case analogSustain:voice.ampValue=sustain;break;
                        case analogLegato:voice.ampValue=sustain+(voice.ampValue-sustain)*legatoCoefficient;break;
                        default:
                            voice.ampValue=0.0001f+(voice.ampValue-0.0001f)*(voice.quickRelease?std::min(releaseCoefficient,quickReleaseCoefficient):releaseCoefficient);
                            break;
                    }
                    if(voice.ampStage==analogRelease&&++voice.releaseElapsed>(voice.quickRelease?std::min(releaseLimit,quickReleaseLimit):releaseLimit)){voice=AnalogVoice{};break;}
                    const float semitones=lfoToPitch!=0.0f?layerLfo[(size_t)sample]*lfoToPitch:0.0f;
                    const float pitchRatio=std::abs(semitones)<1.0e-7f?1.0f:std::exp2(semitones/12.0f);
                    const float base=voice.frequency*pitchRatio;
                    const float inc1=std::clamp(base*ratios[0]/(float)sr,0.0f,0.49f),inc2=std::clamp(base*ratios[1]/(float)sr,0.0f,0.49f),inc3=std::clamp(base*ratios[2]/(float)sr,0.0f,0.49f);
                    const float p1=voice.phase1,p2=voice.phase2,p3=voice.phase3;
                    voice.phase1=analogWrap(voice.phase1+inc1);voice.phase2=analogWrap(voice.phase2+inc2);voice.phase3=analogWrap(voice.phase3+inc3);
                    float mixed=0.0f;
                    if(analog.oscillatorEnabled[0])mixed+=analogBlepWave(analog.waves[0],p1,inc1)*levels[0];
                    if(analog.oscillatorEnabled[1])mixed+=analogBlepWave(analog.waves[1],p2,inc2)*levels[1];
                    if(analog.oscillatorEnabled[2])mixed+=analogBlepWave(analog.waves[2],p3,inc3)*levels[2];
                    float noise=analogNoise(voice.noiseState);
                    if(analog.pinkNoise)noise=0.98f*voice.pinkState+0.02f*noise;
                    voice.pinkState=noise;
                    mixed+=noise*noiseLevel;
                    if(mixerDrive>0.0f)mixed=std::tanh(mixed*mixDrive)/mixDrive;
                    double b0=fixedB0,b1=fixedB1,a1=fixedA1,a2=fixedA2;
                    if(lfoToFilterHz!=0.0f){
                        const float lfo=(float)std::sin(voice.lfoPhase*6.283185307179586);
                        voice.lfoPhase+=lfoStep;voice.lfoPhase-=std::floor(voice.lfoPhase);
                        biquad(baseCutoff+lfo*lfoToFilterHz,b0,b1,a1,a2);
                    }else{voice.lfoPhase+=lfoStep;voice.lfoPhase-=std::floor(voice.lfoPhase);}
                    const double out=b0*mixed+b1*voice.biquadX1+b0*voice.biquadX2-a1*voice.biquadY1-a2*voice.biquadY2;
                    voice.biquadX2=voice.biquadX1;voice.biquadX1=mixed;voice.biquadY2=voice.biquadY1;voice.biquadY1=out;
                    // 0.45 browser master (velocity-insensitive VCA) and the desktop's -6 dB Analog trim.
                    float output=(float)out*voice.ampValue*0.45f*0.5011872f;
                    ++voice.age;
                    output=voice.transition.process(output);
                    scratch[(size_t)sample*2]+=output;
                }
            }
            for(int sample=0;sample<frames;++sample) scratch[(size_t)sample*2+1]=scratch[(size_t)sample*2];
        }
        const float targetLayer = layerGains[(size_t) layer];
        const float outputCalibration=engineTypes[li]==EngineType::hammond
                ?kHammondOutputCalibration:1.0f;
        const float targetMaster = masterGain*blockFade;
        // Gain changes are smoothed per block to avoid clicks when a fader is
        // moved while notes are sounding.
        const float layerStep = (targetLayer - smoothedLayerGains[(size_t)layer]) / (float)std::max(frames, 1);
        const float panStep=(layerPan[(size_t)layer]-smoothedLayerPan[(size_t)layer])/(float)std::max(frames,1);
        const float masterStep = (targetMaster - smoothedMasterGain) / (float)std::max(frames, 1);
        const bool useCutoff=layerCutoff[li]<99.999f;
        const bool useLowEq=eqLow[li]!=0.0f, useMidEq=eqMid[li]!=0.0f,
                useHighEq=eqHigh[li]!=0.0f, useHighPass=eqHighPassHz[li]>20.0f,
                useLowPass=eqLowPassHz[li]<20000.0f;
        const bool useEq=useLowEq||useMidEq||useHighEq||useHighPass||useLowPass;
        const float cutoffHz=20.0f*std::pow(900.0f,layerCutoff[li]/100.0f);
        const float cutoffAlpha=1.0f-std::exp(-6.2831853f*cutoffHz/(float)kSampleRate);
        const float makeupGain=std::pow(10.0f,compressorMakeupDb[li]/20.0f);
        const float attackDetector=std::exp(-1.0f/(std::max(0.0001f,compressorAttack[li])*(float)kSampleRate));
        const float releaseDetector=std::exp(-1.0f/(std::max(0.0001f,compressorRelease[li])*(float)kSampleRate));
        const bool fixedPan=panStep==0.0f;
        const float panAngle=(std::clamp(layerPan[li],-1.0f,1.0f)+1.0f)*0.7853981634f;
        const float fixedLeft=std::cos(panAngle),fixedRight=std::sin(panAngle);
        for (int frame = 0; frame < frames; ++frame)
        {
            std::array<float,2> shaped{scratch[(size_t)frame*2],scratch[(size_t)frame*2+1]};
            if(!originalSf2&&(useEq||useCutoff)){
                for(int channel=0;channel<2;++channel){
                    if(useEq){
                        auto& filters=layerEqFilters[li][(size_t)channel];
                        if(useLowEq)shaped[(size_t)channel]=processBiquad(filters[0],shaped[(size_t)channel]);
                        if(useMidEq)shaped[(size_t)channel]=processBiquad(filters[1],shaped[(size_t)channel]);
                        if(useHighEq)shaped[(size_t)channel]=processBiquad(filters[2],shaped[(size_t)channel]);
                        if(useHighPass)shaped[(size_t)channel]=processBiquad(filters[3],shaped[(size_t)channel]);
                        if(useLowPass)shaped[(size_t)channel]=processBiquad(filters[4],shaped[(size_t)channel]);
                    }
                    if(useCutoff){
                        lowPassState[li][(size_t)channel]+=(shaped[(size_t)channel]-lowPassState[li][(size_t)channel])*cutoffAlpha;
                        shaped[(size_t)channel]=lowPassState[li][(size_t)channel];
                    }else lowPassState[li][(size_t)channel]=shaped[(size_t)channel];
                }
            }
            // Link the compressor detector across both channels, but retain
            // independent stereo signal paths for EQ, filtering and chorus.
            const float magnitude=std::max(std::abs(shaped[0]),std::abs(shaped[1]));
            const float threshold=compressorThreshold[li];
            const float detector=magnitude>compressorEnvelope[li]?attackDetector:releaseDetector;
            compressorEnvelope[li]=detector*compressorEnvelope[li]+(1.0f-detector)*magnitude;
            const float envelope=std::max(0.000001f,compressorEnvelope[li]);
            // The envelope above always advances; the gain (with its division)
            // is only needed when the compressor mix actually uses it.
            const float compressionGain=(!originalSf2&&layerCompressorMix[li]>0.0f&&envelope>threshold)
                    ?(threshold+(envelope-threshold)/compressorRatio[li])/envelope:1.0f;
            const float side=engineTypes[li]==EngineType::hammond?hammondSide[(size_t)frame]:0.0f;
            for(int channel=0;channel<2;++channel){
                float& value=shaped[(size_t)channel];
                const float compressed=value*compressionGain;
                value=(originalSf2||layerCompressorMix[li]<=0.0f)?value
                        :(value+(compressed-value)*layerCompressorMix[li])*makeupGain;
                const float dryShaped=value;
                const int cursor=layerChorusCursor[li][(size_t)channel];
                if(!originalSf2&&layerChorusMix[li]>0.0f) {
                    const float lfo=std::sin(cursor*0.006135923f);
                    const int chorusDelay=std::clamp((int)(720.0f+lfo*300.0f),1,kChorusFrames-1);
                    const int chorusRead=(cursor+kChorusFrames-chorusDelay)%kChorusFrames;
                    value+=(layerChorusBuffer[li][(size_t)channel][(size_t)chorusRead]-value)*layerChorusMix[li];
                }
                layerChorusBuffer[li][(size_t)channel][(size_t)cursor]=dryShaped;
                layerChorusCursor[li][(size_t)channel]=(cursor+1)%kChorusFrames;
                scratch[(size_t)frame*2+(size_t)channel]=value+(channel==0?side:-side);
            }
            const float gain = (smoothedLayerGains[(size_t)layer] + layerStep * frame) *
                    (smoothedMasterGain + masterStep * frame);
            const float pan=fixedPan?layerPan[li]:std::clamp(smoothedLayerPan[li]+panStep*frame,-1.0f,1.0f);
            // TSF already supplies stereo. Unity at center preserves the source
            // instead of applying the -3 dB constant-power law for mono synths.
            // TSF supplies stereo in both SF2 modes. Processing must not
            // silently add a -3 dB center-pan attenuation to its dry signal.
            const bool stereoSf2=engineTypes[li]==EngineType::sf2||engineTypes[li]==EngineType::nord||engineTypes[li]==EngineType::nsmp;
            const float leftPan=stereoSf2?std::min(1.0f,1.0f-pan):(fixedPan?fixedLeft:std::cos((pan+1.0f)*0.7853981634f));
            const float rightPan=stereoSf2?std::min(1.0f,1.0f+pan):(fixedPan?fixedRight:std::sin((pan+1.0f)*0.7853981634f));
            for(int channel=0;channel<2;++channel){
                const size_t sample=(size_t)frame*2+(size_t)channel;
                const float sideGain=channel==0?leftPan:rightPan;
                const float calibratedSample=scratch[sample]*outputCalibration;
                const float layerSample=calibratedSample*gain*sideGain;
                mix[sample]+=layerSample;
                if(!originalSf2)reverbSend[sample]+=layerSample*layerReverbSend[li];
                renderedPeaks[li]=std::max(renderedPeaks[li],std::abs(calibratedSample*gain));
            }
        }
        smoothedLayerGains[(size_t)layer] = targetLayer;
        smoothedLayerPan[(size_t)layer]=layerPan[(size_t)layer];
    }
    smoothedMasterGain = masterGain*blockFade;
    float renderedMasterPeak = 0.0f;
    bool clippedThisBlock=false;
    // Loop-invariant reverb geometry is hoisted out of the per-sample loop and
    // the ring-buffer wraps use compare/subtract instead of integer division;
    // the delay taps and every output sample are numerically unchanged.
    const int reverbSize=(int)reverbBuffer.size();
    const float room=std::clamp((reverbDelayMs-20.0f)/105.0f,0.0f,1.0f);
    const float roomFeedback=0.70f+room*0.28f;                       // longer tail with TEMPO
    const float roomDamp=std::clamp((reverbFeedback-0.24f)/0.34f,0.0f,1.0f)*0.4f;
    const float wetOwn=reverbStereoWidth*0.5f+0.5f,wetCross=(1.0f-reverbStereoWidth)*0.5f;
    // Idle layers cost nothing: stop the 24 filters once nothing feeds the
    // reverb and its tail has had five seconds to die away.
    bool reverbFed=reverbMix>0.0f;
    for(int n=0;n<kLayerCount&&!reverbFed;++n)reverbFed=layerReverbSend[(size_t)n]>0.0f&&engineTypes[(size_t)n]!=EngineType::empty;
    if(reverbFed)reverbHoldSamples=kSampleRate*5;
    else if(reverbHoldSamples>0)reverbHoldSamples=std::max(0,reverbHoldSamples-(int)frames);
    const bool reverbRunning=reverbHoldSamples>0;
    if(!reverbRunning&&reverbWasRunning){reverbLeft.clear();reverbRight.clear();}
    reverbWasRunning=reverbRunning;
    for (int sample = 0; sample < samples; ++sample) {
        // Preserve the summed signal's natural dynamics. Do not apply a soft
        // limiter here; only saturate at final PCM conversion if the mix truly
        // exceeds the representable output range.
        int chorusRead = effectCursor + reverbSize - 960 + (sample & 3) * 24;
        while(chorusRead>=reverbSize)chorusRead-=reverbSize;
        const float chorus = reverbBuffer[(size_t)chorusRead];
        const float dry = mix[(size_t)sample];
        float wet=0.0f;
        if(!reverbRunning){wet=0.0f;}
        else if((sample&1)==0){
            const int frameIndex=sample;
            const float inL=reverbSend[(size_t)frameIndex]+mix[(size_t)frameIndex]*reverbMix;
            const float inR=reverbSend[(size_t)frameIndex+1]+mix[(size_t)frameIndex+1]*reverbMix;
            const float input=(inL+inR)*0.015f;
            const float l=reverbLeft.process(input,0,roomFeedback,roomDamp)*2.0f;
            const float r=reverbRight.process(input,kStereoSpread,roomFeedback,roomDamp)*2.0f;
            pendingWetLeft=l*wetOwn+r*wetCross;pendingWetRight=r*wetOwn+l*wetCross;
            wet=pendingWetLeft;
        } else wet=pendingWetRight;
        const float effected = dry + wet + (chorus - dry) * chorusMix * 0.22f;
        reverbBuffer[(size_t)effectCursor] = dry;
        if(++effectCursor>=reverbSize)effectCursor=0;
        clippedThisBlock=clippedThisBlock||effected>1.f||effected< -1.f;
        const float pcm=std::clamp(effected*32767.0f,-32768.0f,32767.0f);
        output[sample]=static_cast<short>(pcm);
        renderedMasterPeak = std::max(renderedMasterPeak, std::abs((float) output[sample]) / 32768.0f);
    }
    if(clippedThisBlock)masterClippedBlocks.fetch_add(1,std::memory_order_relaxed);
    for (int layer = 0; layer < kLayerCount; ++layer) {
        layerPeaks[(size_t) layer] = std::max(renderedPeaks[(size_t) layer], layerPeaks[(size_t) layer] * 0.88f);
        publishedLayerPeaks[(size_t)layer].store(layerPeaks[(size_t)layer],std::memory_order_relaxed);
    }
    masterPeak = std::max(renderedMasterPeak, masterPeak * 0.88f);
    publishedMasterPeak.store(masterPeak,std::memory_order_relaxed);
    return true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeRender(JNIEnv* env,jclass,jshortArray destination,jint frames)
{
    if(destination==nullptr||frames<=0)return;
    frames=std::min(frames,kMaxFrames);
    if(env->GetArrayLength(destination)<frames*2)return;
    auto* output=env->GetShortArrayElements(destination,nullptr);
    if(!output)return;
    renderClassicPlayerPcm(output,frames,false);
    env->ReleaseShortArrayElements(destination,output,0);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeLayerPeak(JNIEnv*, jclass, jint layer)
{
    if (layer < 0 || layer >= kLayerCount) return 0.0f;
    return publishedLayerPeaks[(size_t)layer].load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeMasterPeak(JNIEnv*, jclass)
{
    return publishedMasterPeak.load(std::memory_order_relaxed);
}
