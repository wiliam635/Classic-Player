#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LicenseVerifier.h"
#include <algorithm>

ClassicPlayerAudioProcessor::ClassicPlayerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "CLASSIC_PLAYER", createParameters())
{
    juce::Logger::writeToLog("ClassicPlayer processor criado (instrumento MIDI, saída estéreo)");
    for (auto& layer : learnedCCs)
        for (auto& cc : layer) cc.store(-1);
    for (auto& layer : learnedChannels)
        for (auto& channel : layer) channel.store(-1);
    for (auto& layer : pendingCCValues)
        for (auto& value : layer) value.store(-1.0f);
    for (int layer = Sf2Engine::defaultLayerCount; layer < Sf2Engine::layerCount; ++layer)
    {
        auto config = engine.getConfig(layer);
        config.enabled = false;
        engine.setConfig(layer, config);
    }
    refreshActivation();
}

ClassicPlayerAudioProcessor::~ClassicPlayerAudioProcessor()
{
    if (standaloneDeviceManager != nullptr)
    {
        for (const auto& identifier : registeredMidiInputIds)
            standaloneDeviceManager->removeMidiInputDeviceCallback(identifier, this);
        registeredMidiInputIds.clear();
        if (standaloneDefaultMidiCallback != nullptr)
            standaloneDeviceManager->addMidiInputDeviceCallback({}, standaloneDefaultMidiCallback);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ClassicPlayerAudioProcessor::createParameters()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> result;
    result.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"master", 1}, "Master", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 80.0f));
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        const auto n = juce::String(i + 1);
        result.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"layer" + n + "Gain", 1}, "Layer " + n + " Volume",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 80.0f));
        result.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"layer" + n + "Release", 1}, "Layer " + n + " Release",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 50.0f));
        result.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"layer" + n + "Cutoff", 1}, "Layer " + n + " Cutoff",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f));
        result.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"layer" + n + "Reverb", 1}, "Layer " + n + " Reverb",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f));
        result.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"layer" + n + "Comp", 1}, "Layer " + n + " Compressor",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f));
    }
    return { result.begin(), result.end() };
}

void ClassicPlayerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::Logger::writeToLog("prepareToPlay: sampleRate=" + juce::String(sampleRate)
                             + " buffer=" + juce::String(samplesPerBlock));
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    engine.prepare(sampleRate, samplesPerBlock);
    for (auto& hosted : externalInstruments) hosted.prepare(sampleRate, samplesPerBlock);
    for (auto& scratch : externalScratch)
        scratch.setSize(2, samplesPerBlock, false, true, true);
    for (auto& midiBuffer : externalMidi) midiBuffer.ensureSize(4096);
    for (auto& collector : routedMidiCollectors) collector.reset(sampleRate);
    visualMidiCollector.reset(sampleRate);
    outputLimiter.prepare({ sampleRate, (juce::uint32) samplesPerBlock, 2 });
    outputLimiter.setThreshold(-0.3f);
    outputLimiter.setRelease(80.0f);
    restoreLayerPaths();
}

void ClassicPlayerAudioProcessor::releaseResources()
{
    engine.reset();
    for (auto& hosted : externalInstruments) hosted.releaseResources();
    outputLimiter.reset();
}

bool ClassicPlayerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto supported = layouts.inputBuses.isEmpty()
                        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    juce::Logger::writeToLog("isBusesLayoutSupported: entradas="
                             + juce::String(layouts.inputBuses.size())
                             + " saída=" + layouts.getMainOutputChannelSet().getDescription()
                             + " resultado=" + (supported ? "OK" : "recusado"));
    return supported;
}

void ClassicPlayerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Always consume MIDI before checking activation.  Besides driving the
    // synth, this state feeds the on-screen keyboard and chord detector, so an
    // activation refresh must never make the MIDI UI appear disconnected.
    keyboardState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

    for (const auto metadata : midi) processMidiControlMessage(metadata.getMessage());

    for (int layer = 0; layer < Sf2Engine::layerCount; ++layer)
    {
        routedMidiBuffers[(size_t) layer].clear();
        routedMidiCollectors[(size_t) layer].removeNextBlockOfMessages(
            routedMidiBuffers[(size_t) layer], buffer.getNumSamples());
    }
    visualMidiBuffer.clear();
    visualMidiCollector.removeNextBlockOfMessages(visualMidiBuffer, buffer.getNumSamples());
    keyboardState.processNextMidiBuffer(visualMidiBuffer, 0, buffer.getNumSamples(), false);

    if (!activated.load())
    {
        buffer.clear();
        return;
    }

    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        auto config = engine.getConfig(i);
        const auto prefix = "layer" + juce::String(i + 1);
        config.gain = parameters.getRawParameterValue(prefix + "Gain")->load() / 100.0f;
        config.release = parameters.getRawParameterValue(prefix + "Release")->load();
        config.cutoff = parameters.getRawParameterValue(prefix + "Cutoff")->load();
        config.reverb = parameters.getRawParameterValue(prefix + "Reverb")->load();
        config.compressor = parameters.getRawParameterValue(prefix + "Comp")->load();
        engine.setConfig(i, config);
    }
    engine.process(buffer, midi, &routedMidiBuffers);
    renderExternalInstruments(buffer, midi);
    buffer.applyGain(parameters.getRawParameterValue("master")->load() / 100.0f);
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    outputLimiter.process(context);
}

void ClassicPlayerAudioProcessor::refreshActivation()
{
    activated.store(LicenseVerifier::isActivated());
}

juce::Result ClassicPlayerAudioProcessor::loadSoundFont(int layer, const juce::File& file)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        return juce::Result::fail("Layer inválida.");
    externalInstruments[(size_t) layer].unload();
    const auto result = engine.loadSoundFont(layer, file);
    if (result.wasOk()) savedPaths[(size_t) layer] = file.getFullPathName();
    return result;
}

void ClassicPlayerAudioProcessor::unloadSoundFont(int layer)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    engine.unloadSoundFont(layer);
    if (juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) savedPaths[(size_t) layer].clear();
}

bool ClassicPlayerAudioProcessor::supportsExternalInstruments() const noexcept
{
    return juce::PluginHostType::getPluginLoadedAs()
           == juce::AudioProcessor::wrapperType_Standalone;
}

juce::Result ClassicPlayerAudioProcessor::loadExternalInstrument(int layer, const juce::File& file)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (!supportsExternalInstruments())
        return juce::Result::fail("Instrumentos externos só podem ser carregados no Classic Player standalone.");
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        return juce::Result::fail("Layer inválida.");

    const auto result = externalInstruments[(size_t) layer].loadInstrument(
        file, currentSampleRate, currentBlockSize);
    if (result.wasOk())
    {
        // The callback lock is already held here; do not call unloadSoundFont(),
        // which also locks it. Loading an external instrument replaces the SF2.
        engine.unloadSoundFont(layer);
        savedPaths[(size_t) layer].clear();
    }
    return result;
}

void ClassicPlayerAudioProcessor::unloadExternalInstrument(int layer)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        externalInstruments[(size_t) layer].unload();
}

bool ClassicPlayerAudioProcessor::hasExternalInstrument(int layer) const
{
    return juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)
        && externalInstruments[(size_t) layer].isLoaded();
}

juce::String ClassicPlayerAudioProcessor::externalInstrumentName(int layer) const
{
    return juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)
        ? externalInstruments[(size_t) layer].getName() : juce::String{};
}

juce::AudioProcessorEditor* ClassicPlayerAudioProcessor::createExternalInstrumentEditor(int layer)
{
    return juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)
        ? externalInstruments[(size_t) layer].createEditor() : nullptr;
}

juce::Array<juce::File> ClassicPlayerAudioProcessor::availableExternalInstruments() const
{
    return externalInstrumentLibrary;
}

void ClassicPlayerAudioProcessor::refreshExternalInstrumentLibrary()
{
    if (!supportsExternalInstruments())
    {
        externalInstrumentLibrary.clear();
        return;
    }

    externalInstrumentLibrary = ExternalInstrumentHost::findInstalledInstruments();
}

