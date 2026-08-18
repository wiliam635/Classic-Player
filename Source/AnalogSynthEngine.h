#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Sf2Engine.h"
#include <array>
#include <atomic>

// Native three-oscillator analog synthesizer used by Classic Keys Analog.
// It is intentionally independent from SF2, DX7 and hosted instruments.
class AnalogSynthEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;
    static constexpr int voicesPerLayer = 8;

    enum class Waveform { triangle = 0, saw, square, pulse };

    struct Config
    {
        Sf2Engine::LayerConfig routing;
        Waveform oscillator1Wave = Waveform::saw;
        Waveform oscillator2Wave = Waveform::saw;
        Waveform oscillator3Wave = Waveform::triangle;
        float oscillator1Level = 0.8f;
        float oscillator2Level = 0.55f;
        float oscillator3Level = 0.25f;
        float oscillator2Semitones = 0.0f;
        float oscillator3Semitones = -12.0f;
        float noiseLevel = 0.0f;
        float cutoff = 72.0f;
        float resonance = 0.12f;
        float filterEnvelopeAmount = 0.35f;
        float ampAttackMs = 6.0f;
        float ampDecayMs = 180.0f;
        float ampSustain = 0.78f;
        float ampReleaseMs = 260.0f;
        float filterAttackMs = 8.0f;
        float filterDecayMs = 240.0f;
        float filterSustain = 0.35f;
        float filterReleaseMs = 320.0f;
        float lfoRateHz = 4.0f;
        float lfoToPitch = 0.0f;
        float lfoToFilter = 0.0f;
        float glideMs = 0.0f;
    };

    AnalogSynthEngine();

    void prepare(double newSampleRate, int maximumBlockSize);
    void reset();
    void stopAllSounds();
    void process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                 const std::array<Config, layerCount>& configs);

    float getLayerPeak(int layer) const noexcept;

private:
    enum class EnvelopeStage { idle, attack, decay, sustain, release };

    struct Envelope
    {
        float value = 0.0f;
        float releaseStart = 0.0f;
        EnvelopeStage stage = EnvelopeStage::idle;
    };

    struct Voice
    {
        bool active = false;
        bool keyDown = false;
        int note = -1;
        int midiChannel = 1;
        float velocity = 0.0f;
        float phase1 = 0.0f;
        float phase2 = 0.0f;
        float phase3 = 0.0f;
        float currentFrequency = 440.0f;
        float targetFrequency = 440.0f;
        float lowpass = 0.0f;
        float noiseState = 0.0f;
        Envelope amp;
        Envelope filter;
    };

    struct Layer
    {
        std::array<Voice, voicesPerLayer> voices {};
        float lfoPhase = 0.0f;
    };

    static float waveform(Waveform waveform, float phase);
    static float midiNoteToFrequency(int note) noexcept;
    static bool acceptsMessage(const Sf2Engine::LayerConfig& config,
                               const juce::MidiMessage& message);
    static float nextEnvelope(Envelope& envelope, float attackMs, float decayMs,
                              float sustain, float releaseMs, double sampleRate);

    void noteOn(int layer, int midiChannel, int note, float velocity, const Config& config);
    void noteOff(int layer, int midiChannel, int note);
    void renderVoice(Voice& voice, Layer& layer, const Config& config, float& left,
                     float& right);

    std::array<Layer, layerCount> layers {};
    std::array<std::atomic<float>, layerCount> peaks {};
    double sampleRate = 44100.0;
};