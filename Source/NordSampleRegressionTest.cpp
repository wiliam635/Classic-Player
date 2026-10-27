#include "NordSampleEngine.h"
#include "NordLegacySampleDecoder.h"
#include "NordSampleExport.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

struct NordSampleRegressionTests
{
    static void putBe16(std::vector<std::uint8_t>& bytes, size_t offset, std::uint16_t value)
    {
        bytes[offset] = (std::uint8_t) (value >> 8);
        bytes[offset + 1] = (std::uint8_t) value;
    }

    static void putBe24(std::vector<std::uint8_t>& bytes, size_t offset, std::uint32_t value)
    {
        bytes[offset] = (std::uint8_t) (value >> 16);
        bytes[offset + 1] = (std::uint8_t) (value >> 8);
        bytes[offset + 2] = (std::uint8_t) value;
    }

    static void putBe32(std::vector<std::uint8_t>& bytes, size_t offset, std::uint32_t value)
    {
        bytes[offset] = (std::uint8_t) (value >> 24);
        bytes[offset + 1] = (std::uint8_t) (value >> 16);
        bytes[offset + 2] = (std::uint8_t) (value >> 8);
        bytes[offset + 3] = (std::uint8_t) value;
    }

    static void putLe32(std::vector<std::uint8_t>& bytes, size_t offset, std::uint32_t value)
    {
        bytes[offset] = (std::uint8_t) value;
        bytes[offset + 1] = (std::uint8_t) (value >> 8);
        bytes[offset + 2] = (std::uint8_t) (value >> 16);
        bytes[offset + 3] = (std::uint8_t) (value >> 24);
    }

