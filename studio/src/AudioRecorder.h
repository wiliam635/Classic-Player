#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

namespace classicplayer
{
/** Writes the device input to a WAV file from a real-time callback.

    ThreadedWriter owns the file writer and flushes it from a background
    TimeSliceThread. The short critical section only protects start/stop from
    a simultaneous callback; the file encoding itself never runs on the audio
    thread.
*/
class AudioRecorder final
{
public:
    AudioRecorder();
    ~AudioRecorder();

    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    bool start(const juce::File&, double sampleRate, int numChannels,
               juce::String& errorMessage);
    void stop() noexcept;
    bool isRecording() const noexcept { return recording.load(std::memory_order_acquire); }
    juce::File outputFile() const noexcept;

    void pushInput(const float* const* inputChannelData, int numChannels,
                   int numSamples) noexcept;

private:
    mutable juce::CriticalSection writerLock;
    juce::TimeSliceThread backgroundThread { "Classic Player Studio Recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<bool> recording { false };
    juce::File outputFileValue;
};
}
