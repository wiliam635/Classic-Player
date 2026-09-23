#include "PluginProcessor.h"
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static void cc(ClassicPlayerAudioProcessor& processor, int channel, int number, int value)
{
    juce::AudioBuffer<float> audio(2, 128);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(channel, number, value), 0);
    processor.processBlock(audio, midi);
    processor.consumeMidiControlUpdates();
}

static void ccAudioOnly(ClassicPlayerAudioProcessor& processor, int channel, int number, int value)
{
    juce::AudioBuffer<float> audio(2, 128);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(channel, number, value), 0);
    processor.processBlock(audio, midi);
}

static void startupPrograms()
{
    juce::TemporaryFile storage;
    const auto root = storage.getFile();
    check(root.createDirectory().wasOk(), "test storage");
    struct Cleanup { juce::File directory; ~Cleanup() { directory.deleteRecursively(); } } cleanup { root };
    const auto open = [&root](bool standalone)
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(standalone
            ? juce::AudioProcessor::wrapperType_Standalone : juce::AudioProcessor::wrapperType_Undefined);
        auto p = std::make_unique<ClassicPlayerAudioProcessor>(root);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        return p;
    };
    auto p = open(true);
    p->prepareToPlay(48000, 128);
    p->parameters.getParameter("master")->setValueNotifyingHost(0.25f);
    juce::File first, last;
    check(p->saveProgram("First", first).wasOk(), "save first");
    p->parameters.getParameter("master")->setValueNotifyingHost(0.33f);
    juce::File portable;
    check(p->saveProgramToFile(root.getChildFile("PortablePreset"), portable).wasOk(),
          "save portable preset");
    check(portable.getFileExtension().toLowerCase() == ".ckprogram" && portable.existsAsFile(),
          "portable preset extension");
    p->parameters.getParameter("master")->setValueNotifyingHost(0.66f);
    check(p->loadProgram(portable).wasOk()
          && p->parameters.getRawParameterValue("master")->load() == 33,
          "load portable preset");
    p->parameters.getParameter("master")->setValueNotifyingHost(0.75f);
    while (p->activeLayerCount() > 4)
        check(p->removeLayer(p->activeLayerCount() - 1), "remove layer for startup test");
    const int savedChannels[] = {1, 0, 10, 16};
    for (int layer = 0; layer < 4; ++layer)
    {
        auto config = p->layerConfig(layer);
        config.midiChannel = savedChannels[layer];
        p->setLayerConfig(layer, config);
    }
    check(p->saveProgram("Last", last).wasOk(), "save last");
    check(p->loadProgram(first).wasOk(), "load older program");
    check(p->activeLayerCount() == 6, "older program layer count");
    juce::MemoryBlock unsaved;
    p->getStateInformation(unsaved);
    p.reset();
    p = open(true);
    p->setStateInformation(unsaved.getData(), static_cast<int>(unsaved.getSize()));
    p->prepareToPlay(48000, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 75, "startup did not restore last SAVED program");
    check(p->currentSavedProgramName() == "Last", "restored program name");
    check(p->activeLayerCount() == 4, "startup expanded four layers to six");
    for (int layer = 0; layer < 4; ++layer)
        check(p->layerConfig(layer).midiChannel == savedChannels[layer], "startup lost layer MIDI channel");
    p->parameters.getParameter("master")->setValueNotifyingHost(0.42f);
    p->prepareToPlay(44100, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 42, "device restart reloaded program");
    check(p->activeLayerCount() == 4 && p->layerConfig(0).midiChannel == 1,
          "device restart lost layer routing");
    p.reset();
    p = open(false);
    p->prepareToPlay(48000, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 80, "plugin loaded standalone program");
    p.reset();
    check(last.deleteFile(), "remove test program");
    p = open(true);
    p->prepareToPlay(48000, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 80, "missing program fallback unsafe");
    p.reset();
    std::cout << "Last-saved restore, program name, device restart, plugin isolation and missing file passed\n";
}

