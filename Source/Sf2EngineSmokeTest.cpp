#include "Sf2Engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

static int benchmarkSoundFont(const juce::File& file)
{
    Sf2Engine engine;
    constexpr int layerCount = 3;
    for (int layer = 0; layer < layerCount; ++layer)
    {
        const auto loadResult = engine.loadSoundFont(layer, file);
        if (loadResult.failed())
        {
            std::cerr << loadResult.getErrorMessage() << "\n";
            return 3;
        }
    }

    for (const auto rate : { 48000.0, 44100.0 })
        for (const auto blockSize : { 512, 256 })
        {
            engine.prepare(rate, blockSize);
            engine.reset();
            juce::AudioBuffer<float> output(2, blockSize);
            juce::MidiBuffer midi;
            std::vector<double> blockTimes;
            blockTimes.reserve(512);
            float peak = 0.0f;
            for (int block = 0; block < 512; ++block)
            {
                if (block % 32 == 0)
                {
                    for (const int note : { 48, 52, 55, 60, 64, 67, 72, 76 })
                        midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 110), 0);
                }
                if (block % 32 == 24)
                {
                    for (const int note : { 48, 52, 55, 60, 64, 67, 72, 76 })
                        midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                }
                const auto start = std::chrono::steady_clock::now();
                engine.process(output, midi);
                const auto end = std::chrono::steady_clock::now();
                blockTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
                peak = juce::jmax(peak, output.getMagnitude(0, 0, blockSize));
                midi.clear();
            }
            std::sort(blockTimes.begin(), blockTimes.end());
            const auto medianMs = blockTimes[blockTimes.size() / 2];
            const auto p99Ms = blockTimes[blockTimes.size() * 99 / 100];
            const auto deadlineMs = 1000.0 * blockSize / rate;
            std::cout << "rate=" << rate << " block=" << blockSize
                      << " deadlineMs=" << deadlineMs << " medianMs=" << medianMs
                      << " p99Ms=" << p99Ms << " maxMs=" << blockTimes.back()
                      << " peak=" << peak << "\n";
        }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3)
    {
        std::cerr << "Uso: ClassicPlayerEngineSmokeTest arquivo.sf2 [--benchmark]\n";
        return 2;
    }
    if (argc == 3)
        return juce::String::fromUTF8(argv[2]) == "--benchmark"
            ? benchmarkSoundFont(juce::File(juce::String::fromUTF8(argv[1]))) : 2;

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    Sf2Engine engine;
    engine.prepare(sampleRate, blockSize);

    const auto result = engine.loadSoundFont(0, juce::File(juce::String::fromUTF8(argv[1])));
    if (result.failed())
    {
        std::cerr << result.getErrorMessage() << "\n";
        return 3;
    }

    juce::AudioBuffer<float> output(2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);

    float peak = 0.0f;
    for (int block = 0; block < 64; ++block)
    {
        engine.process(output, midi);
        midi.clear();
        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            for (int sample = 0; sample < output.getNumSamples(); ++sample)
                peak = juce::jmax(peak, std::abs(output.getSample(channel, sample)));
    }

    if (peak <= 0.00001f)
        return 4;

    engine.reset();
    auto filteredConfig = engine.getConfig(0);
    filteredConfig.cutoff = 5.0f;
    engine.setConfig(0, filteredConfig);
    juce::AudioBuffer<float> filteredOutput(2, blockSize);
    juce::MidiBuffer filteredMidi;
    filteredMidi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    float filteredPeak = 0.0f;
    for (int block = 0; block < 64; ++block)
    {
        engine.process(filteredOutput, filteredMidi);
        filteredMidi.clear();
        for (int channel = 0; channel < filteredOutput.getNumChannels(); ++channel)
            filteredPeak = juce::jmax(filteredPeak,
                filteredOutput.getMagnitude(channel, 0, filteredOutput.getNumSamples()));
    }
    if (filteredPeak >= peak * 0.8f)
        return 6;

    filteredConfig.cutoff = 100.0f;
    filteredConfig.compressor = 100.0f;
    engine.setConfig(0, filteredConfig);

    // A device-rate change must not recreate the FluidSynth instance or lose
    // the already loaded SoundFont. This also exercises the exact path used by
    // JUCE when an audio device is opened or reconfigured.
    engine.prepare(44100.0, 256);
    juce::AudioBuffer<float> changedRateOutput(2, 256);
    juce::MidiBuffer changedRateMidi;
    changedRateMidi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0);
    float changedRatePeak = 0.0f;
    for (int block = 0; block < 32; ++block)
    {
        engine.process(changedRateOutput, changedRateMidi);
        changedRateMidi.clear();
        for (int channel = 0; channel < changedRateOutput.getNumChannels(); ++channel)
            changedRatePeak = juce::jmax(changedRatePeak,
                changedRateOutput.getMagnitude(channel, 0, changedRateOutput.getNumSamples()));
    }

    // Reproduce the reported 48 -> 44.1 kHz return. On Windows prepare()
    // rebuilds FluidSynth; the loaded SoundFont must still render afterward.
    const auto peakAfterRateChange = [&engine](double rate, int size)
    {
        engine.prepare(rate, size);
        engine.reset();
        juce::AudioBuffer<float> audio(2, size);
        juce::MidiBuffer notes;
        notes.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 0);
        float peakResult = 0.0f;
        for (int block = 0; block < 32; ++block)
        {
            engine.process(audio, notes);
            notes.clear();
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                peakResult = juce::jmax(peakResult, audio.getMagnitude(channel, 0, size));
        }
        return peakResult;
    };
    const auto returnedTo48Peak = peakAfterRateChange(48000.0, 512);
    const auto returnedTo44Peak = peakAfterRateChange(44100.0, 512);
    const auto returnedTo44SmallBufferPeak = peakAfterRateChange(44100.0, 256);

    std::cout << "peak=" << peak << " cutoffPeak=" << filteredPeak
              << " changedRatePeak=" << changedRatePeak
              << " returnedTo48Peak=" << returnedTo48Peak
              << " returnedTo44Peak=" << returnedTo44Peak
              << " returnedTo44SmallBufferPeak=" << returnedTo44SmallBufferPeak
              << " block=" << blockSize << " rate=" << sampleRate << "\n";
    return changedRatePeak > 0.00001f && returnedTo48Peak > 0.00001f
           && returnedTo44Peak > 0.00001f && returnedTo44SmallBufferPeak > 0.00001f
           ? 0 : 5;
}
