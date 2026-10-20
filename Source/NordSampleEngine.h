#pragma once

#include "NordSampleDecoder.h"
#include "Sf2Engine.h"
#include "LayerEffects.h"
#include <array>
#include <atomic>

// Independent sample playback engine for Nord NSMP4 (.nsmp4) instruments.
// Decoding and file I/O happen on the caller thread, never in process().
class NordSampleEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;
    static constexpr int voicesPerLayer = 192;

    void prepare(double sampleRate, int maximumBlockSize);
    juce::Result loadSample(int layer, const juce::File& file);
    void unload(int layer);
    void stopAllSounds();
    bool isLoaded(int layer) const;
    juce::String path(int layer) const;
    juce::String name(int layer) const;
    float getLayerPeak(int layer) const;
    void process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                 const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                 const std::array<Sf2Engine::LayerConfig, layerCount>& routing);

private:
    friend struct NordSampleRegressionTests;
    struct Voice
    {
        bool active = false, released = false, keyDown = false, waitingForSustain = false;
        int note = -1, channel = 1, zone = -1;
        std::uint64_t age = 0;
        double position = 0.0, increment = 1.0;
        float amplitude = 0.0f, attack = 1.0f, attackStep = 1.0f;
        float releaseGain = 1.0f, releaseCoefficient = 0.0f;
    };

    struct Layer
    {
        NordSampleDecoder::Instrument instrument;
        juce::String sourcePath;
        bool loaded = false;
        std::array<Voice, voicesPerLayer> voices {};
        std::array<bool, 16> sustainDown {};
        std::array<float, 2> cutoffState {}, compressorEnvelope {};
        LayerEqState eq;
        NordLayerEffects nordFx;
        juce::Reverb reverb;
        std::array<float, 4> lastReverbParameters { -1.0f, -1.0f, -1.0f, -1.0f };
        juce::SmoothedValue<float> gain;
        std::atomic<float> peak { 0.0f };
    };

    static Voice& allocateVoice(Layer&) noexcept;
    static int findZone(const NordSampleDecoder::Instrument&, int note, int velocity) noexcept;
    static float readSample(const NordSampleDecoder::Zone&, int channel, double position) noexcept;
    static float releaseCoefficientFor(float control, double sampleRate) noexcept;
    void handleMidiMessage(Layer&, const juce::MidiMessage&,
                           const Sf2Engine::LayerConfig&) noexcept;
    void renderVoices(Layer&, juce::AudioBuffer<float>&, int startSample, int endSample,
                      float& peak) noexcept;
    void renderLayer(Layer&, juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                     const Sf2Engine::LayerConfig&, int midiStartSample);
    void processEffectsAndMix(Layer&, juce::AudioBuffer<float>&,
                              juce::AudioBuffer<float>&, const Sf2Engine::LayerConfig&);

    std::array<Layer, layerCount> layers;
    std::array<juce::AudioBuffer<float>, layerCount> scratch;
    std::array<juce::MidiBuffer, layerCount> combinedMidi;
    double currentSampleRate = 44100.0;
    int maximumBlockSize = 512;
    mutable juce::CriticalSection lock;
};
