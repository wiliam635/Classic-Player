#include "Sf2Engine.h"
#include <cmath>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Uso: ClassicPlayerEngineSmokeTest arquivo.sf2\n";
        return 2;
    }

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

    std::cout << "peak=" << peak << " cutoffPeak=" << filteredPeak
              << " changedRatePeak=" << changedRatePeak
              << " block=" << blockSize << " rate=" << sampleRate << "\n";
    return changedRatePeak > 0.00001f ? 0 : 5;
}