void ClassicPlayerAudioProcessor::appendExternalMidi(int layer, const juce::MidiBuffer& incoming,
                                                      juce::MidiBuffer& destination)
{
    const auto config = engine.getConfig(layer);
    for (const auto metadata : incoming)
    {
        auto message = metadata.getMessage();
        // JUCE exposes the channel for every MIDI message. System messages
        // use channel 0, so a channel-specific layer naturally ignores them.
        if (config.midiChannel > 0 && message.getChannel() != config.midiChannel)
            continue;
        if (message.isController() && message.getControllerNumber() == 64 && !config.sustainEnabled)
            continue;

        if (message.isNoteOn() || message.isNoteOff())
        {
            const auto note = message.getNoteNumber();
            if (note < config.lowNote || note > config.highNote) continue;
            message.setNoteNumber(juce::jlimit(0, 127, note + config.octave * 12));
        }
        destination.addEvent(message, metadata.samplePosition);
    }
}

void ClassicPlayerAudioProcessor::renderExternalInstruments(juce::AudioBuffer<float>& output,
                                                             const juce::MidiBuffer& hostMidi)
{
    for (int layer = 0; layer < activeLayerCount(); ++layer)
    {
        if (!externalInstruments[(size_t) layer].isLoaded()) continue;
        if (!engine.getConfig(layer).enabled) continue;

        auto& scratch = externalScratch[(size_t) layer];
        auto& midi = externalMidi[(size_t) layer];
        scratch.clear();
        midi.clear();
        appendExternalMidi(layer, hostMidi, midi);
        appendExternalMidi(layer, routedMidiBuffers[(size_t) layer], midi);
        externalInstruments[(size_t) layer].process(scratch, midi);
        // External instruments share the same layer volume and mixer state as SF2 layers.
        scratch.applyGain(engine.getConfig(layer).gain);
        for (int channel = 0; channel < juce::jmin(output.getNumChannels(), scratch.getNumChannels()); ++channel)
            output.addFrom(channel, 0, scratch, channel, 0, output.getNumSamples());
    }
}

juce::String ClassicPlayerAudioProcessor::soundFontPath(int layer) const { return engine.getSoundFontPath(layer); }
Sf2Engine::LayerConfig ClassicPlayerAudioProcessor::layerConfig(int layer) const { return engine.getConfig(layer); }
void ClassicPlayerAudioProcessor::setLayerConfig(int layer, const Sf2Engine::LayerConfig& c) { engine.setConfig(layer, c); }
std::vector<Sf2Engine::Preset> ClassicPlayerAudioProcessor::layerPresets(int layer) const { return engine.getPresets(layer); }
void ClassicPlayerAudioProcessor::selectLayerPreset(int layer, int bank, int program) { engine.selectPreset(layer, bank, program); }
void ClassicPlayerAudioProcessor::sendLayerController(int layer, int controller, int value) { engine.sendController(layer, controller, value); }
float ClassicPlayerAudioProcessor::layerPeak(int layer) const { return engine.getLayerPeak(layer); }

bool ClassicPlayerAudioProcessor::addLayer()
{
    auto count = activeLayers.load(std::memory_order_relaxed);
    while (count < Sf2Engine::layerCount)
    {
        if (activeLayers.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
        {
            auto config = engine.getConfig(count);
            config.enabled = true;
            engine.setConfig(count, config);
            return true;
        }
    }
    return false;
}

bool ClassicPlayerAudioProcessor::removeLayer(int layer)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return false;
    auto count = activeLayers.load(std::memory_order_relaxed);
    if (layer >= count || count <= 1) return false;
    if (layer != count - 1)
    {
        const auto sourcePath = engine.getSoundFontPath(count - 1);
        const auto sourceConfig = engine.getConfig(count - 1);
        engine.unloadSoundFont(layer);
        if (sourcePath.isNotEmpty()) engine.loadSoundFont(layer, juce::File(sourcePath));
        engine.setConfig(layer, sourceConfig);
        savedPaths[(size_t) layer] = savedPaths[(size_t) (count - 1)];
        layerMidiDeviceIds[(size_t) layer] = layerMidiDeviceIds[(size_t) (count - 1)];
        externalInstruments[(size_t) layer].moveFrom(externalInstruments[(size_t) (count - 1)]);
    }
    else
    {
        externalInstruments[(size_t) layer].unload();
    }
    engine.unloadSoundFont(count - 1);
    auto disabled = engine.getConfig(count - 1);
    disabled.enabled = false;
    engine.setConfig(count - 1, disabled);
    savedPaths[(size_t) (count - 1)].clear();
    layerMidiDeviceIds[(size_t) (count - 1)].clear();
    activeLayers.store(count - 1, std::memory_order_relaxed);
    return true;
}

