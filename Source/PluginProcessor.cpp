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
    for (auto& cc : learnedLiveSetSlotCCs) cc.store(-1, std::memory_order_relaxed);
    for (auto& channel : learnedLiveSetSlotChannels) channel.store(-1, std::memory_order_relaxed);
    for (auto& peak : externalPeaks) peak.store(0.0f);
    for (auto& type : layerTypes) type.store(static_cast<int>(LayerType::sf2));
    for (int layer = Sf2Engine::defaultLayerCount; layer < Sf2Engine::layerCount; ++layer)
    {
        auto config = engine.getConfig(layer);
        config.enabled = false;
        engine.setConfig(layer, config);
    }
    loadLiveSetState();
    refreshActivation();
}

ClassicPlayerAudioProcessor::~ClassicPlayerAudioProcessor()
{
    stopAudioRecording();
    recordingThread.stopThread(2000);
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
    dx7Engine.prepare(sampleRate, samplesPerBlock);
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
    dx7Engine.stopAllSounds();
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

    for (const auto metadata : midi)
    {
        processLiveSetSlotMidiMessage(metadata.getMessage());
        processMidiControlMessage(metadata.getMessage());
    }

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
        dx7LayerConfigs[(size_t) i] = config;
    }
    engine.process(buffer, midi, &routedMidiBuffers);
    renderExternalInstruments(buffer, midi);
    dx7Engine.process(buffer, midi, &routedMidiBuffers, dx7LayerConfigs);
    buffer.applyGain(parameters.getRawParameterValue("master")->load() / 100.0f);
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    outputLimiter.process(context);

    // The threaded writer queues this final post-limiter mix. It never performs
    // disk I/O directly in the real-time audio callback.
    if (auto* writer = activeRecordingWriter.load(std::memory_order_acquire))
        writer->write(buffer.getArrayOfReadPointers(), buffer.getNumSamples());
}

juce::Result ClassicPlayerAudioProcessor::startAudioRecording()
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (activeRecordingWriter.load(std::memory_order_acquire) != nullptr)
        return juce::Result::fail("A gravação já está em andamento.");

    auto folder = juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                      .getChildFile("Classic Player Recordings");
    if (!folder.createDirectory())
        return juce::Result::fail("Não foi possível criar a pasta de gravações na Área de Trabalho.");

    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    recordingFile = folder.getNonexistentChildFile("Classic Player " + timestamp, ".wav", false);
    auto stream = std::unique_ptr<juce::FileOutputStream>(recordingFile.createOutputStream());
    if (stream == nullptr)
        return juce::Result::fail("Não foi possível criar o arquivo WAV.");

    juce::WavAudioFormat wav;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(
        stream.release(), currentSampleRate, 2, 24, {}, 0));
    if (writer == nullptr)
    {
        recordingFile.deleteFile();
        recordingFile = {};
        return juce::Result::fail("Não foi possível iniciar o codificador WAV.");
    }

    if (!recordingThread.isThreadRunning()) recordingThread.startThread(3);
    recordingWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        writer.release(), recordingThread, 32768);
    activeRecordingWriter.store(recordingWriter.get(), std::memory_order_release);
    juce::Logger::writeToLog("Gravação WAV iniciada: " + recordingFile.getFullPathName());
    return juce::Result::ok();
}

void ClassicPlayerAudioProcessor::stopAudioRecording()
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (activeRecordingWriter.exchange(nullptr, std::memory_order_acq_rel) == nullptr) return;
    recordingWriter.reset();
    juce::Logger::writeToLog("Gravação WAV finalizada: " + recordingFile.getFullPathName());
}

bool ClassicPlayerAudioProcessor::isAudioRecording() const noexcept
{
    return activeRecordingWriter.load(std::memory_order_acquire) != nullptr;
}

