#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "Sf2Engine.h"
#include <array>
#include <cstdint>

// Native DX7-compatible SysEx reader and six-operator FM player.
// A Yamaha bulk dump exposes all 32 voices; the selected voice is rendered
// without relying on an external GPL instrument.
class Dx7Engine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;
    static constexpr int maxPatches = 32;

    Dx7Engine() = default;

    void prepare(double sampleRate, int maximumBlockSize);
    juce::Result loadSysEx(int layer, const juce::File& file);
    void unload(int layer);
    void stopAllSounds();

    bool isLoaded(int layer) const;
    int patchCount(int layer) const;
    int selectedPatch(int layer) const;
    bool selectPatch(int layer, int patch);
    juce::String patchName(int layer) const;
    juce::String patchName(int layer, int patch) const;
    juce::String path(int layer) const;

    void process(juce::AudioBuffer<float>& output,
                 const juce::MidiBuffer& hostMidi,
                 const std::array<juce::MidiBuffer, layerCount>* routedMidi,
                 const std::array<Sf2Engine::LayerConfig, layerCount>& configs);

private:
    struct Patch
    {
        juce::String name { "DX7 INIT" };
        // Zero-based Yamaha DX7 algorithm number (0..31).
        int algorithm = 0;
        int feedback = 0;
        std::array<float, 6> levels { 0.72f, 0.56f, 0.46f, 0.36f, 0.28f, 0.22f };
        std::array<float, 6> ratios { 1.0f, 1.0f, 2.0f, 1.0f, 3.0f, 0.5f };
        std::array<bool, 6> fixedMode {};
        std::array<float, 6> fixedFrequency {};
        std::array<std::array<float, 4>, 6> egRates {};
        std::array<std::array<float, 4>, 6> egLevels {};
    };

    struct Voice
    {
        bool active = false;
        bool releasing = false;
        int note = -1;
        float velocity = 0.0f;
        float envelope = 0.0f;
        double currentFrequency = 0.0;
        double targetFrequency = 0.0;
        std::array<double, 6> phase {};
        std::array<float, 6> operatorEnvelope {};
        std::array<int, 6> operatorStage {};
        std::array<float, 6> feedback {};
    };

    struct Layer
    {
        juce::String sourcePath;
        std::array<Patch, maxPatches> patches {};
        int patchesLoaded = 0;
        int selectedPatch = 0;
        // Physical note state is kept separately from sounding voices. This
        // lets Mono Legato return to a still-held earlier note.
        std::array<bool, 128> heldNotes {};
        std::array<float, 128> heldVelocities {};
        bool sustainDown = false;
        std::array<Voice, 32> voices {};
    };

    static bool accepts(const Sf2Engine::LayerConfig&, const juce::MidiMessage&);
    static float shapedVelocity(const Sf2Engine::LayerConfig&, float velocity);
    static juce::String decodeName(const uint8_t* data, int size);
    static Patch parsePackedPatch(const uint8_t* voice);
    static Patch parseSinglePatch(const uint8_t* voice, int size);
    static double noteFrequency(int midiNote);

    void dispatch(Layer&, const Sf2Engine::LayerConfig&, const juce::MidiMessage&);
    void render(Layer&, const Sf2Engine::LayerConfig&, juce::AudioBuffer<float>&);

    std::array<Layer, layerCount> layers;
    double sampleRate = 48000.0;
    juce::CriticalSection lock;
};