void ClassicPlayerAudioProcessor::beginMidiLearn(int layer, LearnTarget target)
{
    const auto targetIndex = static_cast<int>(target);
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ||
        !juce::isPositiveAndBelow(targetIndex, learnTargetCount)) return;
    activeMidiLearn.store(layer * learnTargetCount + targetIndex, std::memory_order_relaxed);
}

int ClassicPlayerAudioProcessor::midiLearnCC(int layer, LearnTarget target) const
{
    const auto targetIndex = static_cast<int>(target);
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ||
        !juce::isPositiveAndBelow(targetIndex, learnTargetCount)) return -1;
    return learnedCCs[(size_t) layer][(size_t) targetIndex].load(std::memory_order_relaxed);
}

int ClassicPlayerAudioProcessor::midiLearnChannel(int layer, LearnTarget target) const
{
    const auto targetIndex = static_cast<int>(target);
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ||
        !juce::isPositiveAndBelow(targetIndex, learnTargetCount)) return -1;
    return learnedChannels[(size_t) layer][(size_t) targetIndex].load(std::memory_order_relaxed);
}

bool ClassicPlayerAudioProcessor::isMidiLearning(int layer, LearnTarget target) const
{
    return activeMidiLearn.load(std::memory_order_relaxed) ==
           layer * learnTargetCount + static_cast<int>(target);
}

void ClassicPlayerAudioProcessor::consumeMidiControlUpdates()
{
    static const std::array<const char*, learnTargetCount> suffixes { "Gain", "Cutoff", "Reverb", "Comp", "Release" };
    for (int layer = 0; layer < Sf2Engine::layerCount; ++layer)
    {
        const auto prefix = "layer" + juce::String(layer + 1);
        for (int target = 0; target < learnTargetCount; ++target)
        {
            const auto value = pendingCCValues[(size_t) layer][(size_t) target].exchange(-1.0f);
            if (value < 0.0f) continue;
            if (auto* parameter = parameters.getParameter(prefix + suffixes[(size_t) target]))
                parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
        }
    }
}

void ClassicPlayerAudioProcessor::processMidiControlMessage(const juce::MidiMessage& message,
                                                             int layerFilter)
{
    auto active = activeMidiLearn.load(std::memory_order_relaxed);

    // Preserve a useful diagnosis in the application log when a controller
    // sends something other than a normal CC (for example NRPN/SysEx).
    if (!message.isController())
    {
        if (active >= 0)
            juce::Logger::writeToLog("MIDI Learn ignorou mensagem não-CC: "
                                     + message.getDescription());
        return;
    }

    const auto cc = message.getControllerNumber();
    const auto channel = message.getChannel();

    // Some controllers use CC64–67 for physical faders. Permit those
    // mappings while refusing the usual binary CC64 pedal event during Learn.
    // A pedal sends only off/on (0 or 127); a fader can be learned by moving it
    // to any intermediate position. Already learned mappings still receive all
    // values, including fader endpoints.
    const auto controllerValue = message.getControllerValue();
    const auto binarySustainEvent = cc == 64 && (controllerValue == 0 || controllerValue == 127);

    if (active >= 0 && !binarySustainEvent)
    {
        const auto layer = active / learnTargetCount;
        const auto target = active % learnTargetCount;
        if ((layerFilter < 0 || layerFilter == layer) &&
            juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) &&
            juce::isPositiveAndBelow(target, learnTargetCount))
        {
            learnedCCs[(size_t) layer][(size_t) target].store(cc, std::memory_order_relaxed);
            learnedChannels[(size_t) layer][(size_t) target].store(channel, std::memory_order_relaxed);
            juce::Logger::writeToLog("MIDI Learn: layer=" + juce::String(layer + 1)
                                     + " CC=" + juce::String(cc)
                                     + " canal=" + juce::String(channel));
            activeMidiLearn.compare_exchange_strong(active, -1);
        }
    }

    const auto normalised = static_cast<float>(controllerValue) / 127.0f;
    const auto firstLayer = layerFilter >= 0 ? layerFilter : 0;
    const auto lastLayer = layerFilter >= 0 ? layerFilter + 1 : activeLayerCount();
    for (int layer = firstLayer; layer < lastLayer; ++layer)
        for (int target = 0; target < learnTargetCount; ++target)
        {
            const auto learnedCC = learnedCCs[(size_t) layer][(size_t) target].load(std::memory_order_relaxed);
            const auto learnedChannel = learnedChannels[(size_t) layer][(size_t) target].load(std::memory_order_relaxed);
            if (learnedCC != cc || (learnedChannel >= 0 && learnedChannel != channel))
                continue;

            // A regular sustain pedal sends binary CC64 values only. Do not
            // apply those switch events to a parameter learned from a CC64
            // fader; the MIDI sustain message itself remains routed normally.
            if (learnedCC == 64 && binarySustainEvent)
                continue;

            pendingCCValues[(size_t) layer][(size_t) target].store(normalised,
                                                                      std::memory_order_relaxed);
        }
}

