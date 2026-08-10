#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "Sf2Engine.h"
#include <atomic>

class ClassicPlayerAudioProcessor final : public juce::AudioProcessor,
                                          private juce::MidiInputCallback
{
public:
    enum class LearnTarget { volume = 0, cutoff, reverb, compressor, release, count };
    ClassicPlayerAudioProcessor();
    ~ClassicPlayerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::Result loadSoundFont(int layer, const juce::File& file);
    void unloadSoundFont(int layer);
    juce::String soundFontPath(int layer) const;
    Sf2Engine::LayerConfig layerConfig(int layer) const;
    void setLayerConfig(int layer, const Sf2Engine::LayerConfig& config);
    std::vector<Sf2Engine::Preset> layerPresets(int layer) const;
    void selectLayerPreset(int layer, int bank, int program);
    void sendLayerController(int layer, int controller, int value);
    float layerPeak(int layer) const;
    int activeLayerCount() const { return activeLayers.load(std::memory_order_relaxed); }
    bool addLayer();
    bool removeLayer(int layer);
    void beginMidiLearn(int layer, LearnTarget target);
    int midiLearnCC(int layer, LearnTarget target) const;
    bool isMidiLearning(int layer, LearnTarget target) const;
    void consumeMidiControlUpdates();
    void attachStandaloneMidiRouting(juce::AudioDeviceManager&, juce::MidiInputCallback& defaultCallback);
    void refreshStandaloneMidiInputs();
    juce::Array<juce::MidiDeviceInfo> availableMidiDevices() const;
    void setLayerMidiDevice(int layer, const juce::String& identifier);
    juce::String layerMidiDevice(int layer) const;
    static juce::StringArray soundFontCategories();
    juce::Result importSoundFont(const juce::File&, const juce::String& category,
                                 juce::File& importedFile) const;
    juce::Array<juce::File> librarySoundFonts(const juce::String& category) const;
    juce::Result deleteLibrarySoundFont(const juce::File&);
    void refreshActivation();
    bool isActivated() const { return activated.load(); }
    juce::AudioProcessorValueTreeState parameters;
    juce::MidiKeyboardState keyboardState;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void restoreLayerPaths();
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void processMidiControlMessage(const juce::MidiMessage&, int layerFilter = -1);

    Sf2Engine engine;
    juce::dsp::Limiter<float> outputLimiter;
    std::array<juce::String, Sf2Engine::layerCount> savedPaths;
    static constexpr int learnTargetCount = static_cast<int>(LearnTarget::count);
    // A MIDI controller is identified by CC *and* channel. Some keyboards
    // send the same CC from several faders on different Part channels.
    std::array<std::array<std::atomic<int>, learnTargetCount>, Sf2Engine::layerCount> learnedCCs {};
    std::array<std::array<std::atomic<int>, learnTargetCount>, Sf2Engine::layerCount> learnedChannels {};
    std::array<std::array<std::atomic<float>, learnTargetCount>, Sf2Engine::layerCount> pendingCCValues {};
    std::atomic<int> activeMidiLearn { -1 };
    std::array<juce::MidiMessageCollector, Sf2Engine::layerCount> routedMidiCollectors;
    std::array<juce::MidiBuffer, Sf2Engine::layerCount> routedMidiBuffers;
    juce::MidiMessageCollector visualMidiCollector;
    juce::MidiBuffer visualMidiBuffer;
    std::array<juce::String, Sf2Engine::layerCount> layerMidiDeviceIds;
    std::atomic<int> activeLayers { Sf2Engine::defaultLayerCount };
    mutable juce::CriticalSection midiRoutingLock;
    juce::AudioDeviceManager* standaloneDeviceManager = nullptr;
    juce::MidiInputCallback* standaloneDefaultMidiCallback = nullptr;
    juce::String registeredMidiFingerprint;
    juce::StringArray registeredMidiInputIds;
    std::atomic<bool> activated { false };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassicPlayerAudioProcessor)
};
