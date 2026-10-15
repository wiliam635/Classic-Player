#include "SampleEngine.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

// Synthetic PCM only: no proprietary piano files are needed by CI.
struct SampleEngineRegressionTests
{
    static void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    static std::unique_ptr<SampleEngine> instrument(double rate)
    {
        auto engine = std::make_unique<SampleEngine>();
        engine->prepare(rate, 4096);
        auto& layer = engine->layers[0];
        layer.pianoLoaded = true;
        layer.sourcePath = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("Piano Acustico/synthetic.npno").getFullPathName();
        layer.piano.channels = 1;
        layer.piano.keyMap.fill(60);
        NordPianoDecoder::Stroke stroke;
        stroke.rootNote = 60;
        stroke.loopEndFrame = 4096;
        stroke.interleavedPcm.assign(4096, 4096);
        layer.piano.strokes.push_back(std::move(stroke));
        return engine;
    }

    static Sf2Engine::LayerConfig config()
    {
        Sf2Engine::LayerConfig routing;
        routing.attack = 0.0f;
        routing.release = 30.0f;
        routing.gain = 1.0f;
        routing.midiChannel = 0;
        return routing;
    }

    static int active(const SampleEngine& engine)
    {
        return static_cast<int>(std::count_if(engine.layers[0].voices.begin(),
            engine.layers[0].voices.end(), [](const auto& voice) { return voice.active; }));
    }

    static void sustainAndPolyphony(double rate)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        const auto routing = config();
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        for (int i = 0; i < Sf2Engine::polyphonyPerLayer; ++i)
        {
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            midi.clear();
            require(active(*engine) == i + 1, "Sustain stole a voice below the SF2 ceiling");
        }
        require(layer.voices[0].age > layer.voices.back().age,
                "The first sustained voice was overwritten");
        // A pedal-up must release existing voices, never trigger a new note.
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(active(*engine) == Sf2Engine::polyphonyPerLayer,
                "Pedal-up changed the voice count unexpectedly");
        for (const auto& voice : layer.voices)
            require(voice.released && !voice.waitingForSustain,
                    "Pedal-up left a sustained voice stuck");
        midi.clear();
        for (int i = 0; i < 300; ++i)
        {
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
        }
        require(active(*engine) == 0, "Release tails did not return voices to the pool");
        layer.sustainDown[0] = true;
        engine->stopAllSounds();
        require(!layer.sustainDown[0], "Panic left the pedal latched");
    }

    static void stealing()
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        for (size_t i = 0; i < layer.voices.size(); ++i)
        {
            auto& voice = layer.voices[i];
            voice.active = true;
            voice.level = 1.0f;
            voice.age = i;
        }
        require(&SampleEngine::allocateVoice(layer) == &layer.voices.back(),
                "Voice stealing chose the newest held note");
        layer.voices[4].waitingForSustain = true;
        require(&SampleEngine::allocateVoice(layer) == &layer.voices[4],
                "Voice stealing cut a held key before a sustained tail");
        layer.voices[5].released = true;
        layer.voices[6].released = true;
        layer.voices[6].releaseGain = 0.001f;
        require(&SampleEngine::allocateVoice(layer) == &layer.voices[6],
                "Voice stealing did not prefer the quietest released tail");
    }

    static void lifetime(double rate)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        const auto routing = config();
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> audio(2, 64);
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        auto& voice = layer.voices[0];
        voice.age = static_cast<std::uint64_t>(15.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        const auto at15 = audio.getSample(0, 0);
        voice.age = static_cast<std::uint64_t>(16.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(std::abs(audio.getSample(0, 0) / at15 - 0.5f) < 0.001f,
                "Piano lifetime did not fade smoothly from 15 to 17 seconds");
        voice.age = static_cast<std::uint64_t>(17.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(!voice.active && audio.getMagnitude(0, 64) == 0.0f,
                "Piano loop survived past 17 seconds");
        layer.sourcePath = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("Organ/synthetic.npno").getFullPathName();
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        voice.age = static_cast<std::uint64_t>(18.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(voice.active && audio.getMagnitude(0, 64) > 0.0f,
                "Piano lifetime incorrectly truncated an organ");
    }

    static float response(SampleEngine& engine, int mode, float frequency, double rate)
    {
        auto routing = config();
        routing.pianoTimbre = mode;
        juce::AudioBuffer<float> output(2, 4096);
        for (int pass = 0; pass < 3; ++pass)
        {
            output.clear();
            for (int sample = 0; sample < 4096; ++sample)
            {
                const auto value = 0.05f * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * frequency * (pass * 4096 + sample) / rate));
                for (int channel = 0; channel < 2; ++channel)
                    engine.scratch.setSample(channel, sample, value);
            }
            engine.processEffectsAndMix(engine.layers[0], output, routing);
        }
        require(std::abs(output.getRMSLevel(0, 2048, 2048)
                       - output.getRMSLevel(1, 2048, 2048)) < 1.0e-6f,
                "EQ smoothing produced different left/right gains");
        return output.getRMSLevel(0, 2048, 2048);
    }

    static void voicings(double rate)
    {
        auto engine = instrument(rate);
        // Reuse one engine while holding sustain: changes must affect the
        // current signal, not merely sample selection at the next note-on.
        engine->layers[0].sustainDown[0] = true;
        const auto softHigh = response(*engine, 0, 6000.0f, rate);
        const auto midHigh = response(*engine, 1, 6000.0f, rate);
        const auto brightHigh = response(*engine, 2, 6000.0f, rate);
        require(juce::Decibels::gainToDecibels(brightHigh / softHigh) > 8.0f,
                "Soft/Bright contrast is too small under sustain");
        require(juce::Decibels::gainToDecibels(brightHigh / midHigh) > 4.0f,
                "Mid/Bright contrast is too small under sustain");
        const auto softMid = response(*engine, 0, 1000.0f, rate);
        const auto midMid = response(*engine, 1, 1000.0f, rate);
        require(juce::Decibels::gainToDecibels(midMid / softMid) > 2.0f,
                "Mid does not emphasize the piano body");
        std::cout << "EQ at " << rate << " Hz: Bright/Soft 6 kHz = "
                  << juce::Decibels::gainToDecibels(brightHigh / softHigh) << " dB\n";
    }

    static void run()
    {
        stealing();
        for (const auto rate : { 44100.0, 48000.0 })
        {
            sustainAndPolyphony(rate);
            lifetime(rate);
            voicings(rate);
        }
    }
};

int main()
{
    try
    {
        SampleEngineRegressionTests::run();
        std::cout << "NPNO polyphony, sustain, lifetime and EQ: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
