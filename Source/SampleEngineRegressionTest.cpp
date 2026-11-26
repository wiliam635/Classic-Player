#include "SampleEngine.h"
#include "FastLoadCache.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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
        layer.finitePiano = true;
        layer.sourcePath = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("Piano Acustico/synthetic.npno").getFullPathName();
        layer.piano = std::make_shared<NordPianoDecoder::Instrument>();
        layer.piano->channels = 1;
        layer.piano->keyMap.fill(60);
        NordPianoDecoder::Stroke stroke;
        stroke.rootNote = 60;
        stroke.loopEndFrame = 4096;
        stroke.interleavedPcm.assign(4096, 4096);
        layer.piano->strokes.push_back(std::move(stroke));
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

    static void decodedPianoCacheSharesPcm()
    {
        // Heap-allocated: the engine holds every layer's voices and is too large for a Windows 1 MB stack.
        auto engineHolder = std::make_unique<SampleEngine>();
        auto& engine = *engineHolder;
        const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("classic-piano-cache-test.npno");
        auto decoded = std::make_shared<NordPianoDecoder::Instrument>();
        decoded->channels = 1;
        const auto cached = engine.cachePiano(source, decoded);
        require(cached == decoded && engine.findCachedPiano(source) == decoded,
                "NPNO preload cache copied instead of sharing decoded PCM");
    }

    // A piano too big for RAM is written to the fast-load cache one stroke at a time; loadPiano must then read
    // back exactly what was streamed, including the header fields that are only known after the last stroke.
    static void streamedPianoCacheRoundTrip()
    {
        const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("classic-streamed-piano-test.npno");
        source.replaceWithText("streamed piano test " + juce::String(juce::Time::currentTimeMillis()));
        const auto cache = FastLoadCache::cacheFile(FastLoadCache::sourceKey(source, FastLoadCache::Kind::piano));
        cache.deleteFile();

        NordPianoDecoder::Instrument expected;
        expected.name = "Streamed Test";
        expected.voicing = "Soft";
        expected.streamVersion = 0x464;
        expected.channels = 2;
        expected.gainTenthsDb = -7;
        expected.signedTenthsDbTrim = true;
        expected.velocityScale = 37;
        expected.keyMap.fill(0xff);
        expected.keyMap[60] = 60;
        expected.keyMap[61] = 60;
        expected.fineTune[60] = 3;
        for (int i = 0; i < 3; ++i)
        {
            NordPianoDecoder::Stroke stroke;
            stroke.rootNote = 60;
            stroke.bank = (std::uint8_t) (i == 2 ? 1 : 0);
            stroke.velocityLayer = (std::uint8_t) (i * 5);
            stroke.trimDb = (std::uint16_t) (10 + i);
            stroke.loopStartFrame = (std::uint32_t) (100 + i);
            stroke.loopEndFrame = (std::uint32_t) (900 + i);
            for (int n = 0; n < 2000 + i * 7; ++n)
                stroke.interleavedPcm.push_back((std::int16_t) ((n * (i + 3)) % 20000 - 10000));
            expected.strokes.push_back(std::move(stroke));
        }

        {
            int prepared = 0;
            FastLoadCache::PianoStreamWriter writer(source, [&prepared](NordPianoDecoder::Stroke&, int channels)
            {
                require(channels == 2, "Streamed piano writer lost the channel count");
                ++prepared;
            });
            require(writer.available(1024), "Streamed piano cache reported unavailable");
            // begin() sees the header without the two late fields, exactly as the decoder calls it.
            auto header = expected;
            header.signedTenthsDbTrim = false;
            header.velocityScale = 31;
            require(writer.begin(header, (std::uint32_t) expected.strokes.size()).wasOk(), "Streamed piano begin failed");
            for (auto stroke : expected.strokes)
                require(writer.stroke(stroke).wasOk(), "Streamed piano stroke write failed");
            require(prepared == 3, "Streamed piano writer skipped the per-stroke preparation");
            require(writer.end(expected).wasOk() && writer.completed(), "Streamed piano end failed");
        }
        require(cache.existsAsFile(), "Streamed piano cache file was not published");

        NordPianoDecoder::Instrument loaded;
        require(FastLoadCache::loadPiano(source, loaded, std::numeric_limits<std::uint64_t>::max()),
                "Streamed piano cache could not be loaded");
        require(loaded.name == expected.name && loaded.voicing == expected.voicing
                && loaded.streamVersion == expected.streamVersion && loaded.channels == expected.channels
                && loaded.gainTenthsDb == expected.gainTenthsDb && loaded.keyMap == expected.keyMap
                && loaded.fineTune == expected.fineTune,
                "Streamed piano header changed on the round trip");
        require(loaded.signedTenthsDbTrim == expected.signedTenthsDbTrim && loaded.velocityScale == expected.velocityScale,
                "Streamed piano late header fields were not patched in");
        require(loaded.strokes.size() == expected.strokes.size(), "Streamed piano lost strokes");
        for (size_t i = 0; i < expected.strokes.size(); ++i)
        {
            const auto& a = expected.strokes[i];
            const auto& b = loaded.strokes[i];
            require(a.rootNote == b.rootNote && a.bank == b.bank && a.velocityLayer == b.velocityLayer
                    && a.trimDb == b.trimDb && a.loopStartFrame == b.loopStartFrame
                    && a.loopEndFrame == b.loopEndFrame && a.interleavedPcm == b.interleavedPcm,
                    "Streamed piano stroke changed on the round trip");
        }
        loaded = {};

        // A writer that is dropped before end() must publish nothing and leave no temporary file behind.
        cache.deleteFile();
        {
            FastLoadCache::PianoStreamWriter writer(source, nullptr);
            auto header = expected;
            require(writer.begin(header, 3).wasOk(), "Aborted piano begin failed");
            auto stroke = expected.strokes[0];
            require(writer.stroke(stroke).wasOk(), "Aborted piano stroke failed");
        }
        require(!cache.existsAsFile(), "An incomplete streamed piano cache was published");
        require(!cache.getParentDirectory().getChildFile(cache.getFileNameWithoutExtension() + ".tmp").existsAsFile(),
                "An incomplete streamed piano left a temporary file");
        source.deleteFile();
    }

    static bool hasCachedPiano(SampleEngine& engine, const juce::File& file)
    {
        return engine.findCachedPiano(file) != nullptr;
    }

    static void midiTiming(double rate)
    {
        auto routing = config();
        routing.release = 0.0f;
        auto whole = instrument(rate);
        juce::AudioBuffer<float> wholeAudio(2, 64);
        wholeAudio.clear();
        juce::MidiBuffer events;
        events.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 16);
        events.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 32);
        events.addEvent(juce::MidiMessage::noteOff(1, 60), 40);
        events.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 56);
        whole->renderLayer(whole->layers[0], wholeAudio, events, routing);

        for (int sample = 0; sample < 16; ++sample)
            require(wholeAudio.getSample(0, sample) == 0.0f,
                    "NPNO note started before its MIDI timestamp");
        require(wholeAudio.getSample(0, 16) > 0.0f,
                "NPNO note missed its MIDI timestamp");
        require(wholeAudio.getSample(0, 48) > 0.0f,
                "Pedal did not hold the note after key-up");

        auto split = instrument(rate);
        juce::AudioBuffer<float> splitAudio(2, 64);
        splitAudio.clear();
        for (int start = 0; start < 64; start += 8)
        {
            juce::AudioBuffer<float> segment(splitAudio.getArrayOfWritePointers(), 2, start, 8);
            juce::MidiBuffer segmentEvents;
            for (const auto metadata : events)
                if (metadata.samplePosition >= start && metadata.samplePosition < start + 8)
                    segmentEvents.addEvent(metadata.getMessage(), metadata.samplePosition - start);
            split->renderLayer(split->layers[0], segment, segmentEvents, routing);
        }
        for (int sample = 0; sample < 64; ++sample)
            require(std::abs(wholeAudio.getSample(0, sample)
                             - splitAudio.getSample(0, sample)) < 0.00001f,
                    "NPNO playback changed with MIDI block boundaries");

        auto earlyKeyUp = instrument(rate);
        juce::AudioBuffer<float> earlyAudio(2, 64);
        earlyAudio.clear();
        juce::MidiBuffer earlyEvents;
        earlyEvents.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        earlyEvents.addEvent(juce::MidiMessage::noteOff(1, 60), 16);
        earlyEvents.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 32);
        earlyKeyUp->renderLayer(earlyKeyUp->layers[0], earlyAudio, earlyEvents, routing);
        require(earlyKeyUp->layers[0].voices[0].released
                    && !earlyKeyUp->layers[0].voices[0].waitingForSustain,
                "A later pedal press incorrectly sustained an earlier note-off");
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
        layer.piano->signedTenthsDbTrim = true;
        layer.piano->gainTenthsDb = 80;
        layer.piano->strokes[0].trimDb = 64;
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, config());
        const auto voice = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& candidate) { return candidate.active; });
        require(voice != layer.voices.end(), "Positive-trim piano voice did not start");
        require(std::abs(voice->level - std::pow(10.0f, 1.6f / 20.0f)) < 0.001f,
                "Positive-only 0x464 trim was interpreted as whole decibels");
    }

    static void invertedSustainPedal(double rate)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        const auto routing = config();
        struct Restore { ~Restore() { globalSustainInverted().store(false); } } restore;
        globalSustainInverted().store(true);
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        // Inverted pedal: a low CC64 value means "pressed", so the note-off must be held.
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(layer.sustainDown[0], "Inverted pedal: CC64=0 should hold the sustain");
        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(!layer.sustainDown[0], "Inverted pedal: CC64=127 should release the sustain");
    }

    static void globalTransposeShiftsNotes(double rate)
    {
        struct Restore { ~Restore() { globalTranspose().store(0); } } restore;
        require(noteShift(1) == 12, "Octave shift changed without transpose");
        globalTranspose().store(-5);
        require(noteShift(1) == 7 && noteShift(-1) == -17, "Transpose is not added to the layer octave");
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        const auto routing = config();
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        globalTranspose().store(2);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        const auto voice = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& candidate) { return candidate.active; });
        require(voice != layer.voices.end(), "Transposed note did not start a voice");
        require(voice->note == 62, "Transpose +2 did not play note 62 for key 60");
    }

    static void mappedFaderIsNotChannelVolume(double rate)
    {
        auto engine = instrument(rate);
        auto& layer = engine->layers[0];
        auto routing = config();
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, 64), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(layer.cc7[0] < 0.3f, "CC7 should act as channel volume while nothing is mapped to it");
        // A fader mapped to a layer/master control sends CC7 too: it must not also lower the NPNO channel volume.
        routing.ignoreChannelVolumeCC7 = true;
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(layer.cc7[0] == 1.0f, "A mapped CC7 must not also change the NPNO channel volume");
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
        require(layer.voices[0].age > layer.voices[(size_t) routing.polyphony - 1].age,
                "The first sustained voice was overwritten");
        // A pedal-up must release existing voices, never trigger a new note.
        std::array<std::uint64_t, Sf2Engine::polyphonyPerLayer> agesBeforePedalUp {};
        for (size_t i = 0; i < agesBeforePedalUp.size(); ++i)
            agesBeforePedalUp[i] = layer.voices[i].age;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        // Check release and identity; a voice may retire once its envelope
        // becomes silent, without triggering a replacement note.
        for (size_t i = 0; i < agesBeforePedalUp.size(); ++i)
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

    // Piano model fitted to a reference player: the volume follows velocity^exponent (soft notes far
    // quieter than the old linear law) and, once the sustain loop is running, each key decays at its own rate.
    static void pianoDynamicsAndNaturalDecay()
    {
        const auto peakAt = [](float exponent, int velocity)
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            routing.pianoDynamicsExponent = exponent;
            juce::AudioBuffer<float> audio(2, 4096);
            audio.clear(); // renderLayer adds into the buffer
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(velocity)), 0);
            engine->renderLayer(layer, audio, midi, routing);
            return audio.getMagnitude(0, 0, 4096);
        };
        const auto db = [](float value) { return 20.0f * std::log10(std::max(value, 1.0e-9f)); };
        const auto linearDrop = db(peakAt(1.0f, 64)) - db(peakAt(1.0f, 127));
        require(std::abs(linearDrop - (-5.95f)) < 0.6f, "Default piano law is no longer linear in velocity");
        const auto modelDrop = db(peakAt(2.2f, 64)) - db(peakAt(2.2f, 127));
        require(std::abs(modelDrop - (-13.1f)) < 0.7f, "Piano dynamics exponent did not widen the soft/hard range");
        const auto softDrop = db(peakAt(2.2f, 20)) - db(peakAt(2.2f, 127));
        require(softDrop < -30.0f && softDrop > -38.0f, "Soft notes are not in the measured range below the loud ones");

        const auto decayOver = [](int note, bool natural)
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            routing.pianoNaturalDecay = natural;
            routing.pianoDecaySeconds = 15.0f;
            juce::AudioBuffer<float> audio(2, 4096);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
            float early = 0.0f, late = 0.0f;
            for (int block = 0; block < 24; ++block)
            {
                audio.clear();
                engine->renderLayer(layer, audio, midi, routing);
                midi.clear();
                if (block == 11) early = audio.getRMSLevel(0, 0, 4096);
                if (block == 23) late = audio.getRMSLevel(0, 0, 4096);
            }
            return 20.0f * std::log10(std::max(late, 1.0e-9f) / std::max(early, 1.0e-9f));
        };
        // 12 blocks of 4096 frames at 48 kHz = 1.024 s between the two measurements.
        const auto c3 = decayOver(48, true), c5 = decayOver(72, true), old = decayOver(48, false);
        // Between 1 s and 2 s: the per-key loop slope plus the 2.5 dB/s early decay of the first 4 s.
        require(std::abs(c3 - (-(3.3f + 2.5f) * 1.024f)) < 0.4f, "Natural decay at C3 is not ~5.8 dB/s");
        require(std::abs(c5 - (-(5.46f + 2.5f) * 1.024f)) < 0.4f, "Natural decay at C5 is not ~8 dB/s");
        require(c5 < c3 - 1.5f, "Higher keys must decay faster than lower keys");
        require(old > -2.0f, "The old tail law changed for pianos without the natural decay option");
        std::cout << "Piano dynamics law and per-key natural decay passed\n";
    }

    // Mapped NPNO pages are kept resident ahead of playing voices by a background thread. This checks the
    // publish/touch path on a real mapping (position, loop wrap, instrument swap) without crashing.
    static void prefetcherHandlesMappedSamples()
    {
        auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("classic-prefetch-test.bin");
        file.deleteFile();
        {
            juce::FileOutputStream out(file);
            require(out.openedOk(), "Prefetch test file could not be created");
            std::vector<std::int16_t> pcm(300000, 1000);
            out.write(pcm.data(), pcm.size() * sizeof(std::int16_t));
        }
        auto map = std::make_shared<juce::MemoryMappedFile>(file, juce::MemoryMappedFile::readOnly);
        require(map->getData() != nullptr, "Prefetch test mapping failed");
        auto engine = std::make_unique<SampleEngine>();
        engine->prepare(48000.0, 512);
        auto instrument = std::make_shared<NordPianoDecoder::Instrument>();
        instrument->channels = 1;
        NordPianoDecoder::Stroke stroke;
        stroke.loopStartFrame = 1000;
        stroke.loopEndFrame = 299000;
        stroke.interleavedPcm.mapFrom(map, 0, 300000);
        instrument->strokes.push_back(std::move(stroke));
        engine->prefetcher.setInstrument(0, instrument);
        PianoPrefetcher::Voice voice;
        voice.stroke = 0;
        voice.firstFrame = 295000.0;   // close to the loop end: the range must wrap
        voice.framesAhead = 60000.0;
        voice.looped = true;
        voice.loopStart = 1000.0;
        voice.loopEnd = 299000.0;
        for (int i = 0; i < 20; ++i)
        {
            voice.firstFrame += 700.0;
            engine->prefetcher.publish(0, &voice, 1);
            juce::Thread::sleep(5);
        }
        engine->prefetcher.setInstrument(0, nullptr);   // swap while the thread may be touching pages
        engine->prefetcher.publish(0, &voice, 0);
        instrument.reset();
        engine.reset();                                  // stops the thread
        map.reset();
        file.deleteFile();
        std::cout << "NPNO prefetch of mapped pages passed\n";
    }

    static void pedalResonanceIsOptional()
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        auto pedalSample = layer.piano->strokes[0];
        pedalSample.bank = 1;
        std::fill(pedalSample.interleavedPcm.begin(), pedalSample.interleavedPcm.end(), 8192);
        layer.piano->strokes.push_back(std::move(pedalSample));

        auto routing = config();
        routing.enabled = true;
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        engine->renderLayer(layer, audio, midi, routing);
        auto voice = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& candidate) { return candidate.active; });
        require(voice != layer.voices.end() && voice->sampleIndex == 0,
                "Pedal-down selected the separate NPNO pedal sample");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        engine->renderLayer(layer, audio, midi, routing);
        require(voice->active && voice->waitingForSustain && !voice->released,
                "Sustain pedal did not continue holding the original sample");

        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
        engine->renderLayer(layer, audio, midi, routing);
        require(voice->released && !voice->waitingForSustain,
                "Pedal-up did not release the original sustained sample");

        engine->stopAllSounds();
        routing.pianoPedalResonance = true;
        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        voice = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& candidate) { return candidate.active; });
        require(voice != layer.voices.end() && voice->sampleIndex == 1,
                "Enabled pedal reflections did not select the NPNO pedal-down sample");
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 32);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(voice->released && !voice->waitingForSustain,
                "Pedal reflections stayed latched after sustain was released");

        auto noPedalBank = instrument(48000.0);
        noPedalBank->layers[0].sustainDown[0] = true;
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        audio.clear();
        noPedalBank->renderLayer(noPedalBank->layers[0], audio, midi, routing);
        require(noPedalBank->layers[0].voices[0].sampleIndex == 0,
                "Missing pedal-down bank did not fall back to the original piano sample");
    }

    static void uprightReleaseReflections()
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        auto releaseStroke = layer.piano->strokes[0];
        releaseStroke.bank = 2;
        layer.piano->strokes.push_back(std::move(releaseStroke));
        auto routing = config();
        routing.pianoPedalResonance = true;
        routing.pianoReleaseSamplesEnabled = true;
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 80), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 32);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(std::any_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active && voice.pianoRelease; }),
            "Upright release sample was not triggered on key-up");

        engine->stopAllSounds();
        routing.pianoPedalResonance = false;
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(std::none_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active && voice.pianoRelease; }),
            "Disabled reflections unexpectedly triggered the release sample");
    }

    // Key-off reflections play at half the level of the main note under either velocity law. The piano model's
    // velocity^2.2 once applied to the already-halved velocity and made them about 7 dB quieter than intended.
    static void reflectionLevelFollowsMainNote()
    {
        for (const auto exponent : { 1.0f, 2.2f })
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto releaseStroke = layer.piano->strokes[0];
            releaseStroke.bank = 2;
            layer.piano->strokes.push_back(std::move(releaseStroke));
            auto routing = config();
            routing.enabled = true;
            routing.pianoPedalResonance = true;
            routing.pianoReleaseSamplesEnabled = true;
            routing.pianoDynamicsExponent = exponent;
            juce::AudioBuffer<float> audio(2, 64);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 80), 0);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 32);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            float mainLevel = 0.0f, reflectionLevel = 0.0f;
            for (const auto& voice : layer.voices)
            {
                if (voice.sampleIndex < 0) continue;
                if (voice.pianoRelease) reflectionLevel = voice.level; else mainLevel = voice.level;
            }
            require(mainLevel > 0.0f && reflectionLevel > 0.0f, "Reflection test did not start both voices");
            require(std::abs(reflectionLevel / mainLevel - 0.5f) < 0.02f,
                    "Key-off reflection is not at half the level of the main note");
        }
        std::cout << "Key-off reflection level follows the main note passed\n";
    }

    // A key struck again under the sustain pedal fades the previous (pedal-held) voice of the same note, but only
    // when the piano model is on; the new note keeps playing and other notes are untouched.
    static void restrikeUnderSustain()
    {
        for (const auto natural : { true, false })
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            routing.pianoNaturalDecay = natural;
            juce::AudioBuffer<float> audio(2, 256);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 1);
            midi.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 90), 2);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            midi.clear();
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 0);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            int held = 0, fading = 0, waiting = 0;
            for (const auto& voice : layer.voices)
            {
                if (!voice.active || voice.pianoRelease) continue;
                if (voice.released) ++fading; else if (voice.waitingForSustain) ++waiting; else ++held;
            }
            if (natural)
                require(held == 2 && fading == 1 && waiting == 0,
                        "Re-striking under sustain did not fade the previous voice of the same note only");
            else
                require(held == 2 && fading == 0 && waiting == 1,
                        "Without the piano model a re-struck note must keep stacking (saved performances)");
        }
        std::cout << "Re-strike under sustain passed\n";
    }

    static void stageFourPianoControls()
    {
        auto routing = config();
        routing.enabled = true;
        routing.release = 50.0f;
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.2f), 0);

        auto natural = instrument(48000.0);
        natural->renderLayer(natural->layers[0], audio, midi, routing);
        const auto naturalLevel = natural->layers[0].voices[0].level;
        const auto naturalVelocity = natural->layers[0].voices[0].velocity;

        routing.pianoDynamicCompression = 3;
        auto compressed = instrument(48000.0);
        audio.clear();
        compressed->renderLayer(compressed->layers[0], audio, midi, routing);
        require(compressed->layers[0].voices[0].level > naturalLevel
                    && compressed->layers[0].voices[0].velocity == naturalVelocity,
                "Dynamic compression changed the selected sample instead of lifting quiet level");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        audio.clear();
        natural->renderLayer(natural->layers[0], audio, midi, routing);
        const auto normalRelease = natural->layers[0].voices[0].releaseCoefficient;
        routing.pianoSoftRelease = true;
        audio.clear();
        compressed->renderLayer(compressed->layers[0], audio, midi, routing);
        require(compressed->layers[0].voices[0].releaseCoefficient > normalRelease,
                "Soft release did not extend the NPNO key-up envelope");
    }

    static void stealing()
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        require(layer.voices.size() == Sf2Engine::maxPolyphonyPerLayer,
                "NPNO pool must support 256 voices per layer");
        for (size_t i = 0; i < layer.voices.size(); ++i)
        {
            auto& voice = layer.voices[i];
            voice.active = true;
            voice.level = 1.0f;
            voice.age = i;
        }
        require(&engine->allocateVoice(layer, 64) == &layer.voices[63],
                "NPNO ignored the 64-voice ceiling");
        require(&engine->allocateVoice(layer, 256) == &layer.voices.back(),
                "Voice stealing chose the newest held note");
        layer.voices[4].waitingForSustain = true;
        require(&engine->allocateVoice(layer, 256) == &layer.voices[4],
                "Voice stealing cut a held key before a sustained tail");
        layer.voices[7].waitingForSustain = true;
        layer.voices[7].level = 0.1f;
        require(&engine->allocateVoice(layer, 256) == &layer.voices[7],
                "Voice stealing did not prefer the quietest sustained tail");
        layer.voices[5].released = true;
        layer.voices[6].released = true;
        layer.voices[6].releaseGain = 0.001f;
        require(&engine->allocateVoice(layer, 256) == &layer.voices[6],
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
        layer.finitePiano = false;
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        voice.age = static_cast<std::uint64_t>(18.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(voice.active && audio.getMagnitude(0, 64) > 0.0f,
                "Piano lifetime incorrectly truncated an organ");
    }

    static void shortenedDecay(double rate)
    {
        auto engine = instrument(rate);
        auto routing = config();
        routing.pianoDecaySeconds = 4.0f;
        auto& layer = engine->layers[0];
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        auto& voice = layer.voices[0];
        require(voice.pianoDecaySeconds == 4.0f,
                "NPNO voice did not capture its selected tail length");

        // Editing the layer while a note rings must not abruptly change that note.
        routing.pianoDecaySeconds = 15.0f;
        voice.age = static_cast<std::uint64_t>(2.0 * rate);
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(voice.active && audio.getSample(0, 0) > 0.0f,
                "Shortened sustain stopped before its chosen lifetime");
        require(std::abs(audio.getSample(0, 0) - 0.25f * 4096.0f / 32768.0f) < 0.001f,
                "Shortened NPNO tail did not follow its own decay envelope");
        voice.age = static_cast<std::uint64_t>(4.0 * rate);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        require(!voice.active && audio.getMagnitude(0, 64) == 0.0f,
                "Shortened NPNO tail kept its loop alive too long");

        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        require(layer.voices[0].pianoDecaySeconds == 15.0f,
                "New NPNO note did not adopt the changed tail length");
    }

    static void adsrSustain(double rate)
    {
        auto engine = instrument(rate);
        auto routing = config();
        routing.decayMs = 10.0f;
        routing.sustainLevel = 25.0f;
        auto& layer = engine->layers[0];
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        auto& voice = layer.voices[0];
        juce::AudioBuffer<float> audio(2, 128);
        juce::MidiBuffer midi;
        for (int block = 0; block < 8; ++block)
        {
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
        }
        require(std::abs(voice.decayGain - 0.25f) < 0.001f,
                "NPNO ADSR did not reach the selected sustain level");
        const auto sustainedLevel = audio.getMagnitude(0, 128);
        require(sustainedLevel > 0.0f && sustainedLevel < 0.04f,
                "NPNO ADSR did not attenuate the rendered audio");
        engine->startPianoVoice(layer, 0, 60, 1, 1.0f, routing);
        const auto fresh = std::find_if(layer.voices.begin(), layer.voices.end(),
            [](const auto& item) { return item.active && item.age == 0; });
        require(fresh != layer.voices.end() && fresh->decayGain == 1.0f,
                "NPNO ADSR was not reset for a new note");
    }

    static void customVelocity(double rate)
    {
        auto engine = instrument(rate);
        auto routing = config();
        routing.velocityCurve = 4;
        routing.velocityTable = shapedVelocityTable(1, 127, 0.0f);
        routing.velocityTable[64] = 100;
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), 0);
        juce::AudioBuffer<float> audio(2, 64);
        audio.clear();
        engine->renderLayer(engine->layers[0], audio, midi, routing);
        const auto& voice = engine->layers[0].voices[0];
        require(voice.active && std::abs(voice.velocity - 100.0f / 127.0f) < 0.001f,
                "NPNO did not use the custom velocity curve for sample selection");
    }

    static void crossfades()
    {
        NordPianoDecoder::Instrument piano;
        piano.channels = 2;
        NordPianoDecoder::Stroke stroke;
        const auto rate = NordPianoDecoder::sampleRate;
        const auto start = rate;
        const auto end = rate * 3;
        stroke.rootNote = 57; // A3: protect the 220-Hz body while smoothing the loop.
        stroke.loopStartFrame = static_cast<std::uint32_t>(start);
        stroke.loopEndFrame = static_cast<std::uint32_t>(end);
        stroke.interleavedPcm.resize(static_cast<size_t>((end + 4) * 2));
        for (int frame = 0; frame < end + 4; ++frame)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 220.0 * frame / rate;
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
        require(fade > rate * 45 / 100 && fade < rate * 65 / 100,
                "Piano crossfade escaped its adaptive 550-ms target");
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
        double processedEnergy = 0.0, plainEnergy = 0.0;
        for (int frame = fade / 4; frame < fade * 3 / 4; ++frame)
        {
            const auto weight = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::pi * frame / (fade - 1));
            const auto tail = original[static_cast<size_t>((end - fade + frame) * 2)];
            const auto head = original[static_cast<size_t>((start + frame) * 2)];
            const auto plain = (1.0 - weight) * tail + weight * head;
            const auto processed = prepared.interleavedPcm[
                static_cast<size_t>((end - fade + frame) * 2)];
            plainEnergy += plain * plain;
            processedEnergy += processed * processed;
        }
        require(processedEnergy >= plainEnergy * 0.999,
                "Loop crossfade attenuated the piano body");
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
        SampleEngine::preparePianoLoops(*layer.piano);
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
        const auto neutralHigh = response(*engine, -1, 6000.0f, rate);
        require(std::abs(neutralHigh - 0.05f / std::sqrt(2.0f)) < 0.003f,
                "OFF must return the NPNO piano EQ to a neutral response");
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

    // Half pedal: a CC64 value below the switch point lengthens the release of a released key in proportion to the
    // depth; the usual on/off pedal (0 or 127) must behave exactly as before.
    static float coefficientAfterKeyUp(int pedalBefore, int pedalAfter = -1)
    {
        auto engine = instrument(44100.0);
        auto& layer = engine->layers[0];
        const auto routing = config();
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        if (pedalBefore >= 0) midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, pedalBefore), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 1);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 2);
        if (pedalAfter >= 0) midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, pedalAfter), 3);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        for (const auto& voice : layer.voices)
            if (voice.active && voice.released) return voice.releaseCoefficient;
        require(false, "Half pedal test: no released voice");
        return 0.0f;
    }

    static void halfPedalRelease()
    {
        const auto up = coefficientAfterKeyUp(-1);
        const auto zero = coefficientAfterKeyUp(0);
        const auto light = coefficientAfterKeyUp(20);
        const auto deep = coefficientAfterKeyUp(63);
        require(up == zero, "A pedal at 0 must release exactly like no pedal");
        require(light > zero && deep > light && deep < 1.0f,
                "A half pedal must lengthen the release in proportion to its depth");
        const auto latchedThenUp = coefficientAfterKeyUp(127, 0);
        require(latchedThenUp == zero, "The on/off pedal changed the release it gives on pedal-up");
        const auto latchedThenHalf = coefficientAfterKeyUp(127, 40);
        require(latchedThenHalf > zero && latchedThenHalf < 1.0f,
                "Lifting a latched pedal to a half position must release with the half-pedal tail");
    }

    // ---- Reference model: controllers and the fixed velocity law ----
    static float voiceLevelAt(int velocity, bool reference, int dyn = 0)
    {
        auto engine = instrument(48000.0);
        auto& layer = engine->layers[0];
        SampleEngine::measurePianoLevels(*layer.piano);
        auto routing = config();
        routing.enabled = true;
        routing.pianoReferenceModel = reference;
        routing.pianoDynamicCompression = dyn;
        routing.pianoDynamicsExponent = 2.2f;
        juce::AudioBuffer<float> audio(2, 64);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) velocity), 0);
        audio.clear();
        engine->renderLayer(layer, audio, midi, routing);
        for (const auto& voice : layer.voices)
            if (voice.active) return voice.level;
        require(false, "Reference model test: no voice");
        return 0.0f;
    }

    static void referenceVelocityLaw()
    {
        const auto top = voiceLevelAt(127, true);
        const auto db = [&] (int v, int dyn = 0) { return 20.0f * std::log10(voiceLevelAt(v, true, dyn) / top); };
        require(std::abs(db(64) - (-14.5f)) < 0.6f, "Velocity law: velocity 64 must sit 14.5 dB below 127");
        require(std::abs(db(96) - (-6.2f)) < 0.6f, "Velocity law: velocity 96 must sit 6.2 dB below 127");
        require(std::abs(db(8) - (-34.3f)) < 0.6f && std::abs(db(1) - (-34.3f)) < 0.6f,
                "Velocity law: soft notes must stop at the -34.3 dB floor");
        require(std::abs(db(122)) < 0.7f, "Velocity law: velocity 120 and above are the same level");
        // Dyn 3 raises the normalised velocity to 0.5: velocity 32 plays like 64.
        require(std::abs(db(32, 3) - db(64, 0)) < 0.7f, "Dyn 3 must make velocity 32 play like velocity 64");
        // Without the reference player model the old piano model is untouched.
        require(voiceLevelAt(64, false) < voiceLevelAt(127, false) * 0.3f && voiceLevelAt(1, false) < top * 0.01f,
                "The old piano model changed");
        std::cout << "Reference velocity law passed\n";
    }

    static void referenceControllers()
    {
        // Soft pedal: -3.7 dB on the notes struck while it is down.
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            routing.pianoDynamicsExponent = 2.2f;
            SampleEngine::measurePianoLevels(*layer.piano);
            juce::AudioBuffer<float> audio(2, 64);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 67, 127), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 1);
            engine->renderLayer(layer, audio, midi, routing);
            const auto soft = layer.voices[0].level;
            const auto plain = voiceLevelAt(100, false);
            require(std::abs(20.0f * std::log10(soft / plain) + 3.7f) < 0.3f, "Soft pedal must lower new notes by 3.7 dB");
        }
        // Sostenuto: holds the key that was down when the pedal went down, not a note struck afterwards.
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            juce::AudioBuffer<float> audio(2, 64);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 66, 127), 8);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 16);
            midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 24);
            midi.addEvent(juce::MidiMessage::noteOff(1, 64), 32);
            engine->renderLayer(layer, audio, midi, routing);
            int heldC = 0, releasedE = 0;
            for (const auto& voice : layer.voices)
            {
                if (!voice.active) continue;
                if (voice.note == 60 && voice.waitingForSustain && !voice.released) ++heldC;
                if (voice.note == 64 && voice.released) ++releasedE;
            }
            require(heldC == 1, "Sostenuto did not hold the key that was down");
            require(releasedE == 1, "Sostenuto must not hold a note struck after the pedal");
            midi.clear();
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 66, 0), 0);
            engine->renderLayer(layer, audio, midi, routing);
            int still = 0;
            for (const auto& voice : layer.voices)
                if (voice.active && voice.note == 60 && voice.waitingForSustain && !voice.released) ++still;
            require(still == 0, "Releasing the sostenuto pedal did not release the held note");
        }
        // Pitch bend +-2 semitones and CC7 squared law.
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            juce::AudioBuffer<float> audio(2, 1024);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, 64), 0);
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            midi.clear();
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing); // let the bend and volume settle
            const auto before = layer.voices[0].position;
            audio.clear();
            engine->renderLayer(layer, audio, midi, routing);
            const auto advance = (layer.voices[0].position - before) / 1024.0;
            const auto expected = layer.voices[0].increment * std::pow(2.0, 2.0 / 12.0);
            require(std::abs(advance / expected - 1.0) < 0.01, "Pitch bend must raise the pitch by two semitones");
            require(std::abs(layer.channelGainNow[0] - 0.254f) < 0.01f, "CC7 at 64 must be -12 dB (squared law)");
        }
        std::cout << "Reference controllers passed\n";
    }

    static void referenceReleaseAndHalfPedal()
    {
        // Release floor: the setting never acts faster than 0.55 s; Long Release doubles the setting.
        const auto coefficient = [] (float control, bool longRelease)
        {
            auto engine = instrument(48000.0);
            auto& layer = engine->layers[0];
            auto routing = config();
            routing.enabled = true;
            routing.release = control;
            routing.pianoReferenceModel = true;
            routing.pianoLongRelease = longRelease;
            juce::AudioBuffer<float> audio(2, 64);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 8);
            engine->renderLayer(layer, audio, midi, routing);
            for (const auto& voice : layer.voices)
                if (voice.active && voice.released) return voice.releaseCoefficient;
            require(false, "Release test: no released voice");
            return 0.0f;
        };
        require(coefficient(2.0f, false) == coefficient(40.0f, false), "Below the floor the release setting must not change anything");
        require(coefficient(50.0f, true) > coefficient(50.0f, false), "Long Release must lengthen the release");
        std::cout << "Reference release passed\n";
    }

    static void run()
    {
        decodedPianoCacheSharesPcm();
        streamedPianoCacheRoundTrip();
        halfPedalRelease();
        referenceVelocityLaw();
        referenceControllers();
        referenceReleaseAndHalfPedal();
        require(Sf2Engine::LayerConfig{}.pianoTimbre == -1,
                "A new NPNO layer must start with neutral OFF voicing");
        {
            auto lower = config();
            lower.lowVelocity = 1;
            lower.highVelocity = 100;
            auto upper = config();
            upper.lowVelocity = 101;
            upper.highVelocity = 127;
            require(lower.acceptsVelocity(1) && lower.acceptsVelocity(100)
                    && !lower.acceptsVelocity(101) && !upper.acceptsVelocity(100)
                    && upper.acceptsVelocity(101) && upper.acceptsVelocity(127),
                    "Velocity split must switch layers exactly between 100 and 101");
            auto engine = instrument(48000.0);
            juce::AudioBuffer<float> output(2, 64);
            juce::MidiBuffer notes;
            notes.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 101), 0);
            engine->renderLayer(engine->layers[0], output, notes, lower);
            require(active(*engine) == 0, "NPNO played a note above its velocity range");
            notes.clear();
            notes.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            engine->renderLayer(engine->layers[0], output, notes, lower);
            require(active(*engine) == 1, "NPNO missed the upper velocity boundary");
            notes.clear();
            notes.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            engine->renderLayer(engine->layers[0], output, notes, upper);
            require(engine->layers[0].voices[0].released,
                    "Note-off must release a sounding voice even after the velocity range changes");
        }
        for (const int channels : { 1, 2 })
        {
            NordPianoDecoder::Stroke stroke;
            stroke.loopStartFrame = 6;
            stroke.loopEndFrame = 12;
            stroke.interleavedPcm.resize((size_t) 16 * (size_t) channels);
            for (size_t i = 0; i < stroke.interleavedPcm.size(); ++i)
                stroke.interleavedPcm[i] = static_cast<std::int16_t>((int) i * 173 - 1500);
            for (const bool looped : { false, true })
                for (double position = 0.0; position < 15.5; position += 0.25)
                {
                    float left = 0.0f, right = 0.0f;
                    SampleEngine::readPianoStereoInterpolated(stroke, channels, position,
                                                               looped, left, right);
                    const auto originalLeft = SampleEngine::readPianoInterpolated(
                        stroke, channels, 0, position, looped);
                    const auto originalRight = SampleEngine::readPianoInterpolated(
                        stroke, channels, 1, position, looped);
                    require(std::abs(left - originalLeft) < 0.00001f
                            && std::abs(right - originalRight) < 0.00001f,
                            "Stereo piano interpolation changed at a sample or loop boundary");
                }
        }
        stealing();
        positiveOnlyTenthsDbTrim();
        pedalResonanceIsOptional();
        pianoDynamicsAndNaturalDecay();
        prefetcherHandlesMappedSamples();
        uprightReleaseReflections();
        reflectionLevelFollowsMainNote();
        restrikeUnderSustain();
        stageFourPianoControls();
        crossfades();
        adaptiveVelocity();
        for (const auto rate : { 44100.0, 48000.0 })
        {
            midiTiming(rate);
            sustainAndPolyphony(rate);
            invertedSustainPedal(rate);
            globalTransposeShiftsNotes(rate);
            mappedFaderIsNotChannelVolume(rate);
            lifetime(rate);
            shortenedDecay(rate);
            adsrSustain(rate);
            customVelocity(rate);
            loopDuringRelease(rate);
            require(partitionedDecay(rate, 127) == partitionedDecay(rate, 512),
                    "Piano decay/crossfade changed with audio block size");
            voicings(rate);
        }
    }
};

