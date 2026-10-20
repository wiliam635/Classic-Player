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

static void layerDefaultsSplitAndMasterEqLearn()
{
    using Processor = ClassicPlayerAudioProcessor;
    auto p = std::make_unique<Processor>();
    p->prepareToPlay(48000, 128);
    p->resetToNewProgram();
    check(p->addLayer(), "create first layer");
    auto config = p->layerConfig(0);
    config.octave = 3; config.lowNote = 48; config.highNote = 72;
    config.midiChannel = 9; config.mono = true; config.sustainEnabled = false;
    p->setLayerConfig(0, config);
    check(p->addLayer(Processor::LayerType::nord), "create independent NPNO layer");
    config = p->layerConfig(1);
    check(config.octave == 0 && config.lowNote == 0 && config.highNote == 127
          && config.midiChannel == 0 && !config.mono && config.sustainEnabled,
          "new layer inherited previous layer routing");
    // Dirty an INACTIVE slot as older sessions/removals can do.
    const auto slot = p->activeLayerCount();
    config.octave = -3; config.midiChannel = 16; config.lowNote = 60; config.highNote = 61;
    p->setLayerConfig(slot, config);
    p->setLayerMidiDevice(slot, "stale-device");
    p->parameters.getParameter("layer3Cutoff")->setValueNotifyingHost(0.2f);
    p->beginMidiLearn(slot, Processor::LearnTarget::cutoff);
    cc(*p, 4, 74, 12);
    check(p->addLayer(), "reuse dirty inactive slot");
    config = p->layerConfig(slot);
    check(config.octave == 0 && config.midiChannel == 0 && config.lowNote == 0 && config.highNote == 127,
          "reused slot retained stale routing");
    check(p->layerMidiDevice(slot).isEmpty() && p->midiLearnCC(slot, Processor::LearnTarget::cutoff) < 0,
          "reused slot retained device or CC mapping");
    check(p->parameters.getRawParameterValue("layer3Cutoff")->load() == 100.0f,
          "new layer retained stale effect parameter");
    check(p->layerConfig(0).octave == 3 && p->layerConfig(0).midiChannel == 9,
          "adding layer modified existing layer");

    const auto play = [&p](juce::MidiMessage message)
    {
        juce::AudioBuffer<float> audio(2, 128); audio.clear();
        juce::MidiBuffer midi; midi.addEvent(message, 0);
        p->processBlock(audio, midi);
        p->consumeMidiControlUpdates();
    };
    p->beginSplitLearn(0, false);
    play(juce::MidiMessage::noteOff(2, 36));
    check(p->isSplitLearning(0, false), "note-off captured as split");
    play(juce::MidiMessage::noteOn(2, 36, (juce::uint8) 100));
    check(p->layerConfig(0).lowNote == 36 && !p->isSplitLearning(0, false),
          "raw split capture rejected channel/out-of-range note or transposed it");
    play(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 100));
    check(p->layerConfig(0).lowNote == 36, "split capture not one-shot");
    p->beginSplitLearn(0, true);
    play(juce::MidiMessage::noteOn(2, 30, (juce::uint8) 100));
    check(p->layerConfig(0).lowNote == 30 && p->layerConfig(0).highNote == 30,
          "crossed split endpoints invalid");
    p->beginSplitLearn(0, false); p->beginSplitLearn(0, false);
    play(juce::MidiMessage::noteOn(2, 20, (juce::uint8) 100));
    check(p->layerConfig(0).lowNote == 30, "cancelled split learn still captured");
    p->resetLayerSplit(0);
    config = p->layerConfig(0);
    check(config.lowNote == 0 && config.highNote == 127 && config.octave == 3 && config.midiChannel == 9,
          "split reset changed unrelated routing");

    const std::array<const char*, 3> ids { "masterEqLow", "masterEqMid", "masterEqHigh" };
    for (int band = 0; band < 3; ++band)
    {
        p->beginMasterEqMidiLearn(band);
        cc(*p, 5, 64, 127);
        check(p->isMasterEqMidiLearning(band), "sustain captured by master EQ Learn");
        cc(*p, 5, 120, 127);
        check(p->isMasterEqMidiLearning(band), "channel-mode CC captured by master EQ Learn");
        cc(*p, 5, 20 + band, 127);
        check(p->masterEqMidiLearnCC(band) == 20 + band && p->masterEqMidiLearnChannel(band) == 5,
              "master EQ mapping not captured");
        check(p->masterEqValue(ids[(size_t) band]) == 12.0f, "master EQ maximum gain incorrect");
        cc(*p, 6, 20 + band, 0);
        check(p->masterEqValue(ids[(size_t) band]) == 12.0f, "master EQ accepted wrong MIDI channel");
        ccAudioOnly(*p, 5, 20 + band, 0);
        check(p->masterEqValue(ids[(size_t) band]) == -12.0f, "master EQ realtime minimum gain incorrect");
        p->consumeMidiControlUpdates();
    }
    juce::MemoryBlock state;
    p->getStateInformation(state);
    for (int band = 0; band < 3; ++band) p->clearMasterEqMidiLearn(band);
    p->setStateInformation(state.getData(), (int) state.getSize());
    for (int band = 0; band < 3; ++band)
        check(p->masterEqMidiLearnCC(band) == 20 + band && p->masterEqMidiLearnChannel(band) == 5
              && p->masterEqValue(ids[(size_t) band]) == -12.0f,
              "master EQ gain or mapping did not survive state restore");
    check(p->layerConfig(0).octave == 3 && p->layerConfig(0).lowNote == 0,
          "split/routing did not survive state restore");
    p->clearMasterEqMidiLearn(0);
    cc(*p, 5, 20, 127);
    check(p->masterEqValue("masterEqLow") == -12.0f, "cleared EQ CC mapping still active");
    std::cout << "Layer defaults, split Learn/reset and master EQ CC persistence passed\n";
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
    const auto setLayerParameter = [&p](const juce::String& id, float value)
    {
        auto* parameter = p->parameters.getParameter(id);
        check(parameter != nullptr, "layer effect parameter exists");
        const auto normalized = parameter->convertTo0to1(value);
        parameter->setValue(normalized);
        if (auto* raw = p->parameters.getRawParameterValue(id))
            raw->store(parameter->convertFrom0to1(normalized), std::memory_order_release);
    };
    setLayerParameter("layer1Attack", 12.0f);
    setLayerParameter("layer1Release", 42.0f);
    setLayerParameter("layer1Cutoff", 37.0f);
    setLayerParameter("layer1Reverb", 33.0f);
    setLayerParameter("layer1Comp", 45.0f);
    setLayerParameter("layer1EqLow", -4.5f);
    setLayerParameter("layer1EqMid", 2.5f);
    setLayerParameter("layer1EqHigh", -1.5f);
    setLayerParameter("layer1PreGainDb", 6.0f);
    {
        auto fxConfig = p->layerConfig(0);
        fxConfig.nordFx.delayBpm = 92.0f;
        fxConfig.nordFx.delayMix = 27.0f;
        fxConfig.nordFx.chorusMix = 18.0f;
        p->setLayerConfig(0, fxConfig);
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
    for (const auto& [id, expected] : std::array<std::pair<const char*, float>, 9> {{
             { "layer1Attack", 12.0f }, { "layer1Release", 42.0f },
             { "layer1Cutoff", 37.0f }, { "layer1Reverb", 33.0f },
             { "layer1Comp", 45.0f }, { "layer1EqLow", -4.5f },
             { "layer1EqMid", 2.5f }, { "layer1EqHigh", -1.5f },
             { "layer1PreGainDb", 6.0f }
         }})
    {
        const auto* actual = p->parameters.getRawParameterValue(id);
        check(actual != nullptr && std::abs(actual->load() - expected) < 0.11f,
              "layer sound controls did not survive save and reopen");
    }
    for (int layer = 0; layer < 4; ++layer)
        check(p->layerConfig(layer).midiChannel == savedChannels[layer], "startup lost layer MIDI channel");
    const auto restoredFx = p->layerConfig(0).nordFx;
    check(std::abs(restoredFx.delayBpm - 92.0f) < 0.1f
          && std::abs(restoredFx.delayMix - 27.0f) < 0.1f
          && std::abs(restoredFx.chorusMix - 18.0f) < 0.1f,
          "Nord effects did not survive save and reopen");
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

    auto sourceOwner = std::make_unique<ClassicPlayerAudioProcessor>(sourceRoot);
    auto& sourceProcessor = *sourceOwner;
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

    auto destinationOwner = std::make_unique<ClassicPlayerAudioProcessor>(destinationRoot);
    auto& destinationProcessor = *destinationOwner;
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

    auto startup = juce::XmlDocument::parse(root.getChildFile("Startup.xml"));
    check(startup != nullptr && startup->getStringAttribute("lastSavedProgram").isEmpty(),
          "new project did not clear the startup file");

    processor.reset();
    processor = open();
    check(processor->activeLayerCount() == 0 && processor->currentSavedProgramName().isEmpty(),
          "reopening after a new project restored the previous performance");
    check(processor->masterMidiLearnCC() == 21 && processor->panicMidiLearnCC() == 22,
          "reopening after a new project lost global controller mappings");
}

