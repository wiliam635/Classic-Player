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

    static float renderedNoteVelocity(int inputVelocity, Sf2Engine::LayerConfig routing)
    {
        auto engine = instrument(48000.0);
        routing.enabled = true;
        juce::AudioBuffer<float> audio(2, 64);
        audio.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60,
            static_cast<float>(inputVelocity) / 127.0f), 0);
        engine->renderLayer(engine->layers[0], audio, midi, routing);
        const auto voice = std::find_if(engine->layers[0].voices.begin(),
            engine->layers[0].voices.end(), [](const auto& candidate) { return candidate.active; });
        require(voice != engine->layers[0].voices.end(), "Velocity test failed to start a piano voice");
        return voice->velocity;
    }

    static void adaptiveVelocity()
    {
        auto routing = config();
        routing.velocityCurve = 3;
        routing.velocityCalibrationEnabled = true;
        routing.velocitySoftInput = 20;
        routing.velocityMidInput = 64;
        routing.velocityHardInput = 108;
        const auto soft = renderedNoteVelocity(20, routing);
        const auto medium = renderedNoteVelocity(64, routing);
        const auto hard = renderedNoteVelocity(108, routing);
        std::cout << "Adaptive velocity outputs: soft=" << soft
                  << ", medium=" << medium << ", hard=" << hard << '\n';
        require(std::abs(soft - 24.0f / 127.0f) < 0.001f,
                "Adaptive velocity missed its soft calibration point");
        require(std::abs(medium - 64.0f / 127.0f) < 0.001f,
                "Adaptive velocity missed its medium calibration point");
        require(std::abs(hard - 112.0f / 127.0f) < 0.001f,
                "Adaptive velocity missed its hard calibration point");

        routing.velocityCalibrationEnabled = false;
        require(std::abs(renderedNoteVelocity(20, routing) - 20.0f / 127.0f) < 0.001f,
                "An uncalibrated adaptive curve must safely fall back to linear");
    }

    static void positiveOnlyTenthsDbTrim()
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        layer.piano.signedTenthsDbTrim = true;
        layer.piano.gainTenthsDb = 80;
        layer.piano.strokes[0].trimDb = 64;
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, config());
        const auto voice = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& candidate) { return candidate.active; });
        require(voice != layer.voices.end(), "Positive-trim piano voice did not start");
        require(std::abs(voice->level - std::pow(10.0f, 1.6f / 20.0f)) < 0.001f,
                "Positive-only 0x464 trim was interpreted as whole decibels");
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
        std::array<std::uint64_t, Sf2Engine::polyphonyPerLayer> agesBeforePedalUp {};
        for (size_t i = 0; i < layer.voices.size(); ++i)
            agesBeforePedalUp[i] = layer.voices[i].age;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        // Check release and identity; a voice may retire once its envelope
        // becomes silent, without triggering a replacement note.
        for (size_t i = 0; i < layer.voices.size(); ++i)
        {
            const auto& voice = layer.voices[i];
            require(voice.released && !voice.waitingForSustain,
                    "Pedal-up left a sustained voice stuck");
            require(voice.age >= agesBeforePedalUp[i],
                    "Pedal-up retriggered a note instead of releasing it");
        }
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
        require(layer.voices.size() == 192, "NPNO polyphony must be 192 voices per layer");
        for (size_t i = 0; i < layer.voices.size(); ++i)
        {
            auto& voice = layer.voices[i];
            voice.active = true;
            voice.level = 1.0f;
            voice.age = i;
        }
        require(&engine->allocateVoice(layer) == &layer.voices.back(),
                "Voice stealing chose the newest held note");
        layer.voices[4].waitingForSustain = true;
        require(&engine->allocateVoice(layer) == &layer.voices[4],
                "Voice stealing cut a held key before a sustained tail");
        layer.voices[7].waitingForSustain = true;
        layer.voices[7].level = 0.1f;
        require(&engine->allocateVoice(layer) == &layer.voices[7],
                "Voice stealing did not prefer the quietest sustained tail");
        layer.voices[5].released = true;
        layer.voices[6].released = true;
        layer.voices[6].releaseGain = 0.001f;
        require(&engine->allocateVoice(layer) == &layer.voices[6],
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
        voice.age = 0;
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        const auto initial = audio.getSample(0, 0);
        layer.sustainDown[0] = true;
        voice.waitingForSustain = true;
        for (const auto seconds : { 3.0, 7.5, 14.5 })
        {
            voice.age = static_cast<std::uint64_t>(seconds * rate);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            const auto remaining = 1.0 - seconds / 15.0;
            require(std::abs(audio.getSample(0, 0) / initial - remaining * remaining) < 0.001,
                    "Piano did not decay progressively under sustain");
        }
        voice.age = static_cast<std::uint64_t>(15.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(!voice.active && audio.getMagnitude(0, 64) == 0.0f,
                "Piano loop survived past 15 seconds");
        layer.sourcePath = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("Organ/synthetic.npno").getFullPathName();
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        voice.age = static_cast<std::uint64_t>(18.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(voice.active && audio.getMagnitude(0, 64) > 0.0f,
                "Piano lifetime incorrectly truncated an organ");
    }

    static void crossfades()
    {
        NordPianoDecoder::Instrument piano;
        piano.channels = 2;
        NordPianoDecoder::Stroke stroke;
        const auto rate = NordPianoDecoder::sampleRate;
        const auto start = rate;
        const auto end = rate * 3;
        stroke.rootNote = 60;
        stroke.loopStartFrame = static_cast<std::uint32_t>(start);
        stroke.loopEndFrame = static_cast<std::uint32_t>(end);
        stroke.interleavedPcm.resize(static_cast<size_t>((end + 4) * 2));
        for (int frame = 0; frame < end + 4; ++frame)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 261.63 * frame / rate;
            const auto offset = frame > end - rate / 2 ? 3000.0 : 0.0;
            const auto value = static_cast<std::int16_t>(std::round(
                8000.0 * std::sin(phase) + 1200.0 * std::sin(phase * 2.0) + offset));
            stroke.interleavedPcm[static_cast<size_t>(frame * 2)] = value;
            stroke.interleavedPcm[static_cast<size_t>(frame * 2 + 1)] = value / 2;
        }
        const auto original = stroke.interleavedPcm;
        piano.strokes.push_back(stroke);
        stroke.bank = 2;
        piano.strokes.push_back(stroke);
        SampleEngine::preparePianoLoops(piano);
        auto& prepared = piano.strokes[0];
        const auto fade = static_cast<int>(prepared.loopStartFrame) - start;
        require(fade > rate / 2 && fade < rate * 6 / 10,
                "Piano crossfade did not use the 550-ms target");
        require(prepared.interleavedPcm.size() == original.size(),
                "Loop preparation allocated a second retained PCM buffer");
        for (int frame = 0; frame < end - fade; ++frame)
            for (int channel = 0; channel < 2; ++channel)
                require(prepared.interleavedPcm[static_cast<size_t>(frame * 2 + channel)]
                            == original[static_cast<size_t>(frame * 2 + channel)],
                        "Loop crossfade changed the attack");
        for (int channel = 0; channel < 2; ++channel)
            require(prepared.interleavedPcm[static_cast<size_t>((end - 1) * 2 + channel)]
                        == original[static_cast<size_t>((start + fade - 1) * 2 + channel)],
                    "Loop crossfade does not lead into the advanced head");
        for (int frame = end - fade; frame < end; ++frame)
        {
            const auto index = static_cast<size_t>(frame * 2);
            require(std::abs(prepared.interleavedPcm[index + 1]
                           - prepared.interleavedPcm[index] * 0.5) <= 2.0,
                    "Loop processing changed the stereo image");
        }
        require(piano.strokes[1].interleavedPcm == original
                    && piano.strokes[1].loopStartFrame == static_cast<std::uint32_t>(start),
                "Loop processing modified release-trigger samples");
        const auto edge = SampleEngine::readPianoInterpolated(prepared, 2, 0, end - 0.25, true);
        for (int frame = end; frame < end + 4; ++frame)
            prepared.interleavedPcm[static_cast<size_t>(frame * 2)] = 32767;
        require(edge == SampleEngine::readPianoInterpolated(prepared, 2, 0, end - 0.25, true),
                "Interpolation leaked PCM from beyond the active loop end");
    }

    static void loopDuringRelease(double rate)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        auto routing = config();
        routing.release = 100.0f;
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        layer.voices[0].position = 4094.0;
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        juce::AudioBuffer<float> audio(2, 128);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(layer.voices[0].active && layer.voices[0].released
                    && audio.getSample(0, 127) > 0.0f,
                "The PCM end truncated a long Release tail");
    }

    static std::vector<float> partitionedDecay(double rate, int block)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        const auto routing = config();
        SampleEngine::preparePianoLoops(layer.piano);
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        std::vector<float> result;
        juce::MidiBuffer midi;
        for (int start = 0; start < 16384; start += block)
        {
            const auto count = std::min(block, 16384 - start);
            juce::AudioBuffer<float> audio(2, count);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            result.insert(result.end(), audio.getReadPointer(0), audio.getReadPointer(0) + count);
        }
        return result;
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
        positiveOnlyTenthsDbTrim();
        crossfades();
        adaptiveVelocity();
        for (const auto rate : { 44100.0, 48000.0 })
        {
            sustainAndPolyphony(rate);
            lifetime(rate);
            loopDuringRelease(rate);
            require(partitionedDecay(rate, 127) == partitionedDecay(rate, 512),
                    "Piano decay/crossfade changed with audio block size");
            voicings(rate);
        }
    }
};

int main()
{
    try
    {
        SampleEngineRegressionTests::run();
        std::cout << "NPNO velocity calibration, polyphony, sustain, lifetime and EQ: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
