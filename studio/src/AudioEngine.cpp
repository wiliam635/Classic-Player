#include "AudioEngine.h"

namespace classicplayer
{
AudioEngine::AudioEngine(TransportState& transportToUse, InstrumentHost& hostToUse)
    : transportState(transportToUse), instrumentHost(hostToUse)
{
}

AudioEngine::~AudioEngine()
{
    stop();
}

bool AudioEngine::start(double preferredSampleRate, int preferredBufferSize,
                        juce::String& errorMessage)
{
    if (running)
        return true;

    juce::AudioDeviceManager::AudioDeviceSetup preferredSetup;
    preferredSetup.sampleRate = juce::jmax(8000.0, preferredSampleRate);
    preferredSetup.bufferSize = juce::jmax(16, preferredBufferSize);

    errorMessage = deviceManagerValue.initialise(0, 2, nullptr, true, {}, &preferredSetup);
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

    juce::MidiBuffer midi;
    midiCollector.removeNextBlockOfMessages(midi, numSamples);
    instrumentHost.processBlock(output, midi);
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
    instrumentHost.prepareToPlay(activeSampleRate, activeBufferSize);
}

void AudioEngine::audioDeviceStopped()
{
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
