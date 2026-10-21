#include "NordSampleEngine.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

struct NordSampleRegressionTests
{
    static void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    static std::unique_ptr<NordSampleEngine> fixture(double rate = 48000.0)
    {
        auto engine = std::make_unique<NordSampleEngine>();
        engine->prepare(rate, 512);
        auto& layer = engine->layers[0];
        layer.loaded = true;
        layer.instrument.name = "Synthetic NSMP4";
        layer.instrument.gain.fill(1.0f);
        NordSampleDecoder::Zone zone;
        zone.root = 60;
        zone.low = 0;
        zone.high = 127;
        zone.velocityLow = 1;
        zone.velocityHigh = 127;
        zone.channels = 2;
        zone.gain = 0.8f;
        zone.loopStart = 256;
        zone.loopEnd = 4096;
        zone.pcm.resize(4096 * 2);
        for (int i = 0; i < 4096; ++i)
        {
            const auto sample = 0.35f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * i
                                                  / (float) NordSampleDecoder::sampleRate);
            zone.pcm[(size_t) i * 2] = sample;
            zone.pcm[(size_t) i * 2 + 1] = sample * 0.75f;
        }
        layer.instrument.zones.push_back(std::move(zone));
        return engine;
    }

    static Sf2Engine::LayerConfig routing()
    {
        Sf2Engine::LayerConfig config;
        config.enabled = true;
        config.midiChannel = 0;
        config.attack = 0.0f;
        config.release = 30.0f;
        config.gain = 1.0f;
        return config;
    }

    static int activeVoices(const NordSampleEngine& engine)
    {
        return (int) std::count_if(engine.layers[0].voices.begin(), engine.layers[0].voices.end(),
                                   [](const auto& voice) { return voice.active; });
    }

    static void render(NordSampleEngine& engine, juce::MidiBuffer midi,
                       const Sf2Engine::LayerConfig& config, float& rms)
    {
        juce::AudioBuffer<float> audio(2, 128);
        audio.clear();
        std::array<juce::MidiBuffer, NordSampleEngine::layerCount> routed;
        std::array<Sf2Engine::LayerConfig, NordSampleEngine::layerCount> routes {};
        routes[0] = config;
        engine.process(audio, midi, routed, routes);
        rms = (audio.getRMSLevel(0, 0, audio.getNumSamples())
             + audio.getRMSLevel(1, 0, audio.getNumSamples())) * 0.5f;
    }

    static void audioAndSustain()
    {
        auto engine = fixture();
        auto config = routing();
        float rms = 0.0f;
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        render(*engine, midi, config, rms);
        require(rms > 0.05f && activeVoices(*engine) == 1, "NSMP4 note did not render audible PCM");

        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        render(*engine, midi, config, rms);
        auto voice = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                                  [](const auto& candidate) { return candidate.active; });
        require(voice != engine->layers[0].voices.end() && voice->waitingForSustain
                    && !voice->released && rms > 0.02f,
                "Sustain pedal did not hold the released NSMP4 note");

        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        render(*engine, midi, config, rms);
        voice = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                             [](const auto& candidate) { return candidate.active; });
        require(voice != engine->layers[0].voices.end() && voice->released
                    && !voice->waitingForSustain,
                "Sustain pedal-up failed to release the NSMP4 note");

        // Releasing the pedal while a key remains physically down must not
        // attenuate it; pedal-up affects only notes already key-released.
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        render(*engine, midi, config, rms);
        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        render(*engine, midi, config, rms);
        voice = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                             [](const auto& candidate) { return candidate.active && candidate.keyDown; });
        require(voice != engine->layers[0].voices.end() && !voice->released && rms > 0.02f,
                "Pedal-up changed a note whose key is still held");
    }

    static void voiceCeiling()
    {
        auto engine = fixture();
        auto config = routing();
        juce::MidiBuffer midi;
        for (int i = 0; i < NordSampleEngine::voicesPerLayer + 16; ++i)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.5f), 0);
        float rms = 0.0f;
        render(*engine, midi, config, rms);
        require(activeVoices(*engine) == config.polyphony,
                "NSMP4 exceeded the selected per-layer voice ceiling");
        require(rms > 0.0f, "Voice stealing silenced all active NSMP4 notes");
        engine = fixture();
        config.polyphony = 64;
        render(*engine, midi, config, rms);
        require(activeVoices(*engine) == 64, "NSMP4 ignored the 64-voice setting");
        engine = fixture();
        config.polyphony = 256;
        render(*engine, midi, config, rms);
        require(activeVoices(*engine) == 256, "NSMP4 ignored the 256-voice setting");
    }

    static void nordEffects()
    {
        NordLayerEffects effects;
        effects.prepare(48000.0);
        juce::AudioBuffer<float> audio(2, 12100);
        audio.clear();
        audio.setSample(0, 0, 1.0f);
        audio.setSample(1, 0, 1.0f);
        NordEffectSettings settings;
        effects.process(audio, settings);
        require(audio.getSample(0, 0) == 1.0f && audio.getSample(0, 12000) == 0.0f,
                "Nord effects changed a bypassed layer");
        effects.reset();
        audio.clear();
        audio.setSample(0, 0, 1.0f);
        audio.setSample(1, 0, 1.0f);
        settings.delayMix = 100.0f;
        settings.delayFeedback = 0.0f;
        settings.delayBpm = 240.0f;
        effects.process(audio, settings);
        require(audio.getSample(0, 12000) > 0.9f && audio.getSample(1, 12000) > 0.9f,
                "Nord delay did not follow the manually selected BPM");
        effects.reset();
        settings.delayMix = 0.0f;
        settings.tremoloMix = 100.0f;
        audio.clear();
        audio.setSample(0, 0, 1.0f);
        effects.process(audio, settings);
        require(audio.getSample(0, 0) > 0.4f && audio.getSample(0, 0) < 0.6f,
                "Nord tremolo was not applied");
    }

    static void midiSampleOffsetsAndLargeBlocks()
    {
        auto engine = fixture();
        auto config = routing();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 64);
        juce::AudioBuffer<float> audio(2, 2048);
        audio.clear();
        std::array<juce::MidiBuffer, NordSampleEngine::layerCount> routed;
        std::array<Sf2Engine::LayerConfig, NordSampleEngine::layerCount> routes {};
        routes[0] = config;
        engine->process(audio, midi, routed, routes);

        require(audio.getRMSLevel(0, 0, 64) < 1.0e-7f,
                "NSMP4 note began before its MIDI sample offset");
        require(audio.getRMSLevel(0, 64, audio.getNumSamples() - 64) > 0.05f,
                "NSMP4 failed to render an event in a block larger than prepare() size");
    }

    static void realFiles(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            NordSampleDecoder::Instrument instrument;
            const auto file = juce::File(argv[i]);
            const auto result = NordSampleDecoder::decode(file, instrument);
            require(result.wasOk(), result.getErrorMessage().toRawUTF8());
            require(!instrument.zones.empty(), "NSMP4 real-file decode returned no key zones");
            size_t samples = 0;
            for (const auto& zone : instrument.zones)
            {
                require(zone.frames() > 0 && zone.channels >= 1 && zone.channels <= 2,
                        "NSMP4 real-file zone has invalid PCM dimensions");
                samples += zone.pcm.size();
                require(std::all_of(zone.pcm.begin(), zone.pcm.end(),
                                    [](float sample) { return std::isfinite(sample); }),
                        "NSMP4 real-file decode produced NaN or infinity");
            }
            std::cout << file.getFileName() << ": " << instrument.zones.size()
                      << " zones, " << samples << " decoded samples\n";
        }
    }

    static void run(int argc, char** argv)
    {
        audioAndSustain();
        nordEffects();
        voiceCeiling();
        midiSampleOffsetsAndLargeBlocks();
        realFiles(argc, argv);
    }
};

int main(int argc, char** argv)
{
    try
    {
        NordSampleRegressionTests::run(argc, argv);
        std::cout << "NSMP4 decode, playback, sustain and 192-voice ceiling: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