static void programLibraryExportImport()
{
    juce::TemporaryFile sourceStorage, destinationStorage;
    const auto sourceRoot = sourceStorage.getFile();
    const auto destinationRoot = destinationStorage.getFile();
    check(sourceRoot.createDirectory().wasOk() && destinationRoot.createDirectory().wasOk(),
          "preset import/export storage");
    struct Cleanup
    {
        juce::File first, second;
        ~Cleanup() { first.deleteRecursively(); second.deleteRecursively(); }
    } cleanup { sourceRoot, destinationRoot };

    ClassicPlayerAudioProcessor sourceProcessor(sourceRoot);
    juce::File internalFile;
    check(sourceProcessor.saveProgram("Internal Performance", internalFile).wasOk(),
          "save performance to internal library");
    check(internalFile.getParentDirectory().getFileName() == "Programs"
          && sourceProcessor.savedPrograms().size() == 1,
          "saved performance missing from internal library");

    juce::File exportedFile;
    check(sourceProcessor.exportProgramToFile(sourceRoot.getChildFile("Portable.ckprogram"),
                                               exportedFile).wasOk(),
          "export portable performance");
    check(exportedFile.existsAsFile()
          && sourceProcessor.currentSavedProgramName() == "Internal Performance"
          && sourceProcessor.savedPrograms().size() == 1,
          "export changed current library performance");

    ClassicPlayerAudioProcessor destinationProcessor(destinationRoot);
    juce::File importedFile;
    check(destinationProcessor.importProgramFromFile(exportedFile, importedFile).wasOk(),
          "import portable performance");
    check(importedFile.existsAsFile()
          && importedFile.getParentDirectory().getFileName() == "Programs"
          && destinationProcessor.savedPrograms().size() == 1,
          "imported performance missing from destination library");
    check(destinationProcessor.loadProgram(importedFile).wasOk()
          && destinationProcessor.currentSavedProgramName() == "Portable",
          "imported performance could not be opened");
    std::cout << "Internal library save, portable export and import passed\n";
}

