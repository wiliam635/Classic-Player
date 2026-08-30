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
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