void ClassicPlayerAudioProcessor::attachStandaloneMidiRouting(
    juce::AudioDeviceManager& manager, juce::MidiInputCallback& defaultCallback)
{
    if (standaloneDeviceManager == &manager) return;
    standaloneDeviceManager = &manager;
    standaloneDefaultMidiCallback = &defaultCallback;

    // The JUCE standalone wrapper normally forwards every enabled MIDI input
    // through this global callback. Remove that route before registering our
    // per-device callbacks, otherwise each note is delivered twice.
    manager.removeMidiInputDeviceCallback({}, &defaultCallback);
    refreshStandaloneMidiInputs();
}

void ClassicPlayerAudioProcessor::refreshStandaloneMidiInputs()
{
    if (standaloneDeviceManager == nullptr) return;
    const auto devices = juce::MidiInput::getAvailableDevices();
    juce::String fingerprint;
    for (const auto& device : devices) fingerprint << device.identifier << ";";
    if (fingerprint == registeredMidiFingerprint) return;

    // AudioDeviceManager owns the MidiInput objects. This is important on
    // Windows: opening the same driver directly while the standalone wrapper
    // is enabling/disabling it can invalidate the driver's callback object and
    // crash inside the MSVC runtime during startup.
    for (const auto& identifier : registeredMidiInputIds)
        standaloneDeviceManager->removeMidiInputDeviceCallback(identifier, this);
    registeredMidiInputIds.clear();

    for (const auto& device : devices)
    {
        standaloneDeviceManager->setMidiInputDeviceEnabled(device.identifier, true);
        standaloneDeviceManager->addMidiInputDeviceCallback(device.identifier, this);
        registeredMidiInputIds.add(device.identifier);
    }
    registeredMidiFingerprint = fingerprint;
}

juce::Array<juce::MidiDeviceInfo> ClassicPlayerAudioProcessor::availableMidiDevices() const
{
    return juce::MidiInput::getAvailableDevices();
}

void ClassicPlayerAudioProcessor::setLayerMidiDevice(int layer, const juce::String& identifier)
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return;
    const juce::ScopedLock guard(midiRoutingLock);
    layerMidiDeviceIds[(size_t) layer] = identifier;
}

juce::String ClassicPlayerAudioProcessor::layerMidiDevice(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return {};
    const juce::ScopedLock guard(midiRoutingLock);
    return layerMidiDeviceIds[(size_t) layer];
}

juce::StringArray ClassicPlayerAudioProcessor::soundFontCategories()
{
    return { "Piano Acustico", "Piano Eletrico", "Piano DX", "Strings", "Pad", "Synth",
             "Brass", "Organ", "Guitar", "Bass", "Bells", "Efeitos", "Outros" };
}

