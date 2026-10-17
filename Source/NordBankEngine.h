#pragma once
#include "NordProgramDecoder.h"
#include "SampleEngine.h"
#include "HammondEngine.h"
#include <memory>

class NordBankEngine
{
public:
    struct ProgramInfo {
        juce::File file;
        juce::String name;
        std::array<bool, 2> playable {};
        std::array<juce::String, 2> reason;
        int scene = 0;
    };
    static juce::File libraryRoot();
    static juce::Array<juce::File> banks();
    static std::vector<ProgramInfo> programs(const juce::File& bank);
    void prepare(double sampleRate, int blockSize);
    juce::Result load(int layer, const juce::File& program, int scene = -1);
    void unload(int layer);
    void stopAllSounds();
    bool isLoaded(int layer) const;
    juce::String path(int layer) const;
    juce::String description(int layer) const;
    int scene(int layer) const;
    float peak(int layer) const;
    void process(int layer, juce::AudioBuffer<float>&, const juce::MidiBuffer& host,
                 const juce::MidiBuffer& routed, const Sf2Engine::LayerConfig&);
private:
    struct State;
    struct Slot {
        mutable juce::CriticalSection lock;
        std::unique_ptr<State> state;
        std::atomic<float> peak {};
        Slot();
        ~Slot();
    };
    std::array<Slot, Sf2Engine::layerCount> slots;
    std::atomic<double> rate { 44100.0 };
    std::atomic<int> block { 512 };
};