struct MidiRecordingRegressionAccess
{
    static void sf2Mute(const juce::File& file)
    {
        auto processor = std::make_unique<ClassicPlayerAudioProcessor>();
        processor->prepareToPlay(48000, 256);
        processor->activated.store(true);
        check(processor->loadSoundFont(0, file).wasOk(), "mute SF2 fixture load");
        check(processor->loadSoundFont(1, file).wasOk(), "second mute SF2 fixture load");
        const auto notePeak = [&]
        {
            processor->engine.stopAllSounds();
            juce::AudioBuffer<float> audio(2, 256);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            float peak = 0;
            for (int block = 0; block < 80; ++block)
            {
                processor->processBlock(audio, midi);
                midi.clear();
                if (block > 8) peak = juce::jmax(peak, audio.getMagnitude(0, 0, 256));
            }
            return peak;
        };
        for (int layer : {0, 1})
        {
            for (int i = 0; i < 2; ++i)
            {
                auto config = processor->layerConfig(i);
                config.enabled = i == layer;
                processor->setLayerConfig(i, config);
            }
            for (int controller : {11, 7})
            {
                processor->setLayerMuted(layer, false);
                // Include a legacy duplicate volume mapping on the same CC.
                processor->beginMidiLearn(layer, ClassicPlayerAudioProcessor::LearnTarget::volume);
                cc(*processor, 1, controller, 100);
                processor->beginMidiLearn(layer, ClassicPlayerAudioProcessor::LearnTarget::mute);
                cc(*processor, 1, controller, 127);
                juce::Thread::sleep(510);
                cc(*processor, 1, controller, 0);
                const auto normal = notePeak();
                check(normal > 1.0e-5f, "SF2 silent after learning mute");
                if (layer == 0 && controller == 11)
                {
                    auto other = processor->layerConfig(1);
                    other.enabled = true;
                    processor->setLayerConfig(1, other);
                    notePeak();
                    check(processor->engine.getLayerPeak(1) > 1.0e-5f,
                          "Mute CC also silenced an unmapped SF2 layer");
                    other.enabled = false;
                    processor->setLayerConfig(1, other);
                }
                for (int cycle = 0; cycle < 3; ++cycle)
                {
                    cc(*processor, 1, controller, 127);
                    check(processor->isLayerMuted(layer), "SF2 mute state not set");
                    check(notePeak() < normal * 0.001f, "SF2 still audible while muted");
                    cc(*processor, 1, controller, 0);
                    check(!processor->isLayerMuted(layer), "SF2 did not unmute");
                    check(notePeak() > normal * 0.5f, "SF2 expression/volume stayed silent after unmute");
                }
                std::cout << "SF2 audio mute/unmute layer=" << layer + 1 << " CC=" << controller << " passed\n";
            }
        }
    }
    static void run()
    {
        auto owner = std::make_unique<ClassicPlayerAudioProcessor>();
        auto& processor = *owner;
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

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    try
    {
        if (argc == 3 && juce::String(argv[1]) == "--sf2-mute")
        {
            MidiRecordingRegressionAccess::sf2Mute(juce::File(juce::String::fromUTF8(argv[2])));
            return 0;
        }
        startupPrograms();
        programLibraryExportImport();
        livePerformanceEqPersistence();
        newProgramStartsBlank();
        layerDefaultsSplitAndMasterEqLearn();
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

        // Latched controller buttons send 127 on the first physical press and
        // 0 only on the next one. Detect that pattern and mirror the LED/state
        // instead of requiring two extra presses to bring them back in sync.
        processor->setLayerMuted(1, false);
        processor->beginMidiLearn(1, LearnTarget::mute);
        cc(*processor, 7, 82, 127);
        juce::Thread::sleep(510);
        cc(*processor, 7, 82, 0);
        cc(*processor, 7, 82, 127);
        check(processor->isLayerMuted(1), "latched mute CC high value did not mute");
        cc(*processor, 7, 82, 0);
        check(!processor->isLayerMuted(1), "latched mute CC low value did not unmute");

        juce::MemoryBlock muteState;
        processor->getStateInformation(muteState);
        processor->setLayerMuted(0, false);
        processor->setStateInformation(muteState.getData(), static_cast<int>(muteState.getSize()));
        check(processor->isLayerMuted(0)
              && processor->midiLearnCC(0, LearnTarget::mute) == 81
              && processor->midiLearnChannel(0, LearnTarget::mute) == 7,
              "mute state or Learn mapping did not persist");
        // Layer 2's controller mode must survive preset reloads. Its zero
        // value is a second press, not a momentary button release.
        cc(*processor, 7, 82, 127);
        check(processor->isLayerMuted(1), "restored layer 2 toggle did not mute");
        cc(*processor, 7, 82, 0);
        check(!processor->isLayerMuted(1), "restored layer 2 toggle did not unmute");
        cc(*processor, 7, 82, 127);
        processor->getStateInformation(muteState);
        processor->setStateInformation(muteState.getData(), static_cast<int>(muteState.getSize()));
        cc(*processor, 7, 82, 0);
        check(!processor->isLayerMuted(1), "first low CC after restoring muted layer did not unmute");
        cc(*processor, 7, 81, 0);
        cc(*processor, 7, 81, 127);
        check(!processor->isLayerMuted(0), "restored momentary mute mode was lost");
        for (int layer = 2; layer < processor->activeLayerCount(); ++layer)
        {
            processor->beginMidiLearn(layer, LearnTarget::mute);
            cc(*processor, 7, 82 + layer, 127);
            juce::Thread::sleep(510);
            cc(*processor, 7, 82 + layer, 0);
            cc(*processor, 7, 82 + layer, 127);
            check(processor->isLayerMuted(layer), "later layer toggle did not mute");
        }
        juce::TemporaryFile muteProgram;
        juce::File savedMuteProgram;
        check(processor->saveProgramToFile(muteProgram.getFile().withFileExtension(".ckprogram"),
                                          savedMuteProgram).wasOk(), "save mute preset");
        struct MuteProgramCleanup { juce::File file; ~MuteProgramCleanup() { file.deleteFile(); } }
            muteProgramCleanup { savedMuteProgram };
        check(processor->loadProgram(savedMuteProgram).wasOk(), "reload mute preset");
        for (int layer = 2; layer < processor->activeLayerCount(); ++layer)
        {
            cc(*processor, 7, 82 + layer, 0);
            check(!processor->isLayerMuted(layer), "saved later-layer toggle did not unmute");
            cc(*processor, 7, 82 + layer, 127);
            check(processor->isLayerMuted(layer), "saved later-layer toggle did not mute");
        }
        std::cout << "Layer Mute CC Learn, press-edge and persistence passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
