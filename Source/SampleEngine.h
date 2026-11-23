#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "NordPianoDecoder.h"
#include "Sf2Engine.h"
#include "LayerEffects.h"
#include <array>
#include <atomic>
#include <algorithm>
#include "PianoPrefetch.h"
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

// A small, self-contained sample instrument used by the clean-room Samplit
// compatibility layer.  It intentionally keeps the audio callback free from
// file I/O: files are decoded before they become visible to the renderer.
class SampleEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;

    struct LayerConfig
    {
        int rootNote = 60;
        bool loop = false;
        double startSeconds = 0.0;
        double endSeconds = 0.0; // zero means the end of the decoded sample
    };

    SampleEngine();

    void prepare(double sampleRate, int maximumBlockSize);
    juce::Result loadSample(int layer, const juce::File& file,
                            std::shared_ptr<NordPianoDecoder::Instrument> prepared = {});
    juce::Result prepareSample(const juce::File&, std::shared_ptr<NordPianoDecoder::Instrument>&,
                               std::uint64_t maxDecodedBytes = std::numeric_limits<std::uint64_t>::max());
    juce::Result preloadSample(const juce::File& file);
private:
    std::mutex prepareMutex; // one decode at a time: a preload in flight is awaited, not repeated
public:
    void unload(int layer);
    void stopAllSounds();
    void stopLayerSounds(int layer);
    bool isLoaded(int layer) const;
    juce::String path(int layer) const;
    // The decoded instrument currently loaded in a layer (shared, read-only), or null.
    std::shared_ptr<NordPianoDecoder::Instrument> loadedInstrument(int layer) const;
    float getLayerPeak(int layer) const;
    struct VelocityInfo { int received = 0, shaped = 0, strokeLayer = -1, strokeLayerMax = 0; };
    VelocityInfo getLastVelocity(int layer) const;
    LayerConfig getConfig(int layer) const;
    void setConfig(int layer, const LayerConfig& config);

    void process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                 const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                 const std::array<Sf2Engine::LayerConfig, layerCount>& routing);

