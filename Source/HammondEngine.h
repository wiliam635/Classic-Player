#pragma once
#include "Sf2Engine.h"
#include "AudioTransition.h"
#include <juce_data_structures/juce_data_structures.h>

// Native port of Hammond Lab revision 4: no browser, keyboard or chorus.
class HammondEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;
    struct Config
    {
        Sf2Engine::LayerConfig routing;
        std::array<int,9> bars {8,8,7,6,8,5,4,3,2};
        int leslie = 1, percussion = 0, preset = 0;
        float click = .15f, leakage = .12f, drive = .08f, level = .75f;
        std::array<int,11> cc {{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}};
        std::array<int,11> channel {};
        int learning = -1; // 0..8 drawbars, 9 Leslie, 10 instrument level
    };
    static constexpr std::array<double,13> ratios { .5,1.5,1,2,3,4,5,6,8,
        .9438743126816935,1.0594630943592953,1.4983070768766815,2 };
    static juce::StringArray presetNames();
    static Config preset(int index);
    static Config validated(Config);
    static juce::ValueTree save(const Config&, int layer);
    static Config restore(const juce::ValueTree&);
    void prepare(double rate, int block);
    void unload(int layer);
    void stopAllSounds();
    float getLayerPeak(int layer) const { return peaks[(size_t)layer].load(); }
    void process(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                 std::array<Config,layerCount>&,
                 const std::array<juce::MidiBuffer,layerCount>* routed = nullptr);
private:
    struct Voice
    {
        bool active = false, down = false, releasing = false;
        int note = 0, channel = 0, age = 0, releaseAge = 0;
        float envelope = 0, releaseStart = 0, percussion = 0, click = 0;
        double perPhase = 0, perStep = 0;
        std::array<double,13> phase {}, step {};
        AudioTransition transition;
    };
    struct Layer
    {
        std::array<Voice,80> voices {};
        std::array<bool,16> sustain {};
        std::array<float,16> volume {}, expression {};
        std::array<juce::SmoothedValue<float>,16> channelGain;
        std::array<juce::SmoothedValue<float>,9> bars;
        juce::SmoothedValue<float> gain, drive, leakage, level, pan, rotaryDepth;
        std::array<juce::SmoothedValue<float>,2> speed;
        std::array<double,2> rotorPhase {};
        std::array<std::vector<float>,2> delay;
        int delayPosition = 0;
        float crossover = 0;
        std::array<float,2> lowpass {}, compressor {};
        juce::Reverb reverb;
        uint64_t clock = 0;
        uint32_t noise = 0x1341257u;
        bool enabled = false;
    };
    void message(int layer, const juce::MidiMessage&, Config&);
    void release(Voice&);
    float renderVoice(Voice&, Layer&, const std::array<float,9>&, float leak);
    float sine(double phase) const;
    std::array<Layer,layerCount> layers;
    std::array<std::atomic<float>,layerCount> peaks {};
    std::array<float,4097> sineTable {};
    juce::AudioBuffer<float> scratch;
    double sampleRate = 48000;
};
