#pragma once

#include "AudioRecorder.h"
#include "InstrumentHost.h"
#include "MixerState.h"
#include "TransportState.h"
#include <atomic>
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
    AudioEngine(TransportState&, MixerState&, InstrumentHost&);
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(double preferredSampleRate, int preferredBufferSize,
               juce::String& errorMessage);
    void stop() noexcept;

    bool isRunning() const noexcept { return running; }
    double sampleRate() const noexcept { return activeSampleRate; }
    int bufferSize() const noexcept { return activeBufferSize; }
    // Copies UI-owned mixer values into atomics consumed by the real-time
    // callback. Call this after changing a mixer control and from the UI timer.
    void refreshMixerSnapshot() noexcept;
    bool startRecording(const juce::File&, juce::String& errorMessage);
    void stopRecording() noexcept;
    bool isRecording() const noexcept { return recorder.isRecording(); }
    juce::File recordingFile() const noexcept { return recorder.outputFile(); }
    float channelPreFaderPeak() const noexcept { return channelPrePeak.load(std::memory_order_relaxed); }
    float channelPostFaderPeak() const noexcept { return channelPostPeak.load(std::memory_order_relaxed); }
    float masterPeak() const noexcept { return masterPeakValue.load(std::memory_order_relaxed); }
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
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    juce::AudioDeviceManager deviceManagerValue;
    double activeSampleRate { 44100.0 };
    int activeBufferSize { 512 };
    bool running { false };
    juce::MidiMessageCollector midiCollector;
    std::atomic<float> channelGain { 1.0f };
    std::atomic<float> channelPan { 0.0f };
    std::atomic<float> masterGain { 1.0f };
    std::atomic<bool> channelMuted { false };
    std::atomic<bool> anotherChannelIsSoloed { false };
    std::atomic<float> channelPrePeak { 0.0f };
    std::atomic<float> channelPostPeak { 0.0f };
    std::atomic<float> masterPeakValue { 0.0f };
    AudioRecorder recorder;
    juce::AudioBuffer<float> recorderInputBuffer;
};
}
