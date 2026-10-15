#include "SampleEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
int diagnoseNordPiano(const juce::File& file)
{
    NordPianoDecoder::Instrument piano;
    const auto decoded = NordPianoDecoder::decodeFile(file, piano);
    if (decoded.failed())
    {
        std::cerr << "Nord decode failed: " << decoded.getErrorMessage() << '\n';
        return 10;
    }

    std::cout << "instrument=" << piano.name << " channels=" << piano.channels
              << " strokes=" << piano.strokes.size()
              << " globalGainDb=" << static_cast<double>(piano.gainTenthsDb) / 10.0 << '\n';
    int highestLayer = 0;
    int negativeTrims = 0;
    for (const auto& stroke : piano.strokes)
    {
        if (stroke.bank == 0) highestLayer = std::max(highestLayer, static_cast<int>(stroke.velocityLayer));
        if (stroke.trimDb >= 0x8000u) ++negativeTrims;
    }
    std::cout << "highestAttackLayer=" << highestLayer
              << " negativeTrimRecords=" << negativeTrims << '\n';
    for (int bank = 0; bank <= 2; ++bank)
    {
        int count = 0;
        int minFrames = std::numeric_limits<int>::max();
        int maxFrames = 0;
        for (const auto& stroke : piano.strokes)
            if (stroke.bank == bank)
            {
                ++count;
                minFrames = std::min(minFrames, stroke.frameCount(piano.channels));
                maxFrames = std::max(maxFrames, stroke.frameCount(piano.channels));
            }
        std::cout << "bank=" << bank << " strokes=" << count
                  << " minFrames=" << (count ? minFrames : 0)
                  << " maxFrames=" << maxFrames << '\n';
    }
    for (const auto note : { 48, 60, 72 })
    {
        const auto root = piano.keyMap[(size_t) note];
        std::cout << "note=" << note << " root=" << static_cast<int>(root)
                  << " fineTune=" << static_cast<int>(piano.fineTune[(size_t) note]) << '\n';
        for (const auto& stroke : piano.strokes)
        {
            if (stroke.rootNote != root || stroke.bank != 0) continue;
            if (stroke.velocityLayer == 0)
                std::cout << "  loopStart=" << stroke.loopStartFrame
                          << " loopEnd=" << stroke.loopEndFrame
                          << " frames=" << stroke.frameCount(piano.channels) << '\n';
            if (note == 60 && stroke.velocityLayer == 27)
            {
                const auto start = static_cast<size_t>(stroke.loopStartFrame) * piano.channels;
                const auto end = static_cast<size_t>(stroke.loopEndFrame) * piano.channels;
                std::cout << "  loopBoundaryDelta="
                          << std::abs(static_cast<int>(stroke.interleavedPcm[end])
                                    - static_cast<int>(stroke.interleavedPcm[start])) / 32768.0
                          << '\n';
            }
            const auto frames = juce::jmin(12000, stroke.frameCount(piano.channels));
            double squared = 0.0;
            double peak = 0.0;
            size_t saturated = 0;
            for (int frame = 0; frame < frames; ++frame)
                for (int channel = 0; channel < piano.channels; ++channel)
                {
                    const auto value = stroke.interleavedPcm[(size_t) (frame * piano.channels + channel)];
                    const auto normalized = static_cast<double>(value) / 32768.0;
                    squared += normalized * normalized;
                    peak = std::max(peak, std::abs(normalized));
                    saturated += static_cast<size_t>(value == std::numeric_limits<std::int16_t>::min()
                                                  || value == std::numeric_limits<std::int16_t>::max());
                }
            const auto rms = frames > 0
                ? std::sqrt(squared / static_cast<double>(frames * piano.channels)) : 0.0;
            std::cout << "  layer=" << static_cast<int>(stroke.velocityLayer)
                      << " trimDb=" << stroke.trimDb << " pcmPeak=" << peak
                      << " pcmRms=" << rms << " saturated=" << saturated << '\n';
        }
    }

    std::vector<int> polyphonyTestNotes;
    constexpr int testVelocity = 100;
    const auto layerLimit = ((127 - testVelocity) * piano.velocityScale) / 127;
    for (int note = 0; note < 128; ++note)
    {
        if (note == 60) continue;
        const auto root = piano.keyMap[(size_t) note];
        if (root == 0xff) continue;
        const auto hasAttack = std::any_of(piano.strokes.begin(), piano.strokes.end(),
            [root, layerLimit](const auto& stroke)
            {
                return stroke.rootNote == root && stroke.bank == 0
                    && stroke.velocityLayer <= layerLimit;
            });
        if (hasAttack) polyphonyTestNotes.push_back(note);
    }
    if (polyphonyTestNotes.empty())
    {
        std::cerr << "No mapped notes available for the polyphony test\n";
        return 15;
    }
    std::cout << "polyphonyTestMappedNotes=" << polyphonyTestNotes.size() << '\n';
    piano = {};

    SampleEngine engine;
    constexpr int blockSize = 256;
    engine.prepare(48000.0, blockSize);
    const auto loaded = engine.loadSample(0, file);
    if (loaded.failed())
    {
        std::cerr << "Nord playback load failed: " << loaded.getErrorMessage() << '\n';
        return 11;
    }

    std::array<Sf2Engine::LayerConfig, SampleEngine::layerCount> routing;
    routing[0].enabled = true;
    routing[0].gain = 1.0f;
    juce::AudioBuffer<float> output(2, blockSize);
    for (const auto note : { 48, 60, 72 })
        for (const auto velocity : { 30, 80, 120 })
        {
            engine.stopAllSounds();
            std::array<juce::MidiBuffer, SampleEngine::layerCount> midi;
            midi[0].addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) velocity), 0);
            double squared = 0.0;
            double peak = 0.0;
            for (int block = 0; block < 100; ++block)
            {
                output.clear();
                engine.process(output, midi, routing);
                midi[0].clear();
                for (int channel = 0; channel < output.getNumChannels(); ++channel)
                    for (int sample = 0; sample < output.getNumSamples(); ++sample)
                    {
                        const auto value = static_cast<double>(output.getSample(channel, sample));
                        squared += value * value;
                        peak = std::max(peak, std::abs(value));
                    }
            }
            const auto rms = std::sqrt(squared / (100.0 * blockSize * output.getNumChannels()));
            std::cout << "render note=" << note << " velocity=" << velocity
                      << " peak=" << peak << " rms=" << rms
                      << " rmsDbfs=" << 20.0 * std::log10(std::max(rms, 1.0e-12)) << '\n';
        }

    // Reproduce a chord whose keys are released while CC64 remains down.
    engine.stopAllSounds();
    std::array<juce::MidiBuffer, SampleEngine::layerCount> midi;
    midi[0].addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    for (const auto note : { 48, 60, 64, 67 })
        midi[0].addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
    output.clear();
    engine.process(output, midi, routing);
    midi[0].clear();
    for (const auto note : { 48, 60, 64, 67 })
        midi[0].addEvent(juce::MidiMessage::noteOff(1, note), 0);
    output.clear();
    engine.process(output, midi, routing);
    midi[0].clear();
    double beforePeak = 0.0;
    for (int block = 0; block < 190; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        if (block >= 180)
            beforePeak = std::max(beforePeak, static_cast<double>(output.getMagnitude(0, 0, blockSize)));
    }
    midi[0].addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    double afterPeak = 0.0;
    for (int block = 0; block < 20; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        midi[0].clear();
        afterPeak = std::max(afterPeak, static_cast<double>(output.getMagnitude(0, 0, blockSize)));
    }
    std::cout << "pedalUp beforePeak=" << beforePeak << " afterPeak=" << afterPeak << '\n';
    if (beforePeak > 0.0 && afterPeak > beforePeak * 2.0)
        return 12;

    // Match SF2's 512-voice ceiling: leave note 60 held while filling the
    // layer to capacity, then release the other pitches. The anchor note must
    // still be audible after their short releases have completed.
    engine.stopAllSounds();
    midi[0].clear();
    midi[0].addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) testVelocity), 0);
    for (int voice = 1; voice < static_cast<int>(SampleEngine::maxVoicesPerLayer); ++voice)
    {
        const auto note = polyphonyTestNotes[(size_t) (voice - 1) % polyphonyTestNotes.size()];
        midi[0].addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) testVelocity), 0);
    }
    output.clear();
    engine.process(output, midi, routing);
    midi[0].clear();
    for (const auto note : polyphonyTestNotes)
        midi[0].addEvent(juce::MidiMessage::noteOff(1, note), 0);
    output.clear();
    engine.process(output, midi, routing);
    midi[0].clear();
    double anchorNotePeak = 0.0;
    for (int block = 0; block < 230; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        if (block >= 210)
            anchorNotePeak = std::max(anchorNotePeak,
                                      static_cast<double>(output.getMagnitude(0, 0, blockSize)));
    }
    std::cout << "512VoicePolyphony anchorPeak=" << anchorNotePeak << '\n';
    if (anchorNotePeak <= 0.0001) return 16;

    engine.stopAllSounds();
    midi[0].addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    std::array<double, 4> heldPeak {};
    for (int block = 0; block < 18 * 48000 / blockSize; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        midi[0].clear();
        const auto second = block * blockSize / 48000.0;
        if (second >= 4 && second < 5) heldPeak[0] = std::max(heldPeak[0], static_cast<double>(output.getMagnitude(0, 0, blockSize)));
        if (second >= 9 && second < 10) heldPeak[1] = std::max(heldPeak[1], static_cast<double>(output.getMagnitude(0, 0, blockSize)));
        if (second >= 14 && second < 15) heldPeak[2] = std::max(heldPeak[2], static_cast<double>(output.getMagnitude(0, 0, blockSize)));
        if (second >= 17.2 && second < 17.3) heldPeak[3] = std::max(heldPeak[3], static_cast<double>(output.getMagnitude(0, 0, blockSize)));
    }
    std::cout << "heldNote peak4s=" << heldPeak[0] << " peak9s=" << heldPeak[1]
              << " peak14s=" << heldPeak[2] << " peak17.2s=" << heldPeak[3] << '\n';
    if (heldPeak[1] <= 0.0001 || heldPeak[2] <= 0.0001 || heldPeak[3] > 0.0001)
        return 13;

    engine.stopAllSounds();
    midi[0].addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    double keyHeldPeak = 0.0;
    for (int block = 0; block < 94; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        midi[0].clear();
        if (block >= 84)
            keyHeldPeak = std::max(keyHeldPeak, static_cast<double>(output.getMagnitude(0, 0, blockSize)));
    }
    midi[0].addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    double keyUpPeak = 0.0;
    double keyUpLatePeak = 0.0;
    for (int block = 0; block < 100; ++block)
    {
        output.clear();
        engine.process(output, midi, routing);
        midi[0].clear();
        const auto peak = static_cast<double>(output.getMagnitude(0, 0, blockSize));
        if (block < 20) keyUpPeak = std::max(keyUpPeak, peak);
        if (block >= 90) keyUpLatePeak = std::max(keyUpLatePeak, peak);
    }
    std::cout << "keyUp heldPeak=" << keyHeldPeak << " firstPeak=" << keyUpPeak
              << " latePeak=" << keyUpLatePeak << '\n';
    if (keyHeldPeak <= 0.0 || keyUpPeak > keyHeldPeak * 2.0
        || keyUpLatePeak > keyHeldPeak * 0.1)
        return 14;
    return 0;
}
}

int main(int argc, char** argv)
{
    if (argc == 2)
        return diagnoseNordPiano(juce::File(juce::String::fromUTF8(argv[1])));

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
