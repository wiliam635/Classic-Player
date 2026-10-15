#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "NordPianoDecoder.h"
#include "Sf2Engine.h"
#include <array>
#include <atomic>

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
    juce::Result loadSample(int layer, const juce::File& file);
    void unload(int layer);
    void stopAllSounds();
    bool isLoaded(int layer) const;
    juce::String path(int layer) const;
    float getLayerPeak(int layer) const;
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
        float releaseGain = 1.0f;
        float releaseCoefficient = 0.0f;
        bool pianoRelease = false;
        bool waitingForSustain = false;
    };

    struct Layer
    {
        juce::AudioBuffer<float> audio;
        NordPianoDecoder::Instrument piano;
        bool pianoLoaded = false;
        juce::String sourcePath;
        double sourceSampleRate = 44100.0;
        LayerConfig config;
        std::array<Voice, Sf2Engine::polyphonyPerLayer> voices;
        std::array<bool, 16> sustainDown {};
        std::array<float, 2> cutoffState {};
        std::array<float, 2> compressorEnvelope {};
        std::array<float, 2> pianoLowEqState {};
        std::array<float, 2> pianoHighEqState {};
        std::array<juce::SmoothedValue<float>, 3> pianoTimbreGains;
        double modulationPhase = 0.0;
        float modulationAmount = 0.0f;
        juce::SmoothedValue<float> gain;
        LayerEqState eq;
        juce::Reverb reverb;
        std::array<float, 4> lastReverbParameters { -1.0f, -1.0f, -1.0f, -1.0f };
        std::atomic<float> peak { 0.0f };
    };

    void startVoice(Layer&, int note, int channel, float velocity,
                    const Sf2Engine::LayerConfig& routing);
    void startPianoVoice(Layer&, int sampleIndex, int note, int channel, float velocity,
                         const Sf2Engine::LayerConfig& routing);
    static Voice& allocateVoice(Layer&);
    static void preparePianoLoops(NordPianoDecoder::Instrument&);
    int findPianoStroke(const Layer&, int note, float velocity, int bank) const;
    void releasePianoVoice(Voice&, float releaseControl);
    void releaseVoice(Layer&, int note, int channel, float releaseControl);
    void renderLayer(Layer&, juce::AudioBuffer<float>&,
                     const juce::MidiBuffer&, const Sf2Engine::LayerConfig&);
    void processEffectsAndMix(Layer&, juce::AudioBuffer<float>&,
                              const Sf2Engine::LayerConfig&);
    static float readInterpolated(const juce::AudioBuffer<float>&, int channel, double position);
    static float readPianoInterpolated(const NordPianoDecoder::Stroke&, int channels,
                                       int channel, double position, bool looped = false);

    std::array<Layer, layerCount> layers;
    std::array<juce::MidiBuffer, layerCount> combinedMidi;
    juce::AudioBuffer<float> scratch;
    juce::AudioFormatManager formats;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    mutable juce::CriticalSection lock;
};
