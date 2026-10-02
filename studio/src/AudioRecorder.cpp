#include "AudioRecorder.h"

#include <cmath>

namespace classicplayer
{
AudioRecorder::AudioRecorder()
{
    backgroundThread.startThread();
}

AudioRecorder::~AudioRecorder()
{
    stop();
    backgroundThread.stopThread(2000);
}

bool AudioRecorder::start(const juce::File& file, double sampleRate, int numChannels,
                          juce::String& errorMessage)
{
    stop();

    if (! std::isfinite(sampleRate) || sampleRate <= 0.0 || numChannels <= 0)
    {
        errorMessage = "Parâmetros de gravação inválidos";
        return false;
    }

    const auto output = file.withFileExtension("wav");
    if (output.getParentDirectory().createDirectory().failed())
    {
        errorMessage = "Não foi possível criar a pasta de gravação";
        return false;
    }

    std::unique_ptr<juce::OutputStream> stream = output.createOutputStream();
    if (stream == nullptr)
    {
        errorMessage = "Não foi possível criar o arquivo WAV";
        return false;
    }

    juce::WavAudioFormat wavFormat;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate(sampleRate)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(24);
    auto writer = wavFormat.createWriterFor(stream, options);
    if (writer == nullptr)
    {
        errorMessage = "O formato WAV não aceitou a configuração de áudio";
        return false;
    }

    {
        const juce::ScopedLock lock(writerLock);
        threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), backgroundThread, 32768);
        outputFileValue = output;
        recording.store(true, std::memory_order_release);
    }

    errorMessage.clear();
    return true;
}

void AudioRecorder::stop() noexcept
{
    recording.store(false, std::memory_order_release);
    const juce::ScopedLock lock(writerLock);
    threadedWriter.reset();
}

juce::File AudioRecorder::outputFile() const noexcept
{
    const juce::ScopedLock lock(writerLock);
    return outputFileValue;
}

void AudioRecorder::pushInput(const float* const* inputChannelData, int numChannels,
                              int numSamples) noexcept
{
    juce::ignoreUnused(numChannels);

    if (! recording.load(std::memory_order_acquire)
        || inputChannelData == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    const juce::ScopedTryLock lock(writerLock);
    if (! lock.isLocked() || threadedWriter == nullptr)
        return;

    threadedWriter->write(inputChannelData, numSamples);
}
}