static void livePerformanceEqPersistence()
{
    juce::TemporaryFile storage;
    const auto root = storage.getFile();
    check(root.createDirectory().wasOk(), "EQ program storage");
    struct Cleanup { juce::File directory; ~Cleanup() { directory.deleteRecursively(); } } cleanup { root };
    auto processor = std::make_unique<ClassicPlayerAudioProcessor>(root);
    const auto setParameter = [&processor](const juce::String& id, float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        check(parameter != nullptr, "EQ parameter missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    const auto parameterValue = [&processor](const juce::String& id)
    {
        return processor->parameters.getRawParameterValue(id)->load();
    };
    setParameter("layer1EqLow", 6.5f);
    setParameter("layer1EqMidFrequency", 935.0f);
    setParameter("layer1EqHighQ", 1.35f);
    juce::File first, second;
    check(processor->saveProgramToFile(root.getChildFile("First EQ"), first).wasOk(),
          "save first EQ performance");
    setParameter("layer1EqLow", -3.0f);
    setParameter("layer1EqMidFrequency", 2400.0f);
    check(processor->saveProgramToFile(root.getChildFile("Second EQ"), second).wasOk(),
          "save second EQ performance");
    check(processor->loadProgram(second).wasOk(), "load second EQ performance");
    check(std::abs(parameterValue("layer1EqLow") + 3.0f) < 0.11f
          && std::abs(parameterValue("layer1EqMidFrequency") - 2400.0f) < 1.1f,
          "second performance EQ state changed");
    check(processor->loadProgram(first).wasOk(), "return to first EQ performance");
    check(std::abs(parameterValue("layer1EqLow") - 6.5f) < 0.11f
          && std::abs(parameterValue("layer1EqMidFrequency") - 935.0f) < 1.1f
          && std::abs(parameterValue("layer1EqHighQ") - 1.35f) < 0.011f,
          "returning to a saved performance reset its layer EQ");
}

static void newProgramStartsBlank()
{
    juce::TemporaryFile storage;
    const auto root = storage.getFile();
    check(root.createDirectory().wasOk(), "new-program storage");
    struct Cleanup { juce::File directory; ~Cleanup() { directory.deleteRecursively(); } } cleanup { root };
    const auto open = [&root]
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        auto processor = std::make_unique<ClassicPlayerAudioProcessor>(root);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        processor->prepareToPlay(48000, 128);
        return processor;
    };

    auto processor = open();
    processor->beginMasterMidiLearn();
    cc(*processor, 2, 21, 127);
    processor->beginPanicMidiLearn();
    cc(*processor, 3, 22, 127);
    juce::File saved;
    check(processor->saveProgram("Saved performance", saved).wasOk(), "save before new project");

    processor->resetToNewProgram();
    check(processor->activeLayerCount() == 0 && processor->currentSavedProgramName().isEmpty(),
          "new project kept an existing layer or name");
    check(processor->masterMidiLearnCC() == 21 && processor->masterMidiLearnChannel() == 2
          && processor->panicMidiLearnCC() == 22 && processor->panicMidiLearnChannel() == 3,
          "new project cleared global controller mappings");
    check(std::abs(processor->parameters.getRawParameterValue("master")->load() - 80.0f) < 0.01f,
          "new project did not restore parameter defaults");

    processor.reset();
    processor = open();
    check(processor->activeLayerCount() == 0 && processor->currentSavedProgramName().isEmpty(),
          "reopening after a new project restored the previous performance");
    check(processor->masterMidiLearnCC() == 21 && processor->panicMidiLearnCC() == 22,
          "reopening after a new project lost global controller mappings");
}

struct MidiRecordingRegressionAccess
{
    static void run()
    {
        ClassicPlayerAudioProcessor processor;
        processor.prepareToPlay(48000, 256);
        const auto midiPath = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("Classic-Player-MIDI-regression", ".mid", false);
        struct Cleanup { juce::File file; ~Cleanup() { file.deleteFile(); } } cleanup { midiPath };
        processor.midiRecordingFile = midiPath;
        processor.recordedMidiEventCount = 0;
        processor.recordedMidiSamples = 96000;
        processor.midiRecordingOverflowed = false;

        juce::MidiBuffer source;
        source.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), 0);
        source.addEvent(juce::MidiMessage::controllerEvent(2, 1, 96), 120);
        source.addEvent(juce::MidiMessage::noteOff(2, 60), 240);
        processor.recordMidiBuffer(source, 48000);
        check(processor.writeRecordedMidiFile(), "write simultaneous MIDI file");

        juce::FileInputStream input(midiPath);
        juce::MidiFile midi;
        check(input.openedOk() && midi.readFrom(input), "read simultaneous MIDI file");
        check(midi.getTimeFormat() == 960 && midi.getNumTracks() == 1, "MIDI format");
        const auto* track = midi.getTrack(0);
        check(track != nullptr, "MIDI track");
        auto notes = 0;
        auto controllers = 0;
        double firstNoteTick = -1.0;
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& message = track->getEventPointer(i)->message;
            if (message.isNoteOn()) { ++notes; firstNoteTick = message.getTimeStamp(); }
            if (message.isController()) ++controllers;
        }
        check(notes == 1 && controllers == 1, "recorded MIDI messages");
        check(std::abs(firstNoteTick - 1920.0) < 0.01, "sample-accurate MIDI timestamp");
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    try
    {
        startupPrograms();
        programLibraryExportImport();
        livePerformanceEqPersistence();
        newProgramStartsBlank();
        MidiRecordingRegressionAccess::run();
        // Undefined wrapper deliberately avoids standalone preferences/programs.
        auto processor = std::make_unique<ClassicPlayerAudioProcessor>();
        processor->prepareToPlay(48000, 128);
        processor->beginMasterMidiLearn();
        cc(*processor, 3, 64, 127);
        check(processor->isMasterMidiLearning(), "sustain consumed learn");
        cc(*processor, 3, 123, 0);
        check(processor->isMasterMidiLearning(), "channel-mode message consumed learn");
        cc(*processor, 3, 11, 127);
        check(processor->masterMidiLearnCC() == 11 && processor->masterMidiLearnChannel() == 3, "mapping");
        check(processor->parameters.getRawParameterValue("master")->load() == 100, "full volume");
        cc(*processor, 2, 11, 0);
        check(processor->parameters.getRawParameterValue("master")->load() == 100, "wrong channel accepted");
        cc(*processor, 3, 11, 0);
        check(processor->parameters.getRawParameterValue("master")->load() == 0, "zero volume");
        juce::MemoryBlock saved;
        processor->getStateInformation(saved);
        processor.reset();
        processor = std::make_unique<ClassicPlayerAudioProcessor>();
        processor->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        processor->prepareToPlay(48000, 128);
        check(processor->masterMidiLearnCC() == 11 && processor->masterMidiLearnChannel() == 3, "mapping persistence");
        cc(*processor, 3, 11, 127);
        check(processor->parameters.getRawParameterValue("master")->load() == 100, "restored mapping response");
        processor->resetMasterMidiLearn();
        cc(*processor, 3, 11, 0);
        check(processor->parameters.getRawParameterValue("master")->load() == 100, "reset mapping still active");

        processor->setLayerType(0, ClassicPlayerAudioProcessor::LayerType::drumPads);
        processor->beginMidiLearn(0, ClassicPlayerAudioProcessor::LearnTarget::volume);
        cc(*processor, 4, 73, 64);
        check(processor->midiLearnCC(0, ClassicPlayerAudioProcessor::LearnTarget::volume) == 73,
              "drum volume CC mapping");
        check(processor->midiLearnChannel(0, ClassicPlayerAudioProcessor::LearnTarget::volume) == 4,
              "drum volume CC channel");
        const auto drumGain = processor->parameters.getRawParameterValue("layer1Gain")->load();
        check(drumGain >= 50.0f && drumGain <= 51.0f, "drum volume CC response");
        std::cout << "Master and Drum Pad volume CC/channel mapping passed\n";

        // Regression for the physical knobs used with the SMK37 and Launchkey:
        // CC 48 on channel 6 must be learnable for each layer effect, even
        // when the layer is currently routed to another MIDI input.
        processor->setLayerType(0, ClassicPlayerAudioProcessor::LayerType::sf2);
        const std::array<std::pair<ClassicPlayerAudioProcessor::LearnTarget, const char*>, 3> effectTargets {{
            { ClassicPlayerAudioProcessor::LearnTarget::cutoff, "layer1Cutoff" },
            { ClassicPlayerAudioProcessor::LearnTarget::reverb, "layer1Reverb" },
            { ClassicPlayerAudioProcessor::LearnTarget::compressor, "layer1Comp" }
        }};
        for (const auto& [target, parameterId] : effectTargets)
        {
            processor->beginMidiLearn(0, target);
            cc(*processor, 6, 48, 96);
            check(processor->midiLearnCC(0, target) == 48, "effect CC number not learned");
            check(processor->midiLearnChannel(0, target) == 6, "effect CC channel not learned");
            const auto value = processor->parameters.getRawParameterValue(parameterId)->load();
            check(value >= 75.0f && value <= 76.0f, "effect CC response");
        }
        // The audio callback must apply a learned value even before the
        // message-thread timer has had a chance to notify the UI/host.
        processor->beginMidiLearn(0, ClassicPlayerAudioProcessor::LearnTarget::cutoff);
        cc(*processor, 6, 48, 96);
        ccAudioOnly(*processor, 6, 48, 32);
        const auto audioOnlyCutoff = processor->parameters.getRawParameterValue("layer1Cutoff")->load();
        check(audioOnlyCutoff >= 25.0f && audioOnlyCutoff <= 26.0f,
              "effect CC was not applied by audio callback");
        std::cout << "Layer effect MIDI Learn CC/channel mapping passed\n";

        // A layer's Mute button can learn a momentary CC. Learning must not
        // immediately mute it; each press toggles once and its release rearms.
        using LearnTarget = ClassicPlayerAudioProcessor::LearnTarget;
        processor->setLayerMuted(0, false);
        processor->beginMidiLearn(0, LearnTarget::mute);
        cc(*processor, 7, 81, 127);
        check(processor->midiLearnCC(0, LearnTarget::mute) == 81
              && processor->midiLearnChannel(0, LearnTarget::mute) == 7,
              "mute CC mapping/channel not learned");
        check(!processor->isLayerMuted(0), "mute learn press toggled the layer");
        cc(*processor, 7, 81, 0);
        cc(*processor, 7, 81, 127);
        check(processor->isLayerMuted(0), "mute CC press did not mute the layer");
        cc(*processor, 7, 81, 127);
        check(processor->isLayerMuted(0), "held mute CC toggled more than once");
        cc(*processor, 7, 81, 0);
        cc(*processor, 6, 81, 127);
        check(processor->isLayerMuted(0), "mute CC ignored its learned channel");

        juce::MemoryBlock muteState;
        processor->getStateInformation(muteState);
        processor->setLayerMuted(0, false);
        processor->setStateInformation(muteState.getData(), static_cast<int>(muteState.getSize()));
        check(processor->isLayerMuted(0)
              && processor->midiLearnCC(0, LearnTarget::mute) == 81
              && processor->midiLearnChannel(0, LearnTarget::mute) == 7,
              "mute state or Learn mapping did not persist");
        std::cout << "Layer Mute CC Learn, press-edge and persistence passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
