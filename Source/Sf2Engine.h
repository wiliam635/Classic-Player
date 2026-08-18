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
        bool mono = false; // Mono legato: one voice at a time, without a gap.
        bool portamento = false;
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
        // The front-panel REVERB and COMP knobs are the effect mix amounts.
        // Their detailed parameters are kept independently so SF2 and DX7
        // routing remains source-specific and predictable.
        float reverb = 0.0f;
        float reverbSize = 55.0f;
        float reverbDamping = 45.0f;
        float reverbWidth = 100.0f;
        float compressor = 0.0f;
        float compressorThreshold = -18.0f;
        float compressorRatio = 4.0f;
        float compressorAttack = 10.0f;
        float compressorRelease = 120.0f;
        float compressorMakeup = 0.0f;
        // Used only by DX7 layers.  Keeping it in the common routing config
        // preserves source-specific controls without mixing their engines.
        float dx7Chorus = 20.0f;
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
    // Sends an explicit panic to every synth before a performance is replaced.
    void stopAllSounds();
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
        int monoChannel = 1;
        bool sustainDown = false;
        std::array<bool, 128> heldNotes {};
        std::array<float, 128> heldVelocities {};
        int lastCutoff = -1;
        int lastReverb = -1;
        int lastRelease = -1;
        int lastPortamento = -1;
        int lastMono = -1;
        float modulationAmount = 0.0f;
        double modulationPhase = 0.0;
        std::array<float, 2> filterState { 0.0f, 0.0f };
        std::array<float, 2> compressorEnvelope { 0.0f, 0.0f };
        juce::Reverb nativeReverb;
        std::array<float, 4> lastReverbParameters { -1.0f, -1.0f, -1.0f, -1.0f };
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