juce::Result ClassicPlayerAudioProcessor::importSoundFont(const juce::File& source,
                                                           const juce::String& category,
                                                           juce::File& importedFile) const
{
    if (!source.existsAsFile() || source.getFileExtension().toLowerCase() != ".sf2")
        return juce::Result::fail("Selecione um arquivo SF2 valido");
    if (!soundFontCategories().contains(category))
        return juce::Result::fail("Categoria de SoundFont invalida");

    auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Classic Player").getChildFile("SoundFonts").getChildFile(category);
    const auto directoryResult = folder.createDirectory();
    if (directoryResult.failed()) return directoryResult;

    auto destination = folder.getChildFile(source.getFileName());
    if (destination.existsAsFile())
    {
        if (source.hasIdenticalContentTo(destination))
        {
            importedFile = destination;
            return juce::Result::ok();
        }
        destination = folder.getNonexistentChildFile(source.getFileNameWithoutExtension(), ".sf2", true);
    }
    if (!source.copyFileTo(destination))
        return juce::Result::fail("Nao foi possivel copiar o SF2 para a biblioteca do aplicativo");
    importedFile = destination;
    return juce::Result::ok();
}

juce::Array<juce::File> ClassicPlayerAudioProcessor::librarySoundFonts(
    const juce::String& category) const
{
    juce::Array<juce::File> result;
    if (!soundFontCategories().contains(category)) return result;
    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("Classic Player").getChildFile("SoundFonts").getChildFile(category);
    folder.findChildFiles(result, juce::File::findFiles, false, "*.sf2;*.SF2");
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
    {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    return result;
}

juce::Result ClassicPlayerAudioProcessor::deleteLibrarySoundFont(const juce::File& file)
{
    if (!file.existsAsFile() || file.getFileExtension().toLowerCase() != ".sf2")
        return juce::Result::fail("Selecione um SF2 valido da biblioteca");

    const auto libraryRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("Classic Player").getChildFile("SoundFonts");
    if (!file.isAChildOf(libraryRoot))
        return juce::Result::fail("Apenas arquivos da biblioteca do Classic Player podem ser excluidos");

    // A library item can be loaded in more than one layer. Unload every
    // matching layer first so FluidSynth never keeps a handle to a deleted file.
    for (int layer = 0; layer < Sf2Engine::layerCount; ++layer)
        if (soundFontPath(layer) == file.getFullPathName())
            unloadSoundFont(layer);

    return file.deleteFile()
               ? juce::Result::ok()
               : juce::Result::fail("Nao foi possivel excluir o SF2 da biblioteca");
}

void ClassicPlayerAudioProcessor::handleIncomingMidiMessage(juce::MidiInput* source,
                                                             const juce::MidiMessage& message)
{
    const auto sourceId = source != nullptr ? source->getIdentifier() : juce::String{};
    const juce::ScopedLock guard(midiRoutingLock);
    auto routed = false;
    for (int layer = 0; layer < activeLayerCount(); ++layer)
    {
        const auto& selected = layerMidiDeviceIds[(size_t) layer];
        if (selected.isNotEmpty() && selected != sourceId) continue;
        routedMidiCollectors[(size_t) layer].addMessageToQueue(message);
        processMidiControlMessage(message, layer);
        routed = true;
    }
    if (routed) visualMidiCollector.addMessageToQueue(message);
}

juce::Array<juce::File> ClassicPlayerAudioProcessor::savedPrograms() const
{
    juce::Array<juce::File> result;
    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("Classic Player").getChildFile("Programs");
    folder.findChildFiles(result, juce::File::findFiles, false, "*.ckprogram");
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
    {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    return result;
}

juce::Result ClassicPlayerAudioProcessor::saveProgram(const juce::String& requestedName,
                                                       juce::File& savedFile)
{
    auto name = requestedName.trim();
    name = name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()");
    if (name.isEmpty())
        return juce::Result::fail("Digite um nome para a programação.");

    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("Classic Player").getChildFile("Programs");
    if (const auto result = folder.createDirectory(); result.failed())
        return result;

    juce::MemoryBlock data;
    getStateInformation(data);
    if (data.getSize() == 0)
        return juce::Result::fail("Não foi possível preparar a programação.");

    const auto destination = folder.getChildFile(name + ".ckprogram");
    if (!destination.replaceWithData(data.getData(), data.getSize()))
        return juce::Result::fail("Não foi possível salvar a programação.");

    savedFile = destination;
    return juce::Result::ok();
}

juce::Result ClassicPlayerAudioProcessor::loadProgram(const juce::File& programFile)
{
    if (!programFile.existsAsFile() || programFile.getFileExtension().toLowerCase() != ".ckprogram")
        return juce::Result::fail("Selecione uma programação válida do Classic Player.");

    juce::MemoryBlock data;
    if (!programFile.loadFileAsData(data) || data.getSize() == 0)
        return juce::Result::fail("Não foi possível ler a programação.");

    if (auto xml = getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())); xml == nullptr)
        return juce::Result::fail("A programação está corrompida ou é incompatível.");

    setStateInformation(data.getData(), static_cast<int>(data.getSize()));
    return juce::Result::ok();
}

void ClassicPlayerAudioProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();
    state.setProperty("stateVersion", 162, nullptr);
    state.setProperty("activeLayers", activeLayerCount(), nullptr);
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        const auto key = "sf2Layer" + juce::String(i + 1);
        const auto path = engine.getSoundFontPath(i);
        state.setProperty(key, path.isNotEmpty() ? path : savedPaths[(size_t) i], nullptr);
        state.setProperty("externalInstrument" + juce::String(i + 1),
                          externalInstruments[(size_t) i].getPath(), nullptr);
        const auto config = engine.getConfig(i);
        state.setProperty("low" + juce::String(i), config.lowNote, nullptr);
        state.setProperty("high" + juce::String(i), config.highNote, nullptr);
        state.setProperty("octave" + juce::String(i), config.octave, nullptr);
        state.setProperty("velocityCurve" + juce::String(i), config.velocityCurve, nullptr);
        state.setProperty("mono" + juce::String(i), config.mono, nullptr);
        state.setProperty("sustain" + juce::String(i), config.sustainEnabled, nullptr);
        state.setProperty("midiDevice" + juce::String(i), layerMidiDevice(i), nullptr);
        for (int target = 0; target < learnTargetCount; ++target)
        {
            state.setProperty("learn" + juce::String(i) + "_" + juce::String(target),
                              learnedCCs[(size_t) i][(size_t) target].load(), nullptr);
            state.setProperty("learnChannel" + juce::String(i) + "_" + juce::String(target),
                              learnedChannels[(size_t) i][(size_t) target].load(), nullptr);
        }
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destination);
}

