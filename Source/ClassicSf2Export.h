#pragma once

#include "ClassicSoundFile.h"
#include "NordPianoLoop.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// NPNO piano -> SoundFont 2.04, for use in other players.
//
// A SoundFont cannot hold the whole Classic piano engine, so this is an approximation of it: the attack strokes with
// their velocity layers, loop seams, key tuning, the reference velocity/loudness law, the tone filter (baked into the
// audio), the decay of held notes (volume envelope) and a short release. Release noises, pedal reflections, soft
// pedal, half pedal and string resonance are not represented.
//
// Attenuation values assume the usual 0.4 scaling of the initialAttenuation generator (FluidSynth and most players).
namespace ClassicSf2Export
{
constexpr double attenuationScale = 0.4;
constexpr double releaseSeconds = 0.7;
constexpr double attackSeconds = 0.003;
constexpr int loopGuardFrames = 8, gapFrames = 46;

// Same velocity law the player uses for new piano loads (dB below velocity 127).
inline double velocityDb(int velocity)
{
    struct Point { int velocity; double db; };
    static constexpr std::array<Point, 18> table {{
        { 1, -34.3 }, { 16, -34.3 }, { 20, -33.4 }, { 24, -32.5 }, { 28, -30.8 }, { 32, -29.6 },
        { 40, -25.8 }, { 48, -21.4 }, { 56, -18.6 }, { 64, -14.5 }, { 72, -12.3 }, { 80, -9.8 },
        { 88, -8.2 }, { 96, -6.2 }, { 104, -4.0 }, { 112, -1.3 }, { 120, 0.0 }, { 127, 0.0 } }};
    const auto v = juce::jlimit(1, 127, velocity);
    for (size_t i = 1; i < table.size(); ++i)
        if (v <= table[i].velocity)
        {
            const auto& a = table[i - 1];
            const auto& b = table[i];
            return a.db + (b.db - a.db) * (double) (v - a.velocity) / (double) (b.velocity - a.velocity);
        }
    return 0.0;
}

inline juce::int16 timecents(double seconds)
{
    return (juce::int16) juce::jlimit(-12000, 8000, juce::roundToInt(1200.0 * std::log2(juce::jmax(0.001, seconds))));
}

class Writer final : public NordPianoDecoder::StrokeSink
{
public:
    explicit Writer(const juce::File& destinationFile)
        : destination(destinationFile),
          samplesFile(destinationFile.getSiblingFile(destinationFile.getFileName() + ".samples.part")) {}

    ~Writer() override
    {
        samples.reset();
        samplesFile.deleteFile();
    }

    bool available(std::uint64_t) override { return true; }
    bool completed() const noexcept { return complete; }

    juce::Result begin(const NordPianoDecoder::Instrument& header, std::uint32_t) override
    {
        channels = header.channels;
        kept.clear();
        sampleFrames = 0;
        complete = false;
        samplesFile.deleteFile();
        samples = std::make_unique<juce::FileOutputStream>(samplesFile);
        if (channels < 1 || channels > 2 || !samples->openedOk())
            return ClassicSoundFile::failure("Não foi possível criar o SoundFont.");
        return juce::Result::ok();
    }

    juce::Result stroke(NordPianoDecoder::Stroke& source) override
    {
        if (samples == nullptr) return ClassicSoundFile::damaged();
        const auto frames = source.frameCount(channels);
        if (source.bank != 0 || frames <= 0) return juce::Result::ok(); // release/pedal noises have no SF2 trigger

        auto prepared = source;
        NordPianoLoop::prepare(prepared, channels);
        Kept item;
        item.root = prepared.rootNote;
        item.layer = prepared.velocityLayer;
        item.trim = prepared.trimDb;
        item.looped = prepared.loopEndFrame > prepared.loopStartFrame + (std::uint32_t) loopGuardFrames
                      && prepared.loopStartFrame >= (std::uint32_t) loopGuardFrames
                      && prepared.loopEndFrame <= (std::uint32_t) frames;

        const auto window = juce::jmin((int) (0.3 * NordPianoDecoder::sampleRate), frames);
        int peak = 1;
        for (int i = 0; i < window * channels; ++i)
            peak = juce::jmax(peak, std::abs((int) prepared.interleavedPcm[(size_t) i]));
        item.peakDb = 20.0 * std::log10((double) peak / 32768.0);

        const auto length = item.looped ? (int) prepared.loopEndFrame : frames;
        item.loopStart = item.looped ? prepared.loopStartFrame : 0;
        // Tone of a middle velocity of this layer: the player's low-pass moves only between 2.9 and 2.2 kHz.
        const auto velocity = juce::jlimit(1.0, 127.0, 127.0 - ((double) item.layer + 0.5) * 127.0 / 31.0);
        const auto toneHz = velocity <= 30.0 ? 2900.0
                          : velocity >= 100.0 ? 2200.0
                          : velocity <= 64.0 ? 2900.0 * std::pow(2300.0 / 2900.0, (velocity - 30.0) / 34.0)
                          : 2300.0 * std::pow(2200.0 / 2300.0, (velocity - 64.0) / 36.0);
        const auto toneStep = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * toneHz / NordPianoDecoder::sampleRate);
        const auto airStep = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * 9000.0 / NordPianoDecoder::sampleRate);
        // Early decay (2.5 dB/s, at most 4 s) is part of the audio up to the loop, which stays at a constant level.
        const auto earlyLimit = juce::jmin(4.0, item.looped ? (double) item.loopStart / NordPianoDecoder::sampleRate : 4.0);

