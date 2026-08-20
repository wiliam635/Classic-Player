#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Sf2Engine.h"
#include "Dx7Engine.h"
#include "AnalogSynthEngine.h"
#include "ExternalInstrumentHost.h"
#include <atomic>

class ClassicPlayerAudioProcessor final : public juce::AudioProcessor,
                                          private juce::MidiInputCallback
{
public:
    enum class LearnTarget { volume = 0, cutoff, reverb, compressor, release, count };
    // A source is chosen only when a new layer is created. The initial four
    // layers are SF2 by design.
    enum class LayerType { sf2 = 0, vst, dx7, analog, drumPads };
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

    // Hosting is intentionally available only to the standalone application.
    // The Classic Player VST3/AU remains an instrument for the DAW, not a host.
    bool supportsExternalInstruments() const noexcept;
    juce::Result loadExternalInstrument(int layer, const juce::File& file);
    void unloadExternalInstrument(int layer);
    bool hasExternalInstrument(int layer) const;
    juce::String externalInstrumentName(int layer) const;
    juce::AudioProcessorEditor* createExternalInstrumentEditor(int layer);
    juce::Array<juce::File> availableExternalInstruments() const;
    void refreshExternalInstrumentLibrary();

    juce::String soundFontPath(int layer) const;
    Sf2Engine::LayerConfig layerConfig(int layer) const;
    void setLayerConfig(int layer, const Sf2Engine::LayerConfig& config);
    std::vector<Sf2Engine::Preset> layerPresets(int layer) const;
    void selectLayerPreset(int layer, int bank, int program);
    void sendLayerController(int layer, int controller, int value);
    float layerPeak(int layer) const;
    int activeLayerCount() const { return activeLayers.load(std::memory_order_relaxed); }
    bool addLayer(LayerType type = LayerType::sf2);
    bool removeLayer(int layer);
    LayerType layerType(int layer) const;
    void setLayerType(int layer, LayerType type);
    juce::Result loadDx7(int layer, const juce::File& file);
    void unloadDx7(int layer);
    bool hasDx7(int layer) const;
    bool hasAnalogSynth(int layer) const;
    AnalogSynthEngine::Config analogSynthConfig(int layer) const;
    void setAnalogSynthConfig(int layer, const AnalogSynthEngine::Config& config);
    juce::String dx7PatchName(int layer) const;
    juce::String dx7PatchName(int layer, int patch) const;
    int dx7PatchCount(int layer) const;
    int dx7SelectedPatch(int layer) const;
    bool selectDx7Patch(int layer, int patch);
    juce::String dx7Path(int layer) const;
    juce::Result importDx7Bank(const juce::File&, juce::File& importedFile) const;
    juce::Array<juce::File> libraryDx7Banks() const;
    juce::Result deleteLibraryDx7Bank(const juce::File&);

    // Records the final stereo mix to the Desktop without blocking the audio callback.
    juce::Result startAudioRecording();
    void stopAudioRecording();
    bool isAudioRecording() const noexcept;
    juce::String recordingFilePath() const;

    // Three-band master EQ: low/high shelves plus a variable mid bell.
    float masterEqValue(const juce::String& parameterId) const;
    void setMasterEqValue(const juce::String& parameterId, float value);
    void beginMidiLearn(int layer, LearnTarget target);
    void resetMidiLearn(int layer);
    int midiLearnCC(int layer, LearnTarget target) const;
    int midiLearnChannel(int layer, LearnTarget target) const;
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

    // Persistent Classic Player programs are standalone files, independent
    // from DAW session state, and work on both macOS and Windows.
    juce::Array<juce::File> savedPrograms() const;
    juce::Result saveProgram(const juce::String& name, juce::File& savedFile);
    juce::Result deleteProgram(const juce::File& programFile);
    juce::Result loadProgram(const juce::File& programFile);

    // Live Set stores only references to saved programs. Instruments are loaded
    // only when a Performance is selected, never when a bank is opened.
    static constexpr int liveSetBankCount = 8;
    static constexpr int liveSetSlotsPerBank = 8;
    juce::File liveSetSlotProgram(int bank, int slot) const;
    juce::String liveSetSlotName(int bank, int slot) const;
    juce::String liveSetSlotLayerSummary(int bank, int slot) const;
    juce::Result assignLiveSetSlot(int bank, int slot, const juce::File& programFile);
    void clearLiveSetSlot(int bank, int slot);
    juce::Result loadLiveSetSlot(int bank, int slot);
    void beginLiveSetSlotMidiLearn(int bank, int slot);
    void resetLiveSetSlotMidiLearn(int bank, int slot);
    int liveSetSlotMidiLearnCC(int bank, int slot) const;
    int liveSetSlotMidiLearnChannel(int bank, int slot) const;
    bool isLiveSetSlotMidiLearning(int bank, int slot) const;
    int consumeRequestedLiveSetSlot();
    bool consumeLiveSetSlotMidiLearnChanged();
    void saveLiveSetSlotMidiLearnState() const;

    // Twelve independent drum pads. Pads are triggered by their own UI/MIDI
    // mapping and never enter the melodic keyboard/layer routing.
    static constexpr int drumPadCount = 8;
    juce::String drumPadName(int pad) const;
    juce::String drumPadPath(int pad) const;
    int drumPadMidiCC(int pad) const;
    bool isDrumPadPlaying(int pad) const;
    void triggerDrumPad(int pad);
    void beginDrumPadMidiLearn(int pad);
    bool isDrumPadMidiLearning(int pad) const;
    juce::Result loadDrumPad(int pad, const juce::File& file);

    void refreshActivation();
    bool isActivated() const { return activated.load(); }
    juce::AudioProcessorValueTreeState parameters;
    juce::MidiKeyboardState keyboardState;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void restoreLayerPaths();
    void stopAllSoundsBeforeProgramChange();
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void processMidiControlMessage(const juce::MidiMessage&, int layerFilter = -1);
    void processLiveSetSlotMidiMessage(const juce::MidiMessage&);
    void renderExternalInstruments(juce::AudioBuffer<float>&, const juce::MidiBuffer&);
    void appendExternalMidi(int layer, const juce::MidiBuffer&, juce::MidiBuffer&);
    int liveSetIndex(int bank, int slot) const;
    juce::File liveSetStorageFile() const;
    void loadLiveSetState();
    void saveLiveSetState() const;
    void updateMasterEq();
    void processDrumPads(juce::AudioBuffer<float>&, const juce::MidiBuffer&);

    Sf2Engine engine;
    Dx7Engine dx7Engine;
    AnalogSynthEngine analogSynthEngine;
    // Stored independently from the audio parameters so switching a source
    // never changes the layer routing, split or controller assignments.
    std::array<std::atomic<int>, Sf2Engine::layerCount> layerTypes {};
    std::array<ExternalInstrumentHost, Sf2Engine::layerCount> externalInstruments;
    juce::Array<juce::File> externalInstrumentLibrary;
    std::array<juce::AudioBuffer<float>, Sf2Engine::layerCount> externalScratch;
    std::array<juce::MidiBuffer, Sf2Engine::layerCount> externalMidi;
    std::array<Sf2Engine::LayerConfig, Sf2Engine::layerCount> dx7LayerConfigs {};
    std::array<AnalogSynthEngine::Config, Sf2Engine::layerCount> analogLayerConfigs {};
    std::array<std::atomic<float>, Sf2Engine::layerCount> externalPeaks {};
    std::array<int, Sf2Engine::layerCount> lastExternalPortamento {};
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    juce::dsp::Limiter<float> outputLimiter;
    std::array<juce::dsp::IIR::Filter<float>, 2> masterEqLowCut;
    std::array<juce::dsp::IIR::Filter<float>, 2> masterEqLow;
    std::array<juce::dsp::IIR::Filter<float>, 2> masterEqMid;
    std::array<juce::dsp::IIR::Filter<float>, 2> masterEqHigh;
    std::array<juce::dsp::IIR::Filter<float>, 2> masterEqHighCut;
    std::array<float, 8> lastMasterEqValues {
        -999.0f, -999.0f, -999.0f, -999.0f,
        -999.0f, -999.0f, -999.0f, -999.0f
    };
    juce::TimeSliceThread recordingThread { "Classic Player WAV Writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recordingWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeRecordingWriter { nullptr };
    juce::File recordingFile;
    std::array<juce::String, Sf2Engine::layerCount> savedPaths;
    static constexpr int learnTargetCount = static_cast<int>(LearnTarget::count);
    // A MIDI controller is identified by CC *and* channel. Some keyboards
    // send the same CC from several faders on different Part channels.
    std::array<std::array<std::atomic<int>, learnTargetCount>, Sf2Engine::layerCount> learnedCCs {};
    std::array<std::array<std::atomic<int>, learnTargetCount>, Sf2Engine::layerCount> learnedChannels {};
    std::array<std::array<std::atomic<float>, learnTargetCount>, Sf2Engine::layerCount> pendingCCValues {};
    std::atomic<int> activeMidiLearn { -1 };
    static constexpr int liveSetSlotCount = liveSetBankCount * liveSetSlotsPerBank;
    std::array<std::atomic<int>, liveSetSlotCount> learnedLiveSetSlotCCs {};
    std::array<std::atomic<int>, liveSetSlotCount> learnedLiveSetSlotChannels {};
    std::atomic<int> activeLiveSetSlotMidiLearn { -1 };
    std::atomic<int> requestedLiveSetSlot { -1 };
    std::atomic<bool> liveSetSlotMidiLearnChanged { false };
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
    std::array<juce::String, liveSetBankCount * liveSetSlotsPerBank> liveSetPrograms;
    mutable juce::CriticalSection liveSetLock;
    struct DrumPadState
    {
        juce::AudioBuffer<float> audio;
        juce::String path;
        std::atomic<int> trigger { 0 };
        std::atomic<int> position { -1 };
        std::atomic<int> midiCC { -1 };
        std::atomic<bool> learning { false };
    };
    std::array<DrumPadState, drumPadCount> drumPads;
    juce::AudioFormatManager drumPadFormats;
    mutable juce::CriticalSection drumPadLock;
    std::atomic<bool> activated { false };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassicPlayerAudioProcessor)
};
