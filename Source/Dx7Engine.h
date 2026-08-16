#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "Sf2Engine.h"
#include <array>

// Lightweight native DX7-compatible SysEx reader and six-operator FM player.
// It accepts Yamaha DX7 .syx banks and keeps the player independent of GPL code.
class Dx7Engine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;

    Dx7Engine() = default;

    void prepare(double sampleRate, int maximumBlockSize);
    juce::Result loadSysEx(int layer, const juce::File& file);
    void unload(int layer);
    void stopAllSounds();

    bool isLoaded(int layer) const;
    juce::String patchName(int layer) const;
    juce::String path(int layer) const;

    void process(juce::AudioBuffer<float>& output,
                 const juce::MidiBuffer& hostMidi,
                 const std::array<juce::MidiBuffer, layerCount>* routedMidi,
                 const std::array<Sf2Engine::LayerConfig, layerCount>& configs);

private:
    struct Patch
    {
        juce::String name { "DX7 INIT" };
        int algorithm = 0;
        std::array<float, 6> levels { 0.72f, 0.56f, 0.46f, 0.36f, 0.28f, 0.22f };
        std::array<float, 6> ratios { 1.0f, 1.0f, 2.0f, 1.0f, 3.0f, 0.5f };
    };

    struct Voice
    {
        bool active = false;
        int note = -1;
        float velocity = 0.0f;
        std::array<double, 6> phase {};
    };

    struct Layer
    {
        juce::String sourcePath;
        Patch patch;
        std::array<Voice, 32> voices {};
    };

    static bool accepts(const Sf2Engine::LayerConfig&, const juce::MidiMessage&);
    static float shapedVelocity(const Sf2Engine::LayerConfig&, float velocity);
    static juce::String decodeName(const uint8_t* data, int size);
    static Patch parsePatch(const juce::MemoryBlock& data);
    static double noteFrequency(int midiNote);

    void dispatch(Layer&, const Sf2Engine::LayerConfig&, const juce::MidiMessage&);
    void render(Layer&, const Sf2Engine::LayerConfig&, juce::AudioBuffer<float>&);

    std::array<Layer, layerCount> layers;
    double sampleRate = 48000.0;
    juce::CriticalSection lock;
};
