#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classicplayer
{
AudioEngine::AudioEngine(Session& sessionToUse, TransportState& transportToUse,
                         MixerState& mixerToUse, InstrumentHost& hostToUse)
    : sessionState(sessionToUse), transportState(transportToUse), mixerState(mixerToUse),
      instrumentHost(hostToUse)
{
    formatManager.registerBasicFormats();

    for (int i = 0; i < maxTrackChannels; ++i)
    {
        trackGains[static_cast<size_t>(i)].store(1.0f, std::memory_order_relaxed);
        trackPans[static_cast<size_t>(i)].store(0.0f, std::memory_order_relaxed);
        trackMuted[static_cast<size_t>(i)].store(false, std::memory_order_relaxed);
        trackSoloed[static_cast<size_t>(i)].store(false, std::memory_order_relaxed);
    }

    std::shared_ptr<const ClipPlaybackState> empty = std::make_shared<ClipPlaybackState>();
    std::atomic_store_explicit(&clipPlaybackState, std::move(empty), std::memory_order_release);
}

AudioEngine::~AudioEngine()
{
    stop();
}

bool AudioEngine::startRecording(const juce::File& file, juce::String& errorMessage)
{
    if (! running)
    {
        errorMessage = "Inicie o áudio antes de gravar";
        return false;
    }

    return recorder.start(file, activeSampleRate, recorderInputBuffer.getNumChannels(),
                          errorMessage);
}

void AudioEngine::stopRecording() noexcept
{
    recorder.stop();
}

void AudioEngine::refreshMixerSnapshot() noexcept
{
    const auto count = juce::jmin(mixerState.size(), maxTrackChannels);
    bool hasSolo = false;

    for (int i = 0; i < count; ++i)
    {
        const auto& channel = mixerState.get(i);
        const auto index = static_cast<size_t>(i);
        trackGains[index].store(channel.linearGain(), std::memory_order_relaxed);
        trackPans[index].store(juce::jlimit(-1.0f, 1.0f, channel.pan),
                               std::memory_order_relaxed);
        trackMuted[index].store(channel.muted, std::memory_order_relaxed);
        trackSoloed[index].store(channel.solo, std::memory_order_relaxed);
        hasSolo = hasSolo || channel.solo;
    }

    for (int i = count; i < maxTrackChannels; ++i)
    {
        const auto index = static_cast<size_t>(i);
        trackGains[index].store(1.0f, std::memory_order_relaxed);
        trackPans[index].store(0.0f, std::memory_order_relaxed);
        trackMuted[index].store(false, std::memory_order_relaxed);
        trackSoloed[index].store(false, std::memory_order_relaxed);
    }

    trackCount.store(count, std::memory_order_release);
    anyTrackIsSoloed.store(hasSolo, std::memory_order_release);
    masterGain.store(mixerState.masterLinearGain(), std::memory_order_relaxed);
}

bool AudioEngine::reloadClipSources(juce::String& statusMessage)
{
    const auto timelineRate = juce::jlimit(8000.0, 192000.0, transportState.sampleRate());
    auto next = std::make_shared<ClipPlaybackState>();
    next->timelineSampleRate = timelineRate;
    juce::StringArray failures;

    for (int trackIndex = 0; trackIndex < sessionState.tracks.size(); ++trackIndex)
    {
        const auto& track = sessionState.tracks[trackIndex];
        for (const auto& clip : track.clips)
        {
            if (clip.filePath.isEmpty())
                continue;

            const juce::File file(clip.filePath);
            if (! file.existsAsFile())
            {
                failures.add(clip.name + " (arquivo não encontrado)");
                continue;
            }

            std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
            if (reader == nullptr || reader->lengthInSamples <= 0
                || reader->lengthInSamples > std::numeric_limits<int>::max())
            {
                failures.add(clip.name + " (formato não suportado)");
                continue;
            }

            const auto sourceRate = std::isfinite(reader->sampleRate) && reader->sampleRate > 0.0
                                        ? reader->sampleRate : timelineRate;
            const auto sourceChannels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
            const auto sourceSamples = static_cast<int>(reader->lengthInSamples);

            ClipPlaybackSource source;
            source.trackIndex = trackIndex;
            source.startSample = static_cast<std::int64_t>(
                std::llround(juce::jmax(0.0, clip.startSeconds) * timelineRate));
            source.sourceSampleRate = sourceRate;
            source.samples.setSize(sourceChannels, sourceSamples, false, true, true);

            if (! reader->read(source.samples.getArrayOfWritePointers(), sourceChannels,
                               0, sourceSamples))
            {
                failures.add(clip.name + " (falha ao ler)");
                continue;
            }

            const auto sourceDuration = static_cast<double>(sourceSamples) / sourceRate;
            const auto requestedDuration = clip.lengthSeconds > 0.0
                                               ? clip.lengthSeconds : sourceDuration;
            const auto duration = juce::jmin(sourceDuration, juce::jmax(0.0, requestedDuration));
            source.lengthSamples = static_cast<std::int64_t>(
                std::llround(duration * timelineRate));
            if (source.lengthSamples <= 0)
                continue;

            next->sources.push_back(std::move(source));
        }
    }

    std::shared_ptr<const ClipPlaybackState> published = std::move(next);
    std::atomic_store_explicit(&clipPlaybackState, std::move(published),
                               std::memory_order_release);

    // The shared pointer is now owned by the atomic slot. Load the published
    // snapshot once to expose its count without touching the callback's data.
    const auto publishedSnapshot = std::atomic_load_explicit(&clipPlaybackState,
                                                               std::memory_order_acquire);
    const auto loaded = publishedSnapshot != nullptr
                            ? static_cast<int>(publishedSnapshot->sources.size()) : 0;
    loadedClipCountValue.store(loaded, std::memory_order_relaxed);

    statusMessage = "Clipes de áudio: " + juce::String(loaded) + " carregado(s)";
    if (! failures.isEmpty())
        statusMessage += " · ignorados: " + failures.joinIntoString(", ");
    return failures.isEmpty();
}

bool AudioEngine::start(double preferredSampleRate, int preferredBufferSize,
                        juce::String& errorMessage)
{
    if (running)
        return true;

    refreshMixerSnapshot();

    juce::AudioDeviceManager::AudioDeviceSetup preferredSetup;
    preferredSetup.sampleRate = juce::jmax(8000.0, preferredSampleRate);
    preferredSetup.bufferSize = juce::jmax(16, preferredBufferSize);

    errorMessage = deviceManagerValue.initialise(2, 2, nullptr, true, {}, &preferredSetup);
    if (errorMessage.isNotEmpty())
    {
        // Some output-only devices cannot open input channels. Keep playback
        // usable in that case; recording will simply receive no input signal.
        deviceManagerValue.closeAudioDevice();
        errorMessage = deviceManagerValue.initialise(0, 2, nullptr, true, {}, &preferredSetup);
    }
    if (errorMessage.isNotEmpty())
        return false;

    if (auto* device = deviceManagerValue.getCurrentAudioDevice(); device != nullptr)
    {
        activeSampleRate = device->getCurrentSampleRate();
        activeBufferSize = device->getCurrentBufferSizeSamples();
        transportState.setSampleRate(activeSampleRate);
    }

    juce::String clipStatus;
    reloadClipSources(clipStatus);

    deviceManagerValue.addAudioCallback(this);
    deviceManagerValue.addMidiInputDeviceCallback({}, this);
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        deviceManagerValue.setMidiInputDeviceEnabled(device.identifier, true);
    running = true;
    return true;
}

void AudioEngine::stop() noexcept
{
    if (! running)
        return;

    deviceManagerValue.removeMidiInputDeviceCallback({}, this);
    deviceManagerValue.removeAudioCallback(this);
    deviceManagerValue.closeAudioDevice();
    running = false;
}

void AudioEngine::mixClipSources(juce::AudioBuffer<float>& destination,
                                 const ClipPlaybackState& state,
                                 int trackIndex,
                                 std::int64_t timelineStartSample,
                                 int numSamples) noexcept
{
    if (numSamples <= 0 || destination.getNumChannels() <= 0
        || state.timelineSampleRate <= 0.0)
        return;

    const auto timelineEndSample = timelineStartSample + static_cast<std::int64_t>(numSamples);
    for (const auto& source : state.sources)
    {
        if (source.trackIndex != trackIndex || source.lengthSamples <= 0
            || source.samples.getNumSamples() <= 0)
            continue;

        const auto sourceStart = source.startSample;
        const auto sourceEnd = sourceStart + source.lengthSamples;
        const auto overlapStart = juce::jmax(timelineStartSample, sourceStart);
        const auto overlapEnd = juce::jmin(timelineEndSample, sourceEnd);
        if (overlapStart >= overlapEnd)
            continue;

        for (auto timelineSample = overlapStart; timelineSample < overlapEnd; ++timelineSample)
        {
            const auto destinationSample = static_cast<int>(timelineSample - timelineStartSample);
            const auto sourcePosition = static_cast<double>(timelineSample - sourceStart)
                                        * source.sourceSampleRate / state.timelineSampleRate;
            const auto sourceIndex = static_cast<int>(std::floor(sourcePosition));
            if (sourceIndex < 0 || sourceIndex >= source.samples.getNumSamples())
                continue;

            const auto fraction = static_cast<float>(sourcePosition - sourceIndex);
            for (int destinationChannel = 0;
                 destinationChannel < destination.getNumChannels(); ++destinationChannel)
            {
                const auto sourceChannel = juce::jmin(destinationChannel,
                                                       source.samples.getNumChannels() - 1);
                const auto first = source.samples.getSample(sourceChannel, sourceIndex);
                const auto nextIndex = sourceIndex + 1;
                const auto second = nextIndex < source.samples.getNumSamples()
                                        ? source.samples.getSample(sourceChannel, nextIndex)
                                        : first;
                const auto sample = first + (second - first) * fraction;
                destination.addSample(destinationChannel, destinationSample, sample);
            }
        }
    }
}

void AudioEngine::processTrackBuffer(juce::AudioBuffer<float>& buffer,
                                     int trackIndex,
                                     int numSamples) noexcept
{
    const auto count = trackCount.load(std::memory_order_acquire);
    if (trackIndex < 0 || trackIndex >= count || trackIndex >= maxTrackChannels)
    {
        buffer.clear();
        return;
    }

    const auto index = static_cast<size_t>(trackIndex);
    if (trackMuted[index].load(std::memory_order_relaxed)
        || (anyTrackIsSoloed.load(std::memory_order_relaxed)
            && ! trackSoloed[index].load(std::memory_order_relaxed)))
    {
        buffer.clear();
        return;
    }

    buffer.applyGain(trackGains[index].load(std::memory_order_relaxed));
    if (buffer.getNumChannels() >= 2)
    {
        const auto pan = trackPans[index].load(std::memory_order_relaxed);
        const auto left = juce::jlimit(0.0f, 1.0f, 1.0f - pan);
        const auto right = juce::jlimit(0.0f, 1.0f, 1.0f + pan);
        buffer.applyGain(0, 0, numSamples, left);
        buffer.applyGain(1, 0, numSamples, right);
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);

    juce::AudioBuffer<float> output(outputChannelData, numOutputChannels, numSamples);
    output.clear();

    if (recorder.isRecording() && recorderInputBuffer.getNumSamples() >= numSamples)
    {
        recorderInputBuffer.clear();
        for (int channel = 0; channel < recorderInputBuffer.getNumChannels(); ++channel)
            if (channel < numInputChannels && inputChannelData != nullptr
                && inputChannelData[channel] != nullptr)
                recorderInputBuffer.copyFrom(channel, 0, inputChannelData[channel], numSamples);

        recorder.pushInput(recorderInputBuffer.getArrayOfReadPointers(),
                           recorderInputBuffer.getNumChannels(), numSamples);
    }

    juce::MidiBuffer midi;
    midiCollector.removeNextBlockOfMessages(midi, numSamples);
    instrumentHost.processBlock(output, midi);

    const auto timelineStartSample = transportState.positionSamples();
    const auto isPlaying = transportState.isPlaying();
    const auto clipState = std::atomic_load_explicit(&clipPlaybackState,
                                                     std::memory_order_acquire);
    const auto count = trackCount.load(std::memory_order_acquire);

    if (isPlaying && clipState != nullptr)
    {
        // The hosted instrument and clips assigned to the first track share
        // one bus. Additional audio tracks use the preallocated scratch
        // buffer and are summed after their own mixer processing.
        mixClipSources(output, *clipState, 0, timelineStartSample, numSamples);
        for (int trackIndex = 1; trackIndex < count; ++trackIndex)
        {
            if (clipScratchBuffer.getNumSamples() < numSamples)
                break;

            clipScratchBuffer.clear();
            mixClipSources(clipScratchBuffer, *clipState, trackIndex,
                           timelineStartSample, numSamples);
            processTrackBuffer(clipScratchBuffer, trackIndex, numSamples);
            for (int channel = 0; channel < output.getNumChannels(); ++channel)
            {
                const auto sourceChannel = juce::jmin(channel,
                                                       clipScratchBuffer.getNumChannels() - 1);
                output.addFrom(channel, 0, clipScratchBuffer, sourceChannel, 0, numSamples);
            }
        }
    }

    // The first session track is the hosted instrument bus. Its meter is
    // measured before and after the track gain/pan/mute processing.
    const auto preFaderPeak = output.getNumChannels() > 0
        ? output.getMagnitude(0, numSamples) : 0.0f;
    channelPrePeak.store(preFaderPeak, std::memory_order_relaxed);
    processTrackBuffer(output, 0, numSamples);
    channelPostPeak.store(output.getNumChannels() > 0
                              ? output.getMagnitude(0, numSamples) : 0.0f,
                          std::memory_order_relaxed);

    output.applyGain(masterGain.load(std::memory_order_relaxed));
    masterPeakValue.store(output.getNumChannels() > 0
                              ? output.getMagnitude(0, numSamples) : 0.0f,
                          std::memory_order_relaxed);
    transportState.advanceSamples(numSamples);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    activeSampleRate = device->getCurrentSampleRate();
    activeBufferSize = device->getCurrentBufferSizeSamples();
    transportState.setSampleRate(activeSampleRate);
    midiCollector.reset(activeSampleRate);
    recorderInputBuffer.setSize(2, activeBufferSize, false, true, true);
    clipScratchBuffer.setSize(2, activeBufferSize, false, true, true);
    instrumentHost.prepareToPlay(activeSampleRate, activeBufferSize);
}

void AudioEngine::audioDeviceStopped()
{
    recorder.stop();
    instrumentHost.releaseResources();
}

void AudioEngine::audioDeviceError(const juce::String& errorMessage)
{
    juce::Logger::writeToLog("Studio audio device error: " + errorMessage);
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput* source,
                                            const juce::MidiMessage& message)
{
    juce::ignoreUnused(source);
    midiCollector.addMessageToQueue(message);
}
}