juce::String ClassicPlayerAudioProcessor::recordingFilePath() const
{
    return recordingFile.getFullPathName();
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
    if (result.wasOk())
    {
        dx7Engine.unload(layer);
        layerTypes[(size_t) layer].store(static_cast<int>(LayerType::sf2));
        savedPaths[(size_t) layer] = file.getFullPathName();
    }
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
        dx7Engine.unload(layer);
        layerTypes[(size_t) layer].store(static_cast<int>(LayerType::vst));
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
    for (int layer = 0; layer < Sf2Engine::layerCount; ++layer)
    {
        auto& peak = externalPeaks[(size_t) layer];
        if (layer >= activeLayerCount()
            || !externalInstruments[(size_t) layer].isLoaded()
            || !engine.getConfig(layer).enabled)
        {
            peak.store(peak.load(std::memory_order_relaxed) * 0.82f,
                       std::memory_order_relaxed);
            continue;
        }

        auto& scratch = externalScratch[(size_t) layer];
        auto& midi = externalMidi[(size_t) layer];
        scratch.clear();
        midi.clear();
        appendExternalMidi(layer, hostMidi, midi);
        appendExternalMidi(layer, routedMidiBuffers[(size_t) layer], midi);
        externalInstruments[(size_t) layer].process(scratch, midi);
        // External instruments share the same layer volume and mixer state as SF2 layers.
        scratch.applyGain(engine.getConfig(layer).gain);

        const auto renderedPeak = juce::jmax(
            scratch.getMagnitude(0, 0, scratch.getNumSamples()),
            scratch.getNumChannels() > 1
                ? scratch.getMagnitude(1, 0, scratch.getNumSamples()) : 0.0f);
        peak.store(juce::jmax(renderedPeak,
                               peak.load(std::memory_order_relaxed) * 0.82f),
                   std::memory_order_relaxed);

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
float ClassicPlayerAudioProcessor::layerPeak(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return 0.0f;
    return juce::jmax(engine.getLayerPeak(layer),
                      externalPeaks[(size_t) layer].load(std::memory_order_relaxed));
}

ClassicPlayerAudioProcessor::LayerType ClassicPlayerAudioProcessor::layerType(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return LayerType::sf2;
    const auto value = layerTypes[(size_t) layer].load(std::memory_order_relaxed);
    return static_cast<LayerType>(juce::jlimit(0, 2, value));
}

void ClassicPlayerAudioProcessor::setLayerType(int layer, LayerType type)
{
    if (juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        layerTypes[(size_t) layer].store(static_cast<int>(type), std::memory_order_relaxed);
}

juce::Result ClassicPlayerAudioProcessor::loadDx7(int layer, const juce::File& file)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        return juce::Result::fail("Layer inválida.");
    externalInstruments[(size_t) layer].unload();
    engine.unloadSoundFont(layer);
    const auto result = dx7Engine.loadSysEx(layer, file);
    if (result.wasOk())
    {
        savedPaths[(size_t) layer].clear();
        layerTypes[(size_t) layer].store(static_cast<int>(LayerType::dx7), std::memory_order_relaxed);
    }
    return result;
}

void ClassicPlayerAudioProcessor::unloadDx7(int layer)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (juce::isPositiveAndBelow(layer, Sf2Engine::layerCount))
        dx7Engine.unload(layer);
}

bool ClassicPlayerAudioProcessor::hasDx7(int layer) const { return dx7Engine.isLoaded(layer); }
juce::String ClassicPlayerAudioProcessor::dx7PatchName(int layer) const { return dx7Engine.patchName(layer); }
juce::String ClassicPlayerAudioProcessor::dx7Path(int layer) const { return dx7Engine.path(layer); }

bool ClassicPlayerAudioProcessor::addLayer(LayerType type)
{
    auto count = activeLayers.load(std::memory_order_relaxed);
    while (count < Sf2Engine::layerCount)
    {
        if (activeLayers.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
        {
            auto config = engine.getConfig(count);
            config.enabled = true;
            engine.setConfig(count, config);
            layerTypes[(size_t) count].store(static_cast<int>(type), std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool ClassicPlayerAudioProcessor::removeLayer(int layer)
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return false;
    const auto count = activeLayers.load(std::memory_order_relaxed);
    if (layer >= count || count <= 1) return false;

    const auto last = count - 1;
    if (layer != last)
    {
        const auto sourceType = layerType(last);
        const auto sourcePath = engine.getSoundFontPath(last);
        const auto sourceDx7Path = dx7Engine.path(last);
        const auto sourceConfig = engine.getConfig(last);

        externalInstruments[(size_t) layer].unload();
        engine.unloadSoundFont(layer);
        dx7Engine.unload(layer);
        if (sourceType == LayerType::sf2 && sourcePath.isNotEmpty())
            engine.loadSoundFont(layer, juce::File(sourcePath));
        else if (sourceType == LayerType::dx7 && sourceDx7Path.isNotEmpty())
            dx7Engine.loadSysEx(layer, juce::File(sourceDx7Path));
        else if (sourceType == LayerType::vst)
            externalInstruments[(size_t) layer].moveFrom(externalInstruments[(size_t) last);

        engine.setConfig(layer, sourceConfig);
        savedPaths[(size_t) layer] = sourceType == LayerType::sf2
            ? savedPaths[(size_t) last] : juce::String{};
        layerMidiDeviceIds[(size_t) layer] = layerMidiDeviceIds[(size_t) last];
        layerTypes[(size_t) layer].store(static_cast<int>(sourceType), std::memory_order_relaxed);
    }

    externalInstruments[(size_t) last].unload();
    engine.unloadSoundFont(last);
    dx7Engine.unload(last);
    auto disabled = engine.getConfig(last);
    disabled.enabled = false;
    engine.setConfig(last, disabled);
    savedPaths[(size_t) last].clear();
    layerMidiDeviceIds[(size_t) last].clear();
    layerTypes[(size_t) last].store(static_cast<int>(LayerType::sf2), std::memory_order_relaxed);
    activeLayers.store(last, std::memory_order_relaxed);
    return true;
}

void ClassicPlayerAudioProcessor::beginMidiLearn(int layer, LearnTarget target)
{
    const auto targetIndex = static_cast<int>(target);
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ||
        !juce::isPositiveAndBelow(targetIndex, learnTargetCount)) return;
    activeMidiLearn.store(layer * learnTargetCount + targetIndex, std::memory_order_relaxed);
}

void ClassicPlayerAudioProcessor::resetMidiLearn(int layer)
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return;

    for (int target = 0; target < learnTargetCount; ++target)
    {
        learnedCCs[(size_t) layer][(size_t) target].store(-1, std::memory_order_relaxed);
        learnedChannels[(size_t) layer][(size_t) target].store(-1, std::memory_order_relaxed);
        pendingCCValues[(size_t) layer][(size_t) target].store(-1.0f, std::memory_order_relaxed);
    }

    auto active = activeMidiLearn.load(std::memory_order_relaxed);
    if (active >= 0 && active / learnTargetCount == layer)
        activeMidiLearn.compare_exchange_strong(active, -1, std::memory_order_relaxed);
}

int ClassicPlayerAudioProcessor::midiLearnCC(int layer, LearnTarget target) const
{
    const auto targetIndex = static_cast<int>(target);
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ||
        !juce::isPositiveAndBelow(targetIndex, learnTargetCount)) return -1;
    const auto cc = learnedCCs[(size_t) layer][(size_t) targetIndex].load(std::memory_order_relaxed);
    return cc == 64 ? -1 : cc;
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
    const auto controllerValue = message.getControllerValue();

    // CC64 is reserved for sustain. It never participates in MIDI Learn or
    // parameter automation, so a pedal cannot alter a learned layer control.
    if (cc == 64)
    {
        if (active >= 0)
            juce::Logger::writeToLog("MIDI Learn ignores CC64 because it is reserved for sustain");
        return;
    }

    if (active >= 0)
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

            pendingCCValues[(size_t) layer][(size_t) target].store(normalised,
                                                                      std::memory_order_relaxed);
        }
}

void ClassicPlayerAudioProcessor::processLiveSetSlotMidiMessage(const juce::MidiMessage& message)
{
    if (!message.isController()) return;

    const auto cc = message.getControllerNumber();
    const auto channel = message.getChannel();
    const auto value = message.getControllerValue();

    // CC64 remains exclusively assigned to sustain, including in the Live Set.
    if (cc == 64) return;

    auto learningSlot = activeLiveSetSlotMidiLearn.load(std::memory_order_relaxed);
    if (juce::isPositiveAndBelow(learningSlot, liveSetSlotCount))
    {
        learnedLiveSetSlotCCs[(size_t) learningSlot].store(cc, std::memory_order_relaxed);
        learnedLiveSetSlotChannels[(size_t) learningSlot].store(channel, std::memory_order_relaxed);
        activeLiveSetSlotMidiLearn.compare_exchange_strong(learningSlot, -1,
                                                            std::memory_order_relaxed);
        liveSetSlotMidiLearnChanged.store(true, std::memory_order_release);
        juce::Logger::writeToLog("Live Set MIDI Learn: performance="
                                 + juce::String(learningSlot + 1)
                                 + " CC=" + juce::String(cc)
                                 + " canal=" + juce::String(channel));
        return;
    }

    // Buttons/pads normally send 127 on press and 0 on release. Requiring
    // the upper half prevents the release message from recalling a program.
    if (value < 64) return;
    for (int slot = 0; slot < liveSetSlotCount; ++slot)
    {
        const auto learnedCC = learnedLiveSetSlotCCs[(size_t) slot].load(std::memory_order_relaxed);
        const auto learnedChannel = learnedLiveSetSlotChannels[(size_t) slot].load(std::memory_order_relaxed);
        if (learnedCC == cc && (learnedChannel < 0 || learnedChannel == channel))
        {
            requestedLiveSetSlot.store(slot, std::memory_order_relaxed);
            break;
        }
    }
}

void ClassicPlayerAudioProcessor::beginLiveSetSlotMidiLearn(int bank, int slot)
{
    const auto index = liveSetIndex(bank, slot);
    if (index < 0) return;
    activeLiveSetSlotMidiLearn.store(index, std::memory_order_relaxed);
}

void ClassicPlayerAudioProcessor::resetLiveSetSlotMidiLearn(int bank, int slot)
{
    const auto index = liveSetIndex(bank, slot);
    if (index < 0) return;
    learnedLiveSetSlotCCs[(size_t) index].store(-1, std::memory_order_relaxed);
    learnedLiveSetSlotChannels[(size_t) index].store(-1, std::memory_order_relaxed);
    auto learningSlot = activeLiveSetSlotMidiLearn.load(std::memory_order_relaxed);
    if (learningSlot == index)
        activeLiveSetSlotMidiLearn.compare_exchange_strong(learningSlot, -1,
                                                            std::memory_order_relaxed);
    saveLiveSetState();
}

int ClassicPlayerAudioProcessor::liveSetSlotMidiLearnCC(int bank, int slot) const
{
    const auto index = liveSetIndex(bank, slot);
    return index >= 0 ? learnedLiveSetSlotCCs[(size_t) index].load(std::memory_order_relaxed) : -1;
}

int ClassicPlayerAudioProcessor::liveSetSlotMidiLearnChannel(int bank, int slot) const
{
    const auto index = liveSetIndex(bank, slot);
    return index >= 0 ? learnedLiveSetSlotChannels[(size_t) index].load(std::memory_order_relaxed) : -1;
}

bool ClassicPlayerAudioProcessor::isLiveSetSlotMidiLearning(int bank, int slot) const
{
    const auto index = liveSetIndex(bank, slot);
    return index >= 0
        && activeLiveSetSlotMidiLearn.load(std::memory_order_relaxed) == index;
}

int ClassicPlayerAudioProcessor::consumeRequestedLiveSetSlot()
{
    return requestedLiveSetSlot.exchange(-1, std::memory_order_relaxed);
}

bool ClassicPlayerAudioProcessor::consumeLiveSetSlotMidiLearnChanged()
{
    return liveSetSlotMidiLearnChanged.exchange(false, std::memory_order_acq_rel);
}

void ClassicPlayerAudioProcessor::saveLiveSetSlotMidiLearnState() const
{
    saveLiveSetState();
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
    processLiveSetSlotMidiMessage(message);
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

juce::File ClassicPlayerAudioProcessor::liveSetStorageFile() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Classic Player").getChildFile("LiveSet.xml");
}

int ClassicPlayerAudioProcessor::liveSetIndex(int bank, int slot) const
{
    return (juce::isPositiveAndBelow(bank, liveSetBankCount)
            && juce::isPositiveAndBelow(slot, liveSetSlotsPerBank))
        ? bank * liveSetSlotsPerBank + slot : -1;
}

void ClassicPlayerAudioProcessor::loadLiveSetState()
{
    const auto file = liveSetStorageFile();
    if (!file.existsAsFile()) return;

    const std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
    if (xml == nullptr || !xml->hasTagName("CLASSIC_PLAYER_LIVE_SET")) return;

    const juce::ScopedLock lock(liveSetLock);
    forEachXmlChildElementWithTagName(*xml, slot, "SLOT")
    {
        const auto index = liveSetIndex(slot->getIntAttribute("bank", -1),
                                        slot->getIntAttribute("slot", -1));
        if (index < 0) continue;
        liveSetPrograms[(size_t) index] = slot->getStringAttribute("program");
        learnedLiveSetSlotCCs[(size_t) index].store(slot->getIntAttribute("cc", -1),
                                                     std::memory_order_relaxed);
        learnedLiveSetSlotChannels[(size_t) index].store(slot->getIntAttribute("channel", -1),
                                                          std::memory_order_relaxed);
    }
}

void ClassicPlayerAudioProcessor::saveLiveSetState() const
{
    const auto file = liveSetStorageFile();
    if (file.getParentDirectory().createDirectory().failed()) return;

    juce::XmlElement xml("CLASSIC_PLAYER_LIVE_SET");
    xml.setAttribute("version", 2);
    {
        const juce::ScopedLock lock(liveSetLock);
        for (int bank = 0; bank < liveSetBankCount; ++bank)
        {
            for (int slot = 0; slot < liveSetSlotsPerBank; ++slot)
            {
                const auto index = liveSetIndex(bank, slot);
                const auto& path = liveSetPrograms[(size_t) index];
                const auto cc = learnedLiveSetSlotCCs[(size_t) index].load(std::memory_order_relaxed);
                if (path.isEmpty() && cc < 0) continue;
                auto* item = xml.createNewChildElement("SLOT");
                item->setAttribute("bank", bank);
                item->setAttribute("slot", slot);
                item->setAttribute("program", path);
                if (cc >= 0)
                {
                    item->setAttribute("cc", cc);
                    item->setAttribute("channel",
                        learnedLiveSetSlotChannels[(size_t) index].load(std::memory_order_relaxed));
                }
            }
        }
    }
    xml.writeTo(file);
}

juce::File ClassicPlayerAudioProcessor::liveSetSlotProgram(int bank, int slot) const
{
    const auto index = liveSetIndex(bank, slot);
    if (index < 0) return {};
    const juce::ScopedLock lock(liveSetLock);
    return juce::File(liveSetPrograms[(size_t) index]);
}

juce::String ClassicPlayerAudioProcessor::liveSetSlotName(int bank, int slot) const
{
    const auto file = liveSetSlotProgram(bank, slot);
    return file.existsAsFile() ? file.getFileNameWithoutExtension() : juce::String{};
}

juce::String ClassicPlayerAudioProcessor::liveSetSlotLayerSummary(int bank, int slot) const
{
    const auto program = liveSetSlotProgram(bank, slot);
    if (!program.existsAsFile()) return {};

    juce::MemoryBlock data;
    if (!program.loadFileAsData(data) || data.getSize() == 0) return {};
    const std::unique_ptr<juce::XmlElement> xml(
        getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())));
    if (xml == nullptr) return {};

    juce::StringArray instruments;
    for (int layer = 0; layer < Sf2Engine::layerCount; ++layer)
    {
        const auto number = juce::String(layer + 1);
        auto path = xml->getStringAttribute("externalInstrument" + number);
        if (path.isEmpty())
            path = xml->getStringAttribute("sf2Layer" + number);
        if (path.isNotEmpty())
            instruments.add(juce::File(path).getFileNameWithoutExtension());
    }
    return instruments.joinIntoString(" + ");
}

