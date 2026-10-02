#pragma once

#include "InstrumentHost.h"
#include "TransportState.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace classicplayer
{
/**
    Owns the desktop audio device and connects it to the Studio transport and
    instrument host. Device setup stays in this class so the UI never needs to
    touch the real-time callback directly.
*/
class AudioEngine final : private juce::AudioIODeviceCallback,
                          private juce::MidiInputCallback
{
public:
    AudioEngine(TransportState&, InstrumentHost&);
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(double preferredSampleRate, int preferredBufferSize,
               juce::String& errorMessage);
    void stop() noexcept;

    bool isRunning() const noexcept { return running; }
    double sampleRate() const noexcept { return activeSampleRate; }
    int bufferSize() const noexcept { return activeBufferSize; }
    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManagerValue; }

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;

    TransportState& transportState;
    InstrumentHost& instrumentHost;
    juce::AudioDeviceManager deviceManagerValue;
    double activeSampleRate { 44100.0 };
    int activeBufferSize { 512 };
    bool running { false };
    juce::MidiMessageCollector midiCollector;
};
}