private:
    friend struct SampleEngineRegressionTests;
    struct Voice
    {
        bool active = false;
        bool released = false;
        int note = -1;
        int channel = 1;
        std::uint64_t age = 0; // rendered frames, independent of callback size
        int sampleIndex = -1; // Nord piano stroke; -1 means the single audio buffer
        double position = 0.0;
        double increment = 1.0;
        float level = 0.0f;
        float velocity = 0.0f;
        float attackGain = 1.0f;
        float attackStep = 1.0f;
        float decayGain = 1.0f;
        float releaseGain = 1.0f;
        float releaseCoefficient = 0.0f;
        float pianoDecaySeconds = 15.0f; // captured at note-on to avoid jumps during playback
        bool pianoRelease = false;
        bool waitingForSustain = false;
        bool sostenutoHeld = false; // key was down when the sostenuto pedal was pressed
        // Natural NPNO decay: set at note-on, applied once the voice reaches its sustain loop.
        bool naturalDecay = false;
        bool loopDecayActive = false;
        float loopDecayGain = 1.0f;
        float loopDecayMul = 1.0f; // per output sample
        float earlyDecayGain = 1.0f; // fast initial decay of the first seconds (see startPianoVoice)
        float earlyDecayMul = 1.0f;  // per output sample while the voice is younger than 4 s
        // Reference model: velocity-dependent one-pole low-pass (soft notes are darker). 1 = bypass.
        float toneCoefficient = 1.0f;
        float toneStateLeft = 0.0f, toneStateRight = 0.0f;
        float airCoefficient = 1.0f; // one-pole at ~9 kHz that splits off the top octave added back as "air"
        float airStateLeft = 0.0f, airStateRight = 0.0f;
    };

    struct Layer
    {
        enum class ModulationKind { none, tremolo, rotary };
        juce::AudioBuffer<float> audio;
        std::shared_ptr<NordPianoDecoder::Instrument> piano;
        bool pianoLoaded = false;
        bool finitePiano = false;
        juce::String sourcePath;
        ModulationKind modulationKind = ModulationKind::none;
        double sourceSampleRate = 44100.0;
        LayerConfig config;
        std::array<Voice, Sf2Engine::maxPolyphonyPerLayer> voices;
        std::array<bool, 16> sustainDown {};
        std::array<float, 16> pedalDepth {}; // half-pedal: 0 = up, up to ~1 just below the CC64 switch point
        std::array<bool, 16> sostenutoDown {};
        std::array<bool, 16> softPedalDown {};
        // Per MIDI channel controllers: pitch bend (+-2 semitones), volume (CC7) and expression (CC11).
        std::array<float, 16> pitchBendValue {};        // -1..1
        std::array<float, 16> pitchRatioNow = [] { std::array<float, 16> a; a.fill(1.0f); return a; }(); // smoothed playback ratio
        std::array<float, 16> cc7 = [] { std::array<float, 16> a; a.fill(1.0f); return a; }();
        std::array<float, 16> cc11 = [] { std::array<float, 16> a; a.fill(1.0f); return a; }();
        std::array<float, 16> channelGainNow = [] { std::array<float, 16> a; a.fill(1.0f); return a; }();
        static constexpr int stringResCount = 56; // A0..E5 string fundamentals
        struct StringRes
        {
            // Four resonators per string: [0] fixed pitch, undamped by the sustain pedal; [1..3] at -2.5%, 0, +2.5%
            // around a sounding note (the detuned unison strings).
            std::array<std::array<float, 4>, stringResCount> y1 {}, y2 {}, level {};
            bool active = false;
        } stringRes;
        std::array<float, 2> cutoffState {};
        std::array<float, 2> compressorEnvelope {};
        std::array<float, 2> pianoLowEqState {};
        std::array<float, 2> pianoHighEqState {};
        std::array<juce::SmoothedValue<float>, 3> pianoTimbreGains;
        double modulationPhase = 0.0;
        float modulationAmount = 0.0f;
        juce::SmoothedValue<float> gain;
        LayerEqState eq;
        NordLayerEffects nordFx;
        LayerReverb reverb;
        bool effectTailPending = false;
        uint64_t effectQuietSamples = 0;
        std::array<float, 4> lastReverbParameters { -1.0f, -1.0f, -1.0f, -1.0f };
        std::atomic<float> peak { 0.0f };
        bool prefetchPublished = false; // audio thread: the prefetcher currently holds voice positions for this layer
        // Last note-on seen by this layer, for the velocity diagnostic in the UI.
        std::atomic<int> lastVelocityIn { 0 }, lastVelocityShaped { 0 };
        std::atomic<int> lastStrokeLayer { -1 }, lastStrokeLayerMax { 0 };
    };

    void startVoice(Layer&, int note, int channel, float velocity,
                    const Sf2Engine::LayerConfig& routing, int inputNote = -1);
    // levelScale scales the voice level after the velocity law (key-off reflections play at half the level of the
    // main note, whatever law the main note follows).
    void startPianoVoice(Layer&, int sampleIndex, int note, int channel, float velocity,
                         const Sf2Engine::LayerConfig& routing, int inputNote = -1, float levelScale = 1.0f);
    Voice& allocateVoice(Layer&, int polyphony);
    static void preparePianoLoops(NordPianoDecoder::Instrument&);
    static void measurePianoLevels(NordPianoDecoder::Instrument&);
    static void resetPianoControllers(Layer&);
    void processStringResonance(Layer&, const Sf2Engine::LayerConfig&, int samples);
    void releasePianoVoiceFor(Voice&, const Sf2Engine::LayerConfig&, float halfPedalStretch = 1.0f);
    std::shared_ptr<NordPianoDecoder::Instrument> findCachedPiano(const juce::File&);
    std::shared_ptr<NordPianoDecoder::Instrument> cachePiano(
        const juce::File&, std::shared_ptr<NordPianoDecoder::Instrument>);
    int findPianoStroke(const Layer&, int note, float velocity, int bank) const;
    void releasePianoVoice(Voice&, float releaseControl, bool softRelease, float stretch = 1.0f);
    void releaseVoice(Layer&, int note, int channel, float releaseControl);
    void renderLayer(Layer&, juce::AudioBuffer<float>&,
                     const juce::MidiBuffer&, const Sf2Engine::LayerConfig&);
    void publishPrefetch(Layer&);
    void processEffectsAndMix(Layer&, juce::AudioBuffer<float>&,
                              const Sf2Engine::LayerConfig&);
    static float readInterpolated(const juce::AudioBuffer<float>&, int channel, double position);
    static float readPianoInterpolated(const NordPianoDecoder::Stroke&, int channels,
                                       int channel, double position, bool looped = false);
    static void readPianoStereoInterpolated(const NordPianoDecoder::Stroke&, int channels,
                                            double position, bool looped,
                                            float& left, float& right);

    std::array<Layer, layerCount> layers;
    PianoPrefetcher prefetcher; // keeps mapped NPNO pages resident ahead of playback
    std::vector<PianoPrefetcher::Voice> prefetchScratch; // audio-thread scratch for publishPrefetch (heap, sized in prepare)
    std::array<juce::MidiBuffer, layerCount> combinedMidi;
    juce::AudioBuffer<float> scratch;
    std::vector<float> resonanceInput, resonanceOutput;
    juce::AudioFormatManager formats;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    mutable juce::CriticalSection lock;
    struct CachedPiano
    {
        std::shared_ptr<NordPianoDecoder::Instrument> instrument;
        size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };
    // An XL Nord piano can exceed 600 MiB decoded. Keep enough room for one
    // such piano plus the other instruments in a Live Set bank; otherwise the
    // background preload silently discards it and every switch decodes again.
    // Scales with installed RAM (a quarter, between 1 and 3 GiB) so a Live Set bank can keep
    // two XL pianos (~800 MB decoded each) resident on machines with enough memory.
    static size_t decodedCacheLimitBytes()
    {
        const auto ramMiB = static_cast<std::uint64_t>(juce::jmax<juce::int64>(0, juce::SystemStats::getMemorySizeInMegabytes()));
        const auto quarter = ramMiB * 1024ull * 1024ull / 4ull;
        return static_cast<size_t>(std::clamp<std::uint64_t>(quarter, 1024ull * 1024ull * 1024ull,
                                                             3ull * 1024ull * 1024ull * 1024ull));
    }
    std::mutex pianoCacheMutex;
    std::unordered_map<std::string, CachedPiano> pianoCache;
    // Weak index reuses instruments held by live layers after LRU eviction.
    std::unordered_map<std::string, std::weak_ptr<NordPianoDecoder::Instrument>> pianoCacheLive;
    size_t pianoCacheBytes = 0;
    std::uint64_t pianoCacheClock = 0;
};
