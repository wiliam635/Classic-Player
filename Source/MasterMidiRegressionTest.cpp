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
    check(p->saveProgram("Last", last).wasOk(), "save last");
    check(p->loadProgram(first).wasOk(), "load older program");
    juce::MemoryBlock unsaved;
    p->getStateInformation(unsaved);
    p.reset();
    p = open(true);
    p->setStateInformation(unsaved.getData(), static_cast<int>(unsaved.getSize()));
    p->prepareToPlay(48000, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 75, "startup did not restore last SAVED program");
    check(p->currentSavedProgramName() == "Last", "restored program name");
    p->parameters.getParameter("master")->setValueNotifyingHost(0.42f);
    p->prepareToPlay(44100, 128);
    check(p->parameters.getRawParameterValue("master")->load() == 42, "device restart reloaded program");
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

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    try
    {
        startupPrograms();
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
