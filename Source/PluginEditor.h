#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "ChordDetector.h"
#include <array>
#include <atomic>

class ClassicPlayerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                private juce::Timer,
                                                private juce::MidiKeyboardState::Listener,
                                                private juce::AsyncUpdater
{
public:
    explicit ClassicPlayerAudioProcessorEditor(ClassicPlayerAudioProcessor&);
    ~ClassicPlayerAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class LevelMeter final : public juce::Component
    {
    public:
        void setLevel(float newLevel);
        void paint(juce::Graphics&) override;
    private:
        float level = 0.0f;
    };

    class NamedKeyboard final : public juce::MidiKeyboardComponent
    {
    public:
        explicit NamedKeyboard(juce::MidiKeyboardState&);
        void drawWhiteNote(int, juce::Graphics&, juce::Rectangle<float>, bool, bool,
                           juce::Colour, juce::Colour) override;
        void drawBlackNote(int, juce::Graphics&, juce::Rectangle<float>, bool, bool,
                           juce::Colour) override;
        void setActiveColour(juce::Colour colour);
    private:
        static juce::String noteLabel(int);
    };

    class DrumPadPanel final : public juce::Component
    {
    public:
        explicit DrumPadPanel(ClassicPlayerAudioProcessor&);
        void resized() override;
        void refresh();
        void setControlsVisible(bool shouldShow);
    private:
        void chooseSample(int pad);
        ClassicPlayerAudioProcessor& processor;
        std::array<juce::TextButton, ClassicPlayerAudioProcessor::drumPadCount> pads;
        std::array<juce::TextButton, ClassicPlayerAudioProcessor::drumPadCount> loadButtons;
        std::array<juce::TextButton, ClassicPlayerAudioProcessor::drumPadCount> learnButtons;
        std::unique_ptr<juce::FileChooser> fileChooser;
        bool controlsVisible = true;
    };

    class LayerStrip final : public juce::Component
    {
    public:
        LayerStrip(ClassicPlayerAudioProcessor&, int layerIndex, std::function<void()> mixChanged);
        void paint(juce::Graphics&) override;
        void resized() override;
        void refresh();
        void updateMeter();
        void refreshMidiDevices();
        void refreshExternalInstrumentLibrary();
        void closeExternalInstrumentEditor();
        void setEngineEnabled(bool);
        bool isMuted() const { return muted; }
        bool isSolo() const { return solo; }
        bool isExpanded() const { return expanded; }
        void setRemoveCallback(std::function<void()> callback) { removeLayerCallback = std::move(callback); }

    private:
        void chooseSoundFont();
        void chooseExternalInstrument();
        void chooseDx7();
        void deleteSelectedDx7Bank();
        void rebuildDx7Library();
        void rebuildDx7Patches();
        void updateSourceTypeVisibility();
        void openExternalInstrumentEditor();
        void deleteSelectedSoundFont();
        void resetLayer();
        void rebuildPresets();
        void rebuildLibrary();
        void rebuildExternalInstrumentLibrary();
        void applyConfig();
        void initialiseComboBoxes();
        void updateMidiLearnState();
        void showReverbEditor();
        void showCompressorEditor();
        void showChorusEditor();
        void showDrumPadEditor();
        void showLayerEditor();
        void showAnalogSynthEditor();
        void showDx7Editor();

        ClassicPlayerAudioProcessor& processor;
        const int index;
        std::function<void()> mixStateChanged;
        std::function<void()> removeLayerCallback;
        bool muted = false;
        bool solo = false;
        bool expanded = false;
        std::vector<Sf2Engine::Preset> presets;
        juce::Array<juce::File> libraryFiles;
        juce::Array<juce::File> externalInstrumentFiles;
        juce::Array<juce::File> dx7LibraryFiles;

        juce::Label layerTitle;
        juce::TextButton muteButton { "M" };
        juce::TextButton soloButton { "S" };
        juce::TextButton resetButton { "RESET" };
        juce::TextButton removeButton { "X" };
        juce::TextButton editButton { "EDITAR" };
        juce::TextButton loadButton { "IMPORTAR SF2" };
        juce::TextButton externalInstrumentButton { "CARREGAR VST" };
        juce::TextButton dx7Button { "IMPORTAR DX7" };
        juce::TextButton deleteDx7LibraryButton { "EXCLUIR DX7" };
        juce::TextButton openExternalEditorButton { "ABRIR EDITOR" };
        juce::ComboBox externalInstrumentBox;
        juce::ComboBox dx7LibraryBox;
        juce::ComboBox dx7PatchBox;
        juce::TextButton deleteLibraryButton { "EXCLUIR SF2" };
        juce::Label fileLabel;
        juce::Label sourceSummary;
        juce::ComboBox categoryBox;
        juce::ComboBox libraryBox;
        juce::ComboBox presetBox;
        juce::Slider gain;
        juce::Slider cutoff;
        juce::Slider reverb;
        juce::Slider compressor;
        juce::Label cutoffLabel;
        juce::Label reverbLabel;
        juce::Label compressorLabel;
        juce::Label routingLabel;
        juce::TextButton volumeLearn { "LEARN CC" };
        juce::TextButton resetMidiLearnButton { "RESET CC" };
        juce::TextButton cutoffLearn { "LEARN" };
        juce::TextButton reverbLearn { "LEARN" };
        juce::TextButton compressorLearn { "LEARN" };
        juce::TextButton reverbEditButton { "EDIT" };
        juce::TextButton compressorEditButton { "EDIT" };
        juce::TextButton chorusEditButton { "EDIT" };
        juce::Slider chorus;
        juce::Label chorusLabel;
        juce::ComboBox mode;
        juce::ComboBox sustain;
        juce::ComboBox midiDevice;
        juce::ComboBox midiChannel;
        juce::ComboBox octave;
        juce::ComboBox lowNote;
        juce::ComboBox highNote;
        juce::ComboBox velocityCurve;
        LevelMeter meter;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> cutoffAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compressorAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> chorusAttachment;
        std::unique_ptr<juce::FileChooser> fileChooser;
        std::unique_ptr<juce::DocumentWindow> externalEditorWindow;
        DrumPadPanel drumPadPanel;
        juce::String midiDeviceFingerprint;
        juce::Array<juce::MidiDeviceInfo> midiDevices;
    };

    void timerCallback() override;
    void handleNoteOn(juce::MidiKeyboardState*, int, int, float) override;
    void handleNoteOff(juce::MidiKeyboardState*, int, int, float) override;
    void handleAsyncUpdate() override;
    void applyMixerStates();
    void refreshProgramLibrary();
    void refreshExternalInstrumentLibrary();
    void saveProgram();
    void deleteSelectedProgram();
    void loadSelectedProgram();
    void refreshAfterProgramLoad();
    void showLiveSet(bool show);
    void refreshLiveSet();
    void chooseLiveSetSlot(int slot);
    void loadLiveSetSlot(int slot);
    void addLayer(ClassicPlayerAudioProcessor::LayerType type = ClassicPlayerAudioProcessor::LayerType::sf2);
    void removeLayer(int layer);
    void layoutLayerStrips();
    void activate();
    void showMasterEqEditor();
    juce::String detectedChord() const;

    ClassicPlayerAudioProcessor& classicProcessor;
    juce::Label title;
    juce::Label subtitle;
    juce::Label chordLabel;
    juce::Label chordCaption;
    juce::TextButton chordColourButton { "COR ACORDE" };
    juce::TextButton keyColourButton { "COR TECLAS" };
    juce::TextButton refreshExternalInstrumentButton { "ATUALIZAR VST" };
    juce::ComboBox accidentalStyleBox;
    juce::ComboBox programBox;
    juce::TextButton saveProgramButton { "SALVAR" };
    juce::TextButton deleteProgramButton { "EXCLUIR" };
    juce::TextButton loadProgramButton { "CARREGAR" };
    juce::TextButton addLayerButton { "+ LAYER" };
    juce::TextButton recordingButton { "GRAVAR WAV" };
    juce::Label recordingStatus;
    juce::TextButton liveSetButton { "LIVE SET" };
    juce::TextButton editLiveSetButton { "EDITAR LIVE SET" };
    std::array<juce::TextButton, ClassicPlayerAudioProcessor::liveSetBankCount> liveSetBankButtons;
    std::array<juce::TextButton, ClassicPlayerAudioProcessor::liveSetSlotsPerBank> liveSetSlotButtons;
    std::array<juce::TextButton, ClassicPlayerAudioProcessor::liveSetSlotsPerBank> liveSetSlotLearnButtons;
    juce::Array<juce::File> programFiles;
    juce::Slider master;
    juce::Label masterLabel;
    juce::TextButton masterEqButton { "EQ MASTER" };
    juce::TextButton masterLearnButton { "LEARN" };
    LevelMeter masterMeter;
    juce::ImageComponent appIcon;
    juce::ImageComponent classicKeysLogo;
    juce::ImageComponent willamSilvaLogo;
    juce::Colour chordColour { 0xffffd84a };
    ClassicChordDetector::AccidentalStyle accidentalStyle { ClassicChordDetector::AccidentalStyle::mixed };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttachment;
    juce::Viewport layerViewport;
    juce::Component layerContent;
    std::array<std::unique_ptr<LayerStrip>, Sf2Engine::layerCount> strips;
    NamedKeyboard keyboard;

    juce::Component activationPanel;
    juce::Label activationTitle;
    juce::Label activationHelp;
    juce::TextEditor activationCode;
    juce::TextButton activationButton { "ATIVAR" };
    juce::Label activationStatus;
    std::array<std::atomic<bool>, 128> heldNotes {};
    int timerTicks = 0;
    int displayedLayerCount = Sf2Engine::defaultLayerCount;
    int activeLiveSetBank = 0;
    int activeLiveSetSlot = -1;
    bool showingLiveSet = false;
    bool editingLiveSet = false;
    juce::int64 recordingStartedAtMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassicPlayerAudioProcessorEditor)
};