void ClassicPlayerAudioProcessor::setStateInformation(const void* data, int size)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (auto xml = getXmlFromBinary(data, size))
    {
        auto state = juce::ValueTree::fromXml(*xml);
        if (state.isValid())
        {
            const auto savedStateVersion = static_cast<int>(state.getProperty("stateVersion", 0));
            auto findParameterState = [&state](const juce::String& parameterId)
            {
                for (int child = 0; child < state.getNumChildren(); ++child)
                {
                    auto parameterState = state.getChild(child);
                    if (parameterState.getProperty("id").toString() == parameterId)
                        return parameterState;
                }
                return juce::ValueTree{};
            };
            auto readParameter = [&state, &findParameterState](const juce::String& parameterId,
                                                                float fallback)
            {
                if (auto parameterState = findParameterState(parameterId); parameterState.isValid())
                    return static_cast<float>(parameterState.getProperty("value", fallback));
                return static_cast<float>(state.getProperty(parameterId, fallback));
            };
            auto writeParameter = [&state, &findParameterState](const juce::String& parameterId,
                                                                 float value)
            {
                if (auto parameterState = findParameterState(parameterId); parameterState.isValid())
                    parameterState.setProperty("value", value, nullptr);
                else
                    state.setProperty(parameterId, value, nullptr);
            };

            if (savedStateVersion < 160)
            {
                const auto masterValue = readParameter("master", 80.0f);
                if (masterValue <= 1.0f) writeParameter("master", masterValue * 100.0f);
                for (int i = 0; i < Sf2Engine::layerCount; ++i)
                {
                    const auto prefix = "layer" + juce::String(i + 1);
                    const auto gainValue = readParameter(prefix + "Gain", 80.0f);
                    if (gainValue <= 1.0f) writeParameter(prefix + "Gain", gainValue * 100.0f);
                    // PAN was replaced by RELEASE in the 1.6.1 parameter set.
                    // Do not attempt to write the removed parameter while
                    // restoring older sessions.
                }
            }

            // Preview 1.6.0 could persist both the master and channel faders at
            // zero.  Repair that one known bad state once; later user changes
            // remain untouched because new saves carry stateVersion 161.
            if (savedStateVersion < 161)
            {
                if (readParameter("master", 80.0f) <= 0.0f)
                    writeParameter("master", 80.0f);
                for (int i = 0; i < Sf2Engine::layerCount; ++i)
                {
                    const auto gainId = "layer" + juce::String(i + 1) + "Gain";
                    if (readParameter(gainId, 80.0f) <= 0.0f)
                        writeParameter(gainId, 80.0f);
                }
            }
            state.setProperty("stateVersion", 162, nullptr);
            parameters.replaceState(state);
            for (int i = 0; i < Sf2Engine::layerCount; ++i)
            {
                savedPaths[(size_t) i] = state.getProperty("sf2Layer" + juce::String(i + 1)).toString();
                const auto externalPath = state.getProperty(
                    "externalInstrument" + juce::String(i + 1)).toString();
                externalInstruments[(size_t) i].unload();
                if (externalPath.isNotEmpty() && supportsExternalInstruments())
                {
                    const auto result = externalInstruments[(size_t) i].loadInstrument(
                        juce::File(externalPath), currentSampleRate, currentBlockSize);
                    if (result.wasOk())
                    {
                        engine.unloadSoundFont(i);
                        savedPaths[(size_t) i].clear();
                    }
                    else
                    {
                        juce::Logger::writeToLog("Não foi possível restaurar instrumento externo: "
                                                 + result.getErrorMessage());
                    }
                }
                auto config = engine.getConfig(i);
                config.lowNote = state.getProperty("low" + juce::String(i), 0);
                config.highNote = state.getProperty("high" + juce::String(i), 127);
                config.octave = state.getProperty("octave" + juce::String(i), 0);
                config.velocityCurve = state.getProperty("velocityCurve" + juce::String(i), 0);
                config.mono = state.getProperty("mono" + juce::String(i), false);
                config.sustainEnabled = state.getProperty("sustain" + juce::String(i), true);
                engine.setConfig(i, config);
                setLayerMidiDevice(i, state.getProperty("midiDevice" + juce::String(i)).toString());
                for (int target = 0; target < learnTargetCount; ++target)
                {
                    learnedCCs[(size_t) i][(size_t) target].store(
                        state.getProperty("learn" + juce::String(i) + "_" + juce::String(target), -1));
                    // Projects saved before channel-aware Learn remain compatible
                    // and continue to accept the learned CC on every channel.
                    learnedChannels[(size_t) i][(size_t) target].store(
                        state.getProperty("learnChannel" + juce::String(i) + "_" + juce::String(target), -1));
                }
            }
            const auto restoredLayerCount = juce::jlimit(
                Sf2Engine::defaultLayerCount, Sf2Engine::layerCount,
                static_cast<int>(state.getProperty("activeLayers", Sf2Engine::defaultLayerCount)));
            activeLayers.store(restoredLayerCount, std::memory_order_relaxed);
            for (int i = 0; i < Sf2Engine::layerCount; ++i)
            {
                auto config = engine.getConfig(i);
                config.enabled = i < restoredLayerCount;
                engine.setConfig(i, config);
            }
            restoreLayerPaths();
        }
    }
}

void ClassicPlayerAudioProcessor::restoreLayerPaths()
{
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
        if (savedPaths[(size_t) i].isNotEmpty())
            engine.loadSoundFont(i, juce::File(savedPaths[(size_t) i]));
}

juce::AudioProcessorEditor* ClassicPlayerAudioProcessor::createEditor()
{
    juce::Logger::writeToLog("Criando editor do Classic Player");
    return new ClassicPlayerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClassicPlayerAudioProcessor();
}
