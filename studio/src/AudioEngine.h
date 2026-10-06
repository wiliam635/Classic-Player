#pragma once

#include "AudioRecorder.h"
#include "InstrumentHost.h"
#include "MixerState.h"
#include "Session.h"
#include "TransportState.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
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
    AudioEngine(Session&, TransportState&, MixerState&, InstrumentHost&);
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
    // Rebuilds the immutable audio-clip snapshot from the current session.
    // File I/O happens on the caller's (UI) thread; the callback only reads
    // the published snapshot. Missing files are reported as a warning while
    // valid clips remain available for playback.
    bool reloadClipSources(juce::String& statusMessage);
    int loadedClipCount() const noexcept { return loadedClipCountValue.load(std::memory_order_relaxed); }
    bool startRecording(const juce::File&, juce::String& errorMessage);
    void stopRecording() noexcept;
    bool isRecording() const noexcept { return recorder.isRecording(); }
    juce::File recordingFile() const noexcept { return recorder.outputFile(); }
    int64_t recordedSamples() const noexcept { return recorder.recordedSamples(); }
    float channelPreFaderPeak() const noexcept { return channelPrePeak.load(std::memory_order_relaxed); }
    float channelPostFaderPeak() const noexcept { return channelPostPeak.load(std::memory_order_relaxed); }
    float trackPostFaderPeak(int index) const noexcept
    {
        return juce::isPositiveAndBelow(index, maxTrackChannels)
            ? trackPostPeaks[static_cast<std::size_t>(index)].load(std::memory_order_relaxed) : 0.0f;
    }
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

    struct ClipPlaybackSource
    {
        int trackIndex { 0 };
        std::int64_t startSample { 0 };
        std::int64_t lengthSamples { 0 };
        double sourceSampleRate { 44100.0 };
        juce::AudioBuffer<float> samples;
    };

    struct ClipPlaybackState
    {
        double timelineSampleRate { 44100.0 };
        std::vector<ClipPlaybackSource> sources;
    };

    static constexpr int maxTrackChannels = 64;

    void mixClipSources(juce::AudioBuffer<float>& destination,
                        const ClipPlaybackState& state,
                        int trackIndex,
                        std::int64_t timelineStartSample,
                        int numSamples) noexcept;
    void processTrackBuffer(juce::AudioBuffer<float>& buffer,
                            int trackIndex,
                            int numSamples) noexcept;

    Session& sessionState;
    TransportState& transportState;
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    juce::AudioDeviceManager deviceManagerValue;
    juce::AudioFormatManager formatManager;
    std::shared_ptr<const ClipPlaybackState> clipPlaybackState;
    juce::AudioBuffer<float> clipScratchBuffer;
    double activeSampleRate { 44100.0 };
    int activeBufferSize { 512 };
    bool running { false };
    juce::MidiMessageCollector midiCollector;
    std::array<std::atomic<float>, maxTrackChannels> trackGains {};
    std::array<std::atomic<float>, maxTrackChannels> trackPans {};
    std::array<std::atomic<bool>, maxTrackChannels> trackMuted {};
    std::array<std::atomic<bool>, maxTrackChannels> trackSoloed {};
    std::array<std::atomic<float>, maxTrackChannels> trackPrePeaks {};
    std::array<std::atomic<float>, maxTrackChannels> trackPostPeaks {};
    std::atomic<int> trackCount { 0 };
    std::atomic<bool> anyTrackIsSoloed { false };
    std::atomic<float> masterGain { 1.0f };
    std::atomic<float> channelPrePeak { 0.0f };
    std::atomic<float> channelPostPeak { 0.0f };
    std::atomic<float> masterPeakValue { 0.0f };
    std::atomic<int> loadedClipCountValue { 0 };
    AudioRecorder recorder;
    juce::AudioBuffer<float> recorderInputBuffer;
};
}