int main(int argc, char** argv)
{
    try
    {
        SampleEngineRegressionTests::run();
        for (int argument = 1; argument < argc; ++argument)
        {
            NordPianoDecoder::Instrument piano;
            const auto result = NordPianoDecoder::decodeFile(
                juce::File(juce::String::fromUTF8(argv[argument])), piano);
            if (result.failed())
                throw std::runtime_error(result.getErrorMessage().toStdString());
            SampleEngineRegressionTests::require(!piano.strokes.empty() && piano.channels >= 1,
                                                 "Real NPNO decoded without playable strokes");
            const auto pedalStrokes = std::count_if(piano.strokes.begin(), piano.strokes.end(),
                [](const auto& stroke) { return stroke.bank == 1; });
            size_t pcmBytes = 0;
            for (const auto& stroke : piano.strokes)
                pcmBytes += stroke.interleavedPcm.size() * sizeof(std::int16_t);
            std::cout << "Real NPNO: " << argv[argument] << ", " << piano.strokes.size()
                      << " strokes, " << pedalStrokes << " pedal-down strokes, "
                      << (pcmBytes / (1024 * 1024)) << " MiB decoded PCM\n";
        }
        if (argc > 2)
        {
            auto cacheHolder = std::make_unique<SampleEngine>();
            auto& cache = *cacheHolder;
            for (int argument = 1; argument < argc; ++argument)
                SampleEngineRegressionTests::require(
                    cache.preloadSample(juce::File(juce::String::fromUTF8(argv[argument]))).wasOk(),
                    "Live Set piano preload failed");
            for (int argument = 1; argument < argc; ++argument)
                SampleEngineRegressionTests::require(
                    SampleEngineRegressionTests::hasCachedPiano(
                        cache, juce::File(juce::String::fromUTF8(argv[argument]))),
                    "Live Set piano was evicted during bank preload");
            std::cout << "All Live Set pianos retained after preload\n";
        }
        std::cout << "NPNO velocity calibration, polyphony, sustain, lifetime and EQ: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
