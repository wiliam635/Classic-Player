#include "AudioEngine.h"

namespace classicplayer
{
AudioEngine::AudioEngine(TransportState& transportToUse, MixerState& mixerToUse,
                         InstrumentHost& hostToUse)
    : transportState(transportToUse), mixerState(mixerToUse), instrumentHost(hostToUse)
{
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
    if (mixerState.size() <= 0)
    {
        channelGain.store(1.0f, std::memory_order_relaxed);
        channelPan.store(0.0f, std::memory_order_relaxed);
        channelMuted.store(false, std::memory_order_relaxed);
        anotherChannelIsSoloed.store(false, std::memory_order_relaxed);
    }
    else
    {
        const auto& channel = mixerState.get(0);
        channelGain.store(channel.linearGain(), std::memory_order_relaxed);
        channelPan.store(juce::jlimit(-1.0f, 1.0f, channel.pan), std::memory_order_relaxed);
        channelMuted.store(channel.muted, std::memory_order_relaxed);
        anotherChannelIsSoloed.store(mixerState.anySoloed() && ! channel.solo,
                                     std::memory_order_relaxed);
    }

    masterGain.store(mixerState.masterLinearGain(), std::memory_order_relaxed);
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

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

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

    // The first session track is currently the hosted instrument bus.  Keep
    // the signal path explicit so future audio tracks can reuse the same
    // channel-processing rules without changing plug-in hosting.
    const auto preFaderPeak = output.getNumChannels() > 0
        ? output.getMagnitude(0, numSamples) : 0.0f;
    channelPrePeak.store(preFaderPeak, std::memory_order_relaxed);

    if (channelMuted.load(std::memory_order_relaxed)
        || anotherChannelIsSoloed.load(std::memory_order_relaxed))
    {
        output.clear();
    }
    else
    {
        output.applyGain(channelGain.load(std::memory_order_relaxed));

        if (output.getNumChannels() >= 2)
        {
            const auto pan = channelPan.load(std::memory_order_relaxed);
            const auto left = juce::jlimit(0.0f, 1.0f, 1.0f - pan);
            const auto right = juce::jlimit(0.0f, 1.0f, 1.0f + pan);
            output.applyGain(0, 0, numSamples, left);
            output.applyGain(1, 0, numSamples, right);
        }
    }

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
