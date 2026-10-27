#pragma once

#include "Sf2Engine.h"
#include "LayerEffects.h"
#include <juce_data_structures/juce_data_structures.h>

// Lightweight, independent Nord-style tonewheel/transistor organ voice engine.
// This is a Classic Player adaptation; it does not emulate Nord DSP bit-for-bit.
class NordOrganEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;
    enum class Model { b3 = 0, vox = 1, farfisa = 2, pipe1 = 3, pipe2 = 4, b3Bass = 5 };
    struct Config
    {
        Sf2Engine::LayerConfig routing;
        Model model = Model::b3;
        std::array<int, 9> drawbars { 8, 8, 7, 6, 8, 5, 4, 3, 2 };
        int percussion = 0; // 0 off, 2 second harmonic, 3 third harmonic
        int vibratoChorus = 0; // 0 off, 1 light, 2 deep
        int rotary = 1; // 0 stopped, 1 slow, 2 fast
        float drive = 0.08f;
    };

    void prepare(double sampleRate, int blockSize);
    void unload(int layer);
    void stopAllSounds();
    float getLayerPeak(int layer) const noexcept;
    static juce::ValueTree save(const Config&, int layer);
    static Config restore(const juce::ValueTree&);
    void process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& host,
                 std::array<Config, layerCount>& configs,
                 const std::array<juce::MidiBuffer, layerCount>* routed = nullptr);

private:
    struct Voice
    {
        bool active = false, down = false, releasing = false;
        int note = 0, channel = 0, age = 0, releaseAge = 0;
        float envelope = 0.0f, releaseStart = 0.0f, percussionEnvelope = 0.0f;
        double percussionPhase = 0.0, percussionStep = 0.0;
        std::array<double, 9> phase {}, step {};
    };
    struct Layer
    {
        std::array<Voice, Sf2Engine::maxPolyphonyPerLayer> voices {};
        std::array<bool, 16> sustain {};
        LayerReverb reverb;
        LayerEqState eq;
        std::array<float, 2> cutoffState {};
        double rotaryPhase = 0.0, vibratoPhase = 0.0;
        float compressorEnvelope = 0.0f;
        bool enabled = false;
        bool effectTailPending = false;
        uint64_t effectQuietSamples = 0;
    };

    void handleMessage(Layer&, const Config&, const juce::MidiMessage&);
    void releaseVoice(Voice&) noexcept;
    float oscillator(double phase) const noexcept;
    std::array<Layer, layerCount> layers;
    std::array<std::atomic<float>, layerCount> peaks {};
    std::array<float, 2049> sineTable {};
    double sampleRate = 48000.0;
};