    static void appendBe32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
        const auto offset = bytes.size();
        bytes.resize(offset + 4);
        putBe32(bytes, offset, value);
    }

    static void appendSection(std::vector<std::uint8_t>& body, const char* tag, std::uint32_t version,
                              const std::vector<std::uint8_t>& payload)
    {
        body.insert(body.end(), tag, tag + 4);
        appendBe32(body, version);
        appendBe32(body, (std::uint32_t) payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
    }

    static std::uint32_t crc32(const std::uint8_t* bytes, size_t length)
    {
        std::uint32_t value = 0xffffffffu;
        for (size_t i = 0; i < length; ++i)
        {
            value ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1u) ? 0xedb88320u : 0u);
        }
        return value ^ 0xffffffffu;
    }

    static void decoderUsesExplicitRegionPointers()
    {
        std::vector<std::uint8_t> body;
        appendSection(body, "NSMP", 21, {});

        std::vector<std::uint8_t> map(1334, 0);
        putBe24(map, 0, 0x100000u);
        for (int note = 0; note < 128; ++note)
            putBe24(map, 6 + (size_t) note * 10, 0x100000u);
        map[1317] = 1;
        map[1318] = 60;
        map[1319] = 127;
        map[1320] = 0;
        putBe32(map, 1326, 0x12345678u);
        map[1332] = 0;
        map[1333] = 127;
        appendSection(body, "\0map", 21, map);

        std::vector<std::uint8_t> header(76, 0);
        std::memcpy(header.data() + 10, "NSMP4 region test", 17);
        appendSection(body, "\0hdr", 1, header);

        std::vector<std::uint8_t> stroke(68, 0);
        putBe32(stroke, 0, 0x12345678u);
        stroke[5] = 60;
        stroke[12] = 23; // shift by one so eight samples exceed nominal int16 full scale.
        const auto block = (1u << 23) | (15u << 19) | 32u;
        const auto appendBlock = [&stroke](std::uint32_t value)
        {
            appendBe32(stroke, value);
            for (int word = 0; word < 16; ++word) appendBe32(stroke, 0);
        };
        appendBlock(block);
        // Bit 18 is loop-decay metadata, even when it appears away from LoopBegin.
        appendBlock(block | 0x40000u);
        appendBlock(block); // explicit LoopBegin
        appendBlock(block | 0x40000u); // decay marker after LoopBegin must not move it.
        appendBe32(stroke, 0x800020u); // mono stop block

        const auto sectionDataOffset = body.size() + 12;
        const auto pointerBase = ((sectionDataOffset + 68) / 4) % 65536;
        putBe16(stroke, 20, (std::uint16_t) (pointerBase + 0));
        putBe16(stroke, 29, (std::uint16_t) (pointerBase + 17));
        putBe16(stroke, 38, (std::uint16_t) (pointerBase + 34));
        putBe16(stroke, 47, (std::uint16_t) (pointerBase + 68));
        // Eight hot samples exercise the former >5% saturation rejection path.
        for (int word = 0; word < 4; ++word)
            putBe32(stroke, 68 + 4 + (size_t) word * 4, 0x7fff7fffu);
        appendSection(body, "\0stk", 1, stroke);

        std::vector<std::uint8_t> fileBytes(44, 0);
        std::memcpy(fileBytes.data(), "CBIN", 4);
        putLe32(fileBytes, 4, 1);
        std::memcpy(fileBytes.data() + 8, "nsmp", 4);
        putLe32(fileBytes, 20, 410);
        fileBytes.insert(fileBytes.end(), body.begin(), body.end());
        putLe32(fileBytes, 24, crc32(fileBytes.data() + 44, fileBytes.size() - 44));

        auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("classic-nsmp4-region-test", ".nsmp4", false);
        struct FileCleanup
        {
            juce::File file;
            ~FileCleanup() { file.deleteFile(); }
        } cleanup { file };
        require(file.replaceWithData(fileBytes.data(), fileBytes.size()),
                "Could not create the synthetic NSMP4 regression file");

        NordSampleDecoder::Instrument instrument;
        const auto result = NordSampleDecoder::decode(file, instrument);
        require(result.wasOk(), result.getErrorMessage().toRawUTF8());
        require(instrument.zones.size() == 1, "Synthetic NSMP4 did not produce one zone");
        const auto& zone = instrument.zones.front();
        require(zone.frames() == 128 && zone.secondStart == 32,
                "NSMP4 did not map its alternate-start pointer to sample frames");
        require(zone.loopStart == 64 && zone.loopEnd == 128,
                "Loop-decay flag incorrectly replaced the explicit NSMP4 LoopBegin pointer");
        require(zone.clippedSamples == 8 && zone.pcm[0] > 0.99f && zone.pcm[0] <= 1.0f,
                "NSMP4 hot samples were rejected or hard-clipped instead of soft-limited");

        auto exportFolder = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("classic-nord-wav-export", "", false);
        require(exportFolder.createDirectory().wasOk(), "Could not create WAV export test folder");
        struct FolderCleanup
        {
            juce::File folder;
            ~FolderCleanup() { folder.deleteRecursively(); }
        } folderCleanup { exportFolder };
        int exported = 0;
        const auto exportResult = NordSampleExport::exportZonesAsWav(file, exportFolder, exported);
        require(exportResult.wasOk(), exportResult.getErrorMessage().toRawUTF8());
        juce::Array<juce::File> wavFiles;
        exportFolder.findChildFiles(wavFiles, juce::File::findFiles, false, "*.wav");
        require(exported == 1 && wavFiles.size() == 1 && wavFiles[0].getSize() > 44,
                "NSMP4 zone WAV export did not produce a playable-sized file");
    }

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
        layer.instrument = std::make_shared<NordSampleDecoder::Instrument>();
        layer.instrument->name = "Synthetic NSMP4";
        layer.instrument->gain.fill(1.0f);
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
        layer.instrument->zones.push_back(std::move(zone));
        return engine;
    }

    static void decodedSampleCacheSharesPcm()
    {
        NordSampleEngine engine;
        const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("classic-nsmp-cache-test.nsmp4");
        auto decoded = std::make_shared<NordSampleDecoder::Instrument>();
        const auto cached = engine.cacheInstrument(source, decoded);
        require(cached == decoded && engine.findCachedInstrument(source) == decoded,
                "NSMP preload cache copied instead of sharing decoded PCM");
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

    static void padLoopCrossfade()
    {
        const auto makeZone = [] {
            NordSampleDecoder::Zone zone;
            zone.channels = 1;
            zone.loopStart = 1000;
            zone.loopEnd = 21000;
            zone.secondStart = 500;
            zone.pcm.assign(zone.loopEnd, 0.75f);
            std::fill(zone.pcm.begin() + 16000, zone.pcm.end(), -0.75f);
            return zone;
        };

        for (const auto* name : { "Sweet Saw Pad 1", "Cinematic Strings Leg", "Full Orchestra 1",
                                  "Saxophone", "Pulse Bass", "Royal Grand 3D" })
        {
            NordSampleDecoder::Instrument instrument;
            instrument.name = name;
            instrument.zones.push_back(makeZone());
            NordSampleEngine::prepareLoopCrossfades(instrument);
            const auto& zone = instrument.zones[0];
            require(zone.loopStart == 1000 && zone.loopEnd == 21000 && zone.secondStart == 500,
                    "NSMP4 loop processing changed its pitch or alternate start");
            require(std::abs(zone.pcm.back() - zone.pcm[1000]) < 0.0001f,
                    "NSMP4 loop seam still has a sample discontinuity");
        }

        NordSampleDecoder::Instrument pitched;
        auto cycle = makeZone();
        cycle.loopStart = 1000;
        cycle.loopEnd = 1268; // 268 frames at 35,002 Hz: the recorded saxophone case.
        cycle.pcm.resize(cycle.loopEnd);
        for (size_t i = 0; i < cycle.pcm.size(); ++i)
            cycle.pcm[i] = std::sin(juce::MathConstants<float>::twoPi * (float) i / 268.0f);
        pitched.zones.push_back(std::move(cycle));
        NordSampleEngine::prepareLoopCrossfades(pitched);
        require(pitched.zones[0].loopEnd - pitched.zones[0].loopStart == 268,
                "Short NSMP4 loop was shortened, raising its held-note pitch");
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

    static void monoLegatoAndNoteReturn()
    {
        auto engine = fixture();
        engine->layers[0].instrument->zones[0].secondStart = 512;
        auto config = routing();
        config.mono = true;
        float rms = 0.0f;
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        render(*engine, midi, config, rms);
        require(engine->layers[0].monoNote == 60 && engine->layers[0].heldNotes[60],
                "NSMP4 mono mode did not track its first held note");

        // Repeating the same key is a retrigger, not a legato skip of the attack.
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        render(*engine, midi, config, rms);
        auto current = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                                    [](const auto& voice) { return voice.active && voice.keyDown; });
        require(current != engine->layers[0].voices.end() && current->position < 128.0,
                "NSMP4 same-note retrigger incorrectly skipped to SecondStart");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(2, 67, 0.8f), 0);
        render(*engine, midi, config, rms);
        require(engine->layers[0].monoNote == 67 && engine->layers[0].heldNotes[60]
                    && engine->layers[0].heldNotes[67],
                "NSMP4 mono mode allowed an Omni-channel chord instead of a legato note");
        current = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                                [](const auto& voice) { return voice.active && voice.keyDown; });
        require(current != engine->layers[0].voices.end() && current->note == 67
                    && current->channel == 2 && current->position > 512.0,
                "NSMP4 legato note did not start at the sample's alternate start point");
        for (int block = 0; block < 4; ++block)
        {
            midi.clear();
            render(*engine, midi, config, rms);
        }
        require(std::none_of(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                             [](const auto& voice) { return voice.active && voice.note == 60; }),
                "NSMP4 mono mode kept the previous note sounding for the user release time");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
        render(*engine, midi, config, rms);
        require(engine->layers[0].monoNote == 60,
                "NSMP4 mono note stack did not return to a still-held lower note");
        current = std::find_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                               [](const auto& voice) { return voice.active && voice.keyDown; });
        require(current != engine->layers[0].voices.end() && current->note == 60
                    && current->channel == 1 && current->position > 512.0,
                "NSMP4 fallback note did not use the alternate sample start");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        render(*engine, midi, config, rms);
        require(engine->layers[0].monoNote == -1
                    && std::none_of(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                                    [](const auto& voice) { return voice.active && voice.keyDown; }),
                "NSMP4 mono note-off left a key-down voice active");
    }

    static void monoModeChangeReleasesPriorVoices()
    {
        auto engine = fixture();
        auto config = routing();
        float rms = 0.0f;
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        render(*engine, midi, config, rms);

        config.mono = true;
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(2, 67, 0.8f), 0);
        render(*engine, midi, config, rms);
        require(engine->layers[0].monoNote == 67
                    && std::count_if(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                                     [](const auto& voice) { return voice.active && voice.keyDown; }) == 1,
                "Enabling NSMP4 mono mode left earlier polyphonic notes held");

        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
        render(*engine, midi, config, rms);
        require(std::none_of(engine->layers[0].voices.begin(), engine->layers[0].voices.end(),
                             [](const auto& voice) { return voice.active && voice.keyDown; }),
                "NSMP4 mono note-off left a key held after switching from poly mode");
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
            const auto result = file.hasFileExtension(".nsmp4")
                ? NordSampleDecoder::decode(file, instrument)
                : NordLegacySampleDecoder::decodeFile(file, instrument);
            require(result.wasOk(), result.getErrorMessage().toRawUTF8());
            require(!instrument.zones.empty(), "Nord Sample decode returned no key zones");
            size_t samples = 0;
            for (const auto& zone : instrument.zones)
            {
                require(zone.frames() > 0 && zone.channels >= 1 && zone.channels <= 2,
                        "Nord Sample zone has invalid PCM dimensions");
                samples += zone.pcm.size();
                require(std::all_of(zone.pcm.begin(), zone.pcm.end(),
                                    [](float sample) { return std::isfinite(sample); }),
                        "Nord Sample decode produced NaN or infinity");
            }
            std::vector<std::pair<size_t, size_t>> loopPoints;
            for (const auto& zone : instrument.zones)
                loopPoints.emplace_back(zone.loopStart, zone.loopEnd);
            NordSampleEngine::prepareLoopCrossfades(instrument);
            for (size_t zoneIndex = 0; zoneIndex < instrument.zones.size(); ++zoneIndex)
            {
                const auto& zone = instrument.zones[zoneIndex];
                require(zone.loopStart == loopPoints[zoneIndex].first
                            && zone.loopEnd == loopPoints[zoneIndex].second,
                        "NSMP4 real-file loop pitch changed during seam smoothing");
                require(std::all_of(zone.pcm.begin(), zone.pcm.end(),
                                    [](float sample) { return std::isfinite(sample) && std::abs(sample) < 4.0f; }),
                        "Nord Sample loop smoothing produced unstable PCM");
            }
            auto engine = std::make_unique<NordSampleEngine>();
            engine->prepare(48000.0, 512);
            const auto loaded = engine->loadSample(0, file);
            require(loaded.wasOk(), loaded.getErrorMessage().toRawUTF8());
            require(engine->isLoaded(0), "Nord Sample could not be loaded into its layer");
            auto exportFolder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getNonexistentChildFile("classic-nord-real-wav-export", "", false);
            require(exportFolder.createDirectory().wasOk(), "Could not create real-file WAV export folder");
            struct FolderCleanup
            {
                juce::File folder;
                ~FolderCleanup() { folder.deleteRecursively(); }
            } folderCleanup { exportFolder };
            int exported = 0;
            const auto exportResult = NordSampleExport::exportZonesAsWav(file, exportFolder, exported);
            require(exportResult.wasOk(), exportResult.getErrorMessage().toRawUTF8());
            require(exported == static_cast<int>(instrument.zones.size()),
                    "Nord Sample did not export every zone as WAV");
            std::cout << file.getFileName() << ": " << instrument.zones.size()
                      << " zones, " << samples << " decoded samples\n";
        }
    }

    static void run(int argc, char** argv)
    {
        decodedSampleCacheSharesPcm();
        decoderUsesExplicitRegionPointers();
        padLoopCrossfade();
        audioAndSustain();
        monoLegatoAndNoteReturn();
        monoModeChangeReleasesPriorVoices();
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
        std::cout << "Nord Sample region mapping, decode, playback, sustain and polyphony: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
