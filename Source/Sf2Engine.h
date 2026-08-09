#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <fluidsynth.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class Sf2Engine
{
public:
    static constexpr int defaultLayerCount = 4;
    static constexpr int layerCount = 8;

    struct LayerConfig
    {
        bool enabled = true;
        bool mono = false;
        bool sustainEnabled = true;
        int midiChannel = 0; // 0 = omni, 1..16 = fixed
        int lowNote = 0;
        int highNote = 127;
        int octave = 0;
        int velocityCurve = 0; // 0 linear, 1 soft, 2 hard
        float gain = 0.8f;
        float pan = 0.0f;
        float release = 50.0f;
        float cutoff = 100.0f;
        float reverb = 0.0f;
        float compressor = 0.0f;
    };

    struct Preset
    {
        juce::String name;
        int bank = 0;
        int program = 0;
    };

    Sf2Engine();
    ~Sf2Engine();

    void prepare(double sampleRate, int maximumBlockSize);
    void reset();
    juce::Result loadSoundFont(int layer, const juce::File& file);
    void unloadSoundFont(int layer);
    void process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                 const std::array<juce::MidiBuffer, layerCount>* routedMidi = nullptr);

    LayerConfig getConfig(int layer) const;
    void setConfig(int layer, const LayerConfig& config);
    juce::String getSoundFontPath(int layer) const;
    std::vector<Preset> getPresets(int layer) const;
    void selectPreset(int layer, int bank, int program);
    void sendController(int layer, int controller, int value);
    float getLayerPeak(int layer) const;

private:
    struct SettingsDeleter { void operator()(fluid_settings_t* p) const { if (p) delete_fluid_settings(p); } };
    struct SynthDeleter { void operator()(fluid_synth_t* p) const { if (p) delete_fluid_synth(p); } };

    struct Layer
    {
        std::unique_ptr<fluid_settings_t, SettingsDeleter> settings;
        std::unique_ptr<fluid_synth_t, SynthDeleter> synth;
        LayerConfig config;
        juce::String soundFontPath;
        int soundFontId = -1;
        int selectedBank = 0;
        int selectedProgram = 0;
        int monoNote = -1;
        int lastCutoff = -1;
        int lastReverb = -1;
        int lastRelease = -1;
        float modulationAmount = 0.0f;
        double modulationPhase = 0.0;
        std::array<float, 2> filterState { 0.0f, 0.0f };
        std::atomic<float> peak { 0.0f };
    };

    void createSynth(Layer& layer);
    void dispatchMidi(Layer& layer, const juce::MidiMessage& message);

    std::array<Layer, layerCount> layers;
    juce::AudioBuffer<float> scratch;
    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    mutable juce::CriticalSection lock;
};