juce::Result ClassicPlayerAudioProcessor::assignLiveSetSlot(int bank, int slot,
                                                            const juce::File& programFile)
{
    const auto index = liveSetIndex(bank, slot);
    if (index < 0)
        return juce::Result::fail("Posição inválida no Live Set.");
    if (!programFile.existsAsFile() || programFile.getFileExtension().toLowerCase() != ".ckprogram")
        return juce::Result::fail("Selecione uma programação salva válida.");

    {
        const juce::ScopedLock lock(liveSetLock);
        liveSetPrograms[(size_t) index] = programFile.getFullPathName();
    }
    saveLiveSetState();
    return juce::Result::ok();
}

void ClassicPlayerAudioProcessor::clearLiveSetSlot(int bank, int slot)
{
    const auto index = liveSetIndex(bank, slot);
    if (index < 0) return;
    {
        const juce::ScopedLock lock(liveSetLock);
        liveSetPrograms[(size_t) index].clear();
    }
    saveLiveSetState();
}

juce::Result ClassicPlayerAudioProcessor::loadLiveSetSlot(int bank, int slot)
{
    const auto program = liveSetSlotProgram(bank, slot);
    if (!program.existsAsFile())
        return juce::Result::fail("Esta posicao do Live Set nao possui uma programacao salva.");
    return loadProgram(program);
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

juce::Result ClassicPlayerAudioProcessor::deleteProgram(const juce::File& programFile)
{
    const auto programsFolder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                    .getChildFile("Classic Player").getChildFile("Programs");
    if (!programFile.existsAsFile() || programFile.getParentDirectory() != programsFolder
        || programFile.getFileExtension().toLowerCase() != ".ckprogram")
        return juce::Result::fail("Selecione uma programacao salva valida.");

    if (!programFile.deleteFile())
        return juce::Result::fail("Nao foi possivel excluir a programacao.");

    bool liveSetChanged = false;
    {
        const juce::ScopedLock lock(liveSetLock);
        for (auto& path : liveSetPrograms)
        {
            if (path == programFile.getFullPathName())
            {
                path.clear();
                liveSetChanged = true;
            }
        }
    }
    if (liveSetChanged) saveLiveSetState();
    return juce::Result::ok();
}

void ClassicPlayerAudioProcessor::stopAllSoundsBeforeProgramChange()
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    engine.stopAllSounds();
    dx7Engine.stopAllSounds();
    for (auto& instrument : externalInstruments)
        instrument.stopAllSounds();

    const juce::ScopedLock midiLock(midiRoutingLock);
    for (auto& collector : routedMidiCollectors) collector.reset(currentSampleRate);
    for (auto& buffer : routedMidiBuffers) buffer.clear();
    visualMidiCollector.reset(currentSampleRate);
    visualMidiBuffer.clear();
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

    // A performance change is a hard stop: sustained notes from the previous
    // SF2 or external instrument must never remain audible in the next one.
    stopAllSoundsBeforeProgramChange();
    setStateInformation(data.getData(), static_cast<int>(data.getSize()));
    return juce::Result::ok();
}

void ClassicPlayerAudioProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();
    state.setProperty("stateVersion", 163, nullptr);
    state.setProperty("activeLayers", activeLayerCount(), nullptr);
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        state.setProperty("layerType" + juce::String(i + 1),
                          layerTypes[(size_t) i].load(std::memory_order_relaxed), nullptr);
        state.setProperty("dx7Layer" + juce::String(i + 1), dx7Engine.path(i), nullptr);
        const auto key = "sf2Layer" + juce::String(i + 1);
        const auto path = engine.getSoundFontPath(i);
        state.setProperty(key, path.isNotEmpty() ? path : savedPaths[(size_t) i], nullptr);
        state.setProperty("externalInstrument" + juce::String(i + 1),
                          externalInstruments[(size_t) i].getPath(), nullptr);
        // Save the instrument's own preset/state separately. Reusing the
        // existing instance avoids plugin crashes while changing programs.
        state.setProperty("externalInstrumentState" + juce::String(i + 1),
                          externalInstruments[(size_t) i].getState().toBase64Encoding(), nullptr);
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
            state.setProperty("stateVersion", 163, nullptr);
            parameters.replaceState(state);
            for (int i = 0; i < Sf2Engine::layerCount; ++i)
            {
                const auto savedType = juce::jlimit(0, 2, static_cast<int>(state.getProperty(
                    "layerType" + juce::String(i + 1), static_cast<int>(LayerType::sf2))));
                layerTypes[(size_t) i].store(savedType, std::memory_order_relaxed);
                savedPaths[(size_t) i] = savedType == static_cast<int>(LayerType::sf2)
                    ? state.getProperty("sf2Layer" + juce::String(i + 1)).toString() : juce::String{};
                const auto externalPath = savedType == static_cast<int>(LayerType::vst)
                    ? state.getProperty("externalInstrument" + juce::String(i + 1)).toString() : juce::String{};
                const auto externalStateText = state.getProperty(
                    "externalInstrumentState" + juce::String(i + 1)).toString();
                juce::MemoryBlock externalState;
                externalState.fromBase64Encoding(externalStateText);

                // Do not destroy/recreate an already loaded VST merely because
                // a Classic Player program is selected. Some instruments,
                // including Pianoteq, keep native window resources that are
                // invalidated by this unnecessary recreation.
                const auto keepLoadedInstrument = externalPath.isNotEmpty()
                    && supportsExternalInstruments()
                    && externalInstruments[(size_t) i].isLoaded()
                    && externalInstruments[(size_t) i].getPath() == externalPath;

                if (!keepLoadedInstrument)
                {
                    externalInstruments[(size_t) i].unload();
                    if (externalPath.isNotEmpty() && supportsExternalInstruments())
                    {
                        const auto result = externalInstruments[(size_t) i].loadInstrument(
                            juce::File(externalPath), currentSampleRate, currentBlockSize);
                        if (result.failed())
                            juce::Logger::writeToLog("Não foi possível restaurar instrumento externo: "
                                                     + result.getErrorMessage());
                    }
                }

                if (externalInstruments[(size_t) i].isLoaded())
                {
                    engine.unloadSoundFont(i);
                    savedPaths[(size_t) i].clear();
                    if (externalState.getSize() > 0)
                        externalInstruments[(size_t) i].restoreState(externalState);
                }
                else
                {
                    // A program may have fewer layers than the previously
                    // loaded one. Clear the old SoundFont now; restoreLayerPaths()
                    // below will load only the files explicitly stored by the
                    // selected program.
                    engine.unloadSoundFont(i);
                }
                dx7Engine.unload(i);
                if (savedType == static_cast<int>(LayerType::dx7))
                {
                    const auto dx7Path = state.getProperty("dx7Layer" + juce::String(i + 1)).toString();
                    if (dx7Path.isNotEmpty())
                    {
                        const auto result = dx7Engine.loadSysEx(i, juce::File(dx7Path));
                        if (result.failed()) juce::Logger::writeToLog("Não foi possível restaurar DX7: " + result.getErrorMessage());
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
                    const auto restoredCC = static_cast<int>(state.getProperty(
                        "learn" + juce::String(i) + "_" + juce::String(target), -1));
                    // Migration: old CC64 mappings are removed because CC64 is
                    // exclusively reserved for sustain in Classic Player.
                    learnedCCs[(size_t) i][(size_t) target].store(
                        restoredCC == 64 ? -1 : restoredCC);
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
                if (i >= restoredLayerCount)
                {
                    // Never leave an inactive layer holding the instrument from
                    // the previous performance. This releases both SF2 and VST
                    // resources before the next Live Set selection.
                    engine.unloadSoundFont(i);
                    externalInstruments[(size_t) i].unload();
                    dx7Engine.unload(i);
                    savedPaths[(size_t) i].clear();
                    layerTypes[(size_t) i].store(static_cast<int>(LayerType::sf2), std::memory_order_relaxed);
                }
                engine.setConfig(i, config);
            }
            restoreLayerPaths();
        }
    }
}

void ClassicPlayerAudioProcessor::restoreLayerPaths()
{
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
        if (layerType(i) == LayerType::sf2 && savedPaths[(size_t) i].isNotEmpty())
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