        std::vector<std::int16_t> output;
        for (int channel = 0; channel < channels; ++channel)
        {
            output.assign((size_t) length, 0);
            double tone = 0.0, air = 0.0;
            for (int frame = 0; frame < length; ++frame)
            {
                const auto x = (double) prepared.interleavedPcm[(size_t) (frame * channels + channel)];
                air += airStep * (x - air);
                tone += toneStep * (x - tone);
                const auto seconds = juce::jmin(earlyLimit, (double) frame / NordPianoDecoder::sampleRate);
                const auto value = (tone + 0.45 * (x - air)) * std::pow(10.0, -2.5 * seconds / 20.0);
                output[(size_t) frame] = (std::int16_t) juce::jlimit(-32768L, 32767L, std::lround(value));
            }
            auto& slot = item.sample[(size_t) channel];
            slot.start = sampleFrames;
            slot.frames = (std::uint32_t) length;
            samples->write(output.data(), output.size() * sizeof(std::int16_t));
            sampleFrames += (std::uint32_t) length;
            // SoundFont players may read past the loop end: repeat the loop start there, then leave silence.
            if (item.looped)
            {
                samples->write(output.data() + item.loopStart, loopGuardFrames * sizeof(std::int16_t));
                sampleFrames += loopGuardFrames;
            }
            samples->writeRepeatedByte(0, gapFrames * sizeof(std::int16_t));
            sampleFrames += gapFrames;
        }
        if (!samples->getStatus().wasOk() || sampleFrames > 0x7f000000u)
            return ClassicSoundFile::failure("Falha ao gravar o SoundFont (disco cheio ou piano grande demais).");
        kept.push_back(item);
        return juce::Result::ok();
    }

    juce::Result end(const NordPianoDecoder::Instrument& piano) override
    {
        if (samples == nullptr || kept.empty()) return ClassicSoundFile::damaged();
        samples->flush();
        const auto samplesOk = samples->getStatus().wasOk();
        samples.reset();
        if (!samplesOk) return ClassicSoundFile::failure("Falha ao gravar o SoundFont (disco cheio?).");

        // Zones: finer velocity steps follow the loudness law better; coarsen them if the SF2 tables would overflow.
        std::vector<Zone> zones;
        for (int step = 4; ; step *= 2)
        {
            zones = buildZones(piano, step);
            if (zones.size() * (size_t) channels <= 5400 || step >= 128) break; // 11 generators per zone, 65535 max
        }
        if (zones.empty() || zones.size() * (size_t) channels > 5900)
            return ClassicSoundFile::failure("Este piano tem camadas demais para um SoundFont.");

        const auto result = writeFile(piano, zones);
        samplesFile.deleteFile();
        complete = result.wasOk();
        return result;
    }

