#include "SampleEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>

int main()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("classic-player-sample-test", ".wav");
    juce::WavAudioFormat wav;
    {
        auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
        if (stream == nullptr) return 1;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
            stream.release(), 48000.0, 1, 16, {}, 0));
        if (writer == nullptr) return 2;
        juce::AudioBuffer<float> source(1, 480);
        for (int sample = 0; sample < source.getNumSamples(); ++sample)
            source.setSample(0, sample, std::sin(juce::MathConstants<double>::twoPi
                                                 * 440.0 * sample / 48000.0) * 0.5f);
        writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
    }

    SampleEngine engine;
    engine.prepare(48000.0, 128);
    const auto loaded = engine.loadSample(0, file);
    if (loaded.failed()) { file.deleteFile(); return 3; }

    std::array<juce::MidiBuffer, SampleEngine::layerCount> midi;
    midi[0].addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    std::array<Sf2Engine::LayerConfig, SampleEngine::layerCount> routing;
    routing[0].enabled = true;
    routing[0].gain = 1.0f;
    juce::AudioBuffer<float> output(2, 128);
    output.clear();
    engine.process(output, midi, routing);
    const auto peak = output.getMagnitude(0, 0, output.getNumSamples());
    const auto loadedPath = engine.path(0);
    engine.stopAllSounds();
    engine.unload(0);
    file.deleteFile();

    if (loadedPath.isEmpty() || peak <= 0.01f)
    {
        std::cerr << "sample engine test failed: peak=" << peak << std::endl;
        return 4;
    }
    std::cout << "sample engine test passed: peak=" << peak << std::endl;
    return 0;
}