private:
    struct Slot { std::uint32_t start = 0, frames = 0; };
    struct Kept
    {
        int root = 60, layer = 0, trim = 0;
        double peakDb = 0.0;
        bool looped = false;
        std::uint32_t loopStart = 0;
        std::array<Slot, 2> sample {};
    };
    struct Zone { int low = 0, high = 0, velocityLow = 1, velocityHigh = 127, cents = 0, stroke = 0; double db = 0.0; };

    // The stroke the player picks for a key's root and a velocity (loudest/softest fallback instead of silence).
    int pick(int root, int velocity, int velocityScale) const
    {
        const auto limit = ((127 - velocity) * velocityScale) / 127;
        int best = -1, lowest = -1;
        for (int i = 0; i < (int) kept.size(); ++i)
        {
            const auto& item = kept[(size_t) i];
            if (item.root != root) continue;
            if (lowest < 0 || item.layer < kept[(size_t) lowest].layer) lowest = i;
            if (item.layer <= limit && (best < 0 || item.layer > kept[(size_t) best].layer)) best = i;
        }
        return best >= 0 ? best : lowest;
    }

    std::vector<Zone> buildZones(const NordPianoDecoder::Instrument& piano, int step) const
    {
        const auto strokeDb = [&piano] (const Kept& item)
        {
            const auto signedTrim = item.trim >= 0x8000 ? item.trim - 0x10000 : item.trim;
            return piano.gainTenthsDb / 10.0 + (piano.signedTenthsDbTrim ? -signedTrim / 10.0 : -(double) item.trim);
        };
        const auto centsFor = [&piano] (int key) { return juce::roundToInt(piano.fineTune[(size_t) key] * 0.7); };
        std::vector<Zone> zones;
        for (int low = 0; low < 128;)
        {
            const int root = piano.keyMap[(size_t) low];
            if (root == 0xff || pick(root, 127, piano.velocityScale) < 0) { ++low; continue; }
            const auto cents = centsFor(low);
            auto high = low;
            // At most four keys share a zone, so the per-key decay stays close.
            while (high + 1 < 128 && high - low < 3 && piano.keyMap[(size_t) (high + 1)] == root
                   && centsFor(high + 1) == cents)
                ++high;
            const auto& top = kept[(size_t) pick(root, 127, piano.velocityScale)];
            for (int velocity = 1; velocity < 128;)
            {
                const auto stroke = pick(root, velocity, piano.velocityScale);
                auto last = velocity;
                while (last < 127 && pick(root, last + 1, piano.velocityScale) == stroke
                       && velocityDb(last + 1) - velocityDb(velocity) <= step * 0.55)
                    ++last;
                const auto& item = kept[(size_t) stroke];
                const auto db = top.peakDb + strokeDb(top) + velocityDb((velocity + last) / 2) + 6.4 - item.peakDb;
                zones.push_back({ low, high, velocity, last, cents, stroke, db });
                velocity = last + 1;
            }
            low = high + 1;
        }
        return zones;
    }

    // ---- file -----------------------------------------------------------------------------
    static void name20(juce::MemoryOutputStream& out, const juce::String& text)
    {
        char buffer[20] {};
        const auto ascii = text.toStdString();
        size_t n = 0;
        for (const auto c : ascii)
            if (n < 19 && c >= 32 && c < 127) buffer[n++] = c;
        out.write(buffer, sizeof(buffer));
    }

    static void u16(juce::MemoryOutputStream& out, int value) { out.writeShort((short) (value & 0xffff)); }

    static void chunk(juce::OutputStream& out, const char* id, const juce::MemoryOutputStream& data)
    {
        out.write(id, 4);
        out.writeInt((int) data.getDataSize());
        out.write(data.getData(), data.getDataSize());
        if (data.getDataSize() & 1) out.writeByte(0);
    }

    juce::Result writeFile(const NordPianoDecoder::Instrument& piano, const std::vector<Zone>& zones) const
    {
        auto title = piano.name.trim();
        if (title.isEmpty()) title = destination.getFileNameWithoutExtension();

        juce::MemoryOutputStream phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
        name20(phdr, title); u16(phdr, 0); u16(phdr, 0); u16(phdr, 0); phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
        name20(phdr, "EOP"); u16(phdr, 0); u16(phdr, 0); u16(phdr, 1); phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
        u16(pbag, 0); u16(pbag, 0);
        u16(pbag, 1); u16(pbag, 0);
        u16(pgen, 41); u16(pgen, 0); // instrument 0
        u16(pgen, 0); u16(pgen, 0);
        for (int i = 0; i < 5; ++i) u16(pmod, 0);

        int generators = 0, bags = 0;
        const auto gen = [&] (int id, int amount) { u16(igen, id); u16(igen, amount); ++generators; };
        const auto bag = [&] (int modulatorIndex) { u16(ibag, generators); u16(ibag, modulatorIndex); ++bags; };

        // Global zone: envelope shared by all zones, and the default velocity modulators switched off (the zones
        // carry the velocity law themselves).
        bag(0);
        gen(34, timecents(attackSeconds));
        gen(38, timecents(releaseSeconds * 100.0 / 80.0));
        const auto modulator = [&] (int source, int target, int amountSource)
        { u16(imod, source); u16(imod, target); u16(imod, 0); u16(imod, amountSource); u16(imod, 0); };
        modulator(0x0502, 48, 0);
        modulator(0x0102, 8, 0);
        modulator(0x0102, 8, 0x0c02);
        modulator(0x0102, 8, 0x0d02);
        constexpr int modulatorCount = 4;

        auto loudest = -std::numeric_limits<double>::infinity();
        for (const auto& zone : zones) loudest = std::max(loudest, zone.db);

        for (const auto& zone : zones)
        {
            const auto& item = kept[(size_t) zone.stroke];
            const auto attenuation = juce::jlimit(0, 1440,
                juce::roundToInt((loudest - zone.db) * 10.0 / attenuationScale));
            const auto middle = (zone.low + zone.high) / 2;
            const auto slope = juce::jlimit(1.5, 9.0, 3.3 + 0.09 * (middle - 48)); // dB per second once looping
            for (int channel = 0; channel < channels; ++channel)
            {
                bag(modulatorCount);
                gen(43, zone.low | (zone.high << 8));
                gen(44, zone.velocityLow | (zone.velocityHigh << 8));
                gen(17, channels == 2 ? (channel == 0 ? -500 : 500) : 0);
                if (item.looped)
                {
                    gen(35, timecents((double) item.loopStart / NordPianoDecoder::sampleRate));
                    gen(36, timecents(100.0 / slope));
                    gen(37, 1000);
                }
                gen(48, attenuation);
                gen(52, zone.cents);
                gen(54, item.looped ? 1 : 0);
                gen(58, item.root);
                gen(53, zone.stroke * channels + channel); // sampleID must be last
            }
        }
        const auto totalGenerators = generators;
        gen(0, 0);
        u16(ibag, totalGenerators); u16(ibag, modulatorCount);
        for (int i = 0; i < 5; ++i) u16(imod, 0);
        name20(inst, title); u16(inst, 0);
        name20(inst, "EOI"); u16(inst, bags);
        if (totalGenerators > 65534 || bags > 65534) return ClassicSoundFile::damaged();

        for (int i = 0; i < (int) kept.size(); ++i)
        {
            const auto& item = kept[(size_t) i];
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto& slot = item.sample[(size_t) channel];
                name20(shdr, "n" + juce::String(item.root) + " v" + juce::String(item.layer) + " " + juce::String(i)
                             + (channels == 2 ? (channel == 0 ? "L" : "R") : ""));
                shdr.writeInt((int) slot.start);
                shdr.writeInt((int) (slot.start + slot.frames));
                shdr.writeInt((int) (slot.start + (item.looped ? item.loopStart : 0)));
                shdr.writeInt((int) (slot.start + slot.frames));
                shdr.writeInt(NordPianoDecoder::sampleRate);
                shdr.writeByte((char) item.root);
                shdr.writeByte(0);
                u16(shdr, channels == 2 ? i * 2 + (1 - channel) : 0);
                u16(shdr, channels == 2 ? (channel == 0 ? 4 : 2) : 1); // left, right, mono
            }
        }
        name20(shdr, "EOS");
        for (int i = 0; i < 5; ++i) shdr.writeInt(0);
        shdr.writeByte(0); shdr.writeByte(0); u16(shdr, 0); u16(shdr, 0);

        juce::MemoryOutputStream info, version, engine, bankName, pdta;
        u16(version, 2); u16(version, 4);
        engine.write("EMU8000\0", 8);
        const auto safeName = title.toStdString();
        bankName.write(safeName.c_str(), safeName.size() + 1);
        if (bankName.getDataSize() & 1) bankName.writeByte(0);
        info.write("INFO", 4);
        chunk(info, "ifil", version);
        chunk(info, "isng", engine);
        chunk(info, "INAM", bankName);
        pdta.write("pdta", 4);
        chunk(pdta, "phdr", phdr); chunk(pdta, "pbag", pbag); chunk(pdta, "pmod", pmod); chunk(pdta, "pgen", pgen);
        chunk(pdta, "inst", inst); chunk(pdta, "ibag", ibag); chunk(pdta, "imod", imod); chunk(pdta, "igen", igen);
        chunk(pdta, "shdr", shdr);

        ClassicSoundFile::PendingFile pending;
        if (const auto opened = pending.open(destination); opened.failed()) return opened;
        auto& out = *pending.get();
        const auto sampleBytes = (juce::int64) sampleFrames * 2;
        const auto sdtaBytes = 4 + 8 + sampleBytes;
        const auto riffBytes = 4 + (8 + (juce::int64) info.getDataSize()) + (8 + sdtaBytes)
                             + (8 + (juce::int64) pdta.getDataSize());
        out.write("RIFF", 4); out.writeInt((int) riffBytes); out.write("sfbk", 4);
        chunk(out, "LIST", info);
        out.write("LIST", 4); out.writeInt((int) sdtaBytes); out.write("sdta", 4);
        out.write("smpl", 4); out.writeInt((int) sampleBytes);
        {
            juce::FileInputStream audio(samplesFile);
            if (!audio.openedOk() || audio.getTotalLength() != sampleBytes
                || out.writeFromInputStream(audio, sampleBytes) != sampleBytes)
                return ClassicSoundFile::failure("Falha ao gravar o SoundFont (disco cheio?).");
        }
        chunk(out, "LIST", pdta);
        if (!pending.close()) return ClassicSoundFile::failure("Falha ao gravar o SoundFont (disco cheio?).");
        return pending.publish();
    }

    juce::File destination, samplesFile;
    std::unique_ptr<juce::FileOutputStream> samples;
    std::vector<Kept> kept;
    std::uint32_t sampleFrames = 0;
    int channels = 0;
    bool complete = false;
};
}
