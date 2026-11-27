#pragma once

#include "NordPianoDecoder.h"
#include "NordSampleDecoder.h"
#include "PcmBuffer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

// Classic Player instrument files: ".ckpiano" (a decoded NPNO piano) and ".cksample" (a decoded NSMP sample).
//
// They hold exactly what the vendor decoder produces, before any playback preparation (loop seams are prepared by
// the player when it unpacks the file, so later engine improvements apply to files converted today). Every stroke or
// zone is an independent FLAC stream: lossless, and unpacked one block at a time. Each block carries a hash of its
// PCM, so a damaged file is rejected instead of played.
//
// All integers are little endian (JUCE stream defaults). Layout:
//   magic "CPSF", format version, kind, instrument header, block count, blocks..., end marker
//   block = fields, frame count (int64), PCM hash (int64), FLAC byte count (int64), FLAC bytes
namespace ClassicSoundFile
{
constexpr juce::uint32 magic = 0x46535043;     // "CPSF"
constexpr juce::uint32 formatVersion = 1;      // readers must keep opening every older version
constexpr juce::uint32 endMarker = 0x454E4421; // "END!"
constexpr juce::uint32 pianoKind = 1, sampleKind = 2;
constexpr juce::uint64 blockLimitBytes = 256ull * 1024 * 1024;
constexpr juce::uint64 totalLimitBytes = 6ull * 1024 * 1024 * 1024;

inline bool isPianoFile(const juce::File& file) { return file.hasFileExtension(".npno;.ckpiano"); }
inline bool isSampleFile(const juce::File& file) { return file.hasFileExtension(".nsmp;.nsmp3;.nsmp4;.cksample"); }

inline juce::Result failure(const char* utf8) { return juce::Result::fail(juce::String::fromUTF8(utf8)); }
inline juce::Result damaged() { return failure("O arquivo da Classic está incompleto ou corrompido."); }

inline juce::int64 pcmHash(const std::int16_t* pcm, size_t count) noexcept
{
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a, one 16-bit sample per step
    for (size_t i = 0; i < count; ++i)
        hash = (hash ^ static_cast<std::uint16_t>(pcm[i])) * 1099511628211ull;
    return static_cast<juce::int64>(hash);
}

// ---- strings and blocks ------------------------------------------------------------------
inline void writeText(juce::OutputStream& out, const juce::String& text)
{
    const auto utf8 = text.toStdString();
    out.writeInt((int) utf8.size());
    out.write(utf8.data(), utf8.size());
}

inline bool readText(juce::InputStream& in, juce::String& text)
{
    const auto size = in.readInt();
    if (size < 0 || size > 4096 || in.getNumBytesRemaining() < size) return false;
    std::string buffer((size_t) size, '\0');
    if (size > 0 && in.read(buffer.data(), size) != size) return false;
    text = juce::String::fromUTF8(buffer.data(), size);
    return true;
}

inline bool readExact(juce::InputStream& in, void* destination, int bytes)
{
    return in.read(destination, bytes) == bytes;
}

inline bool writeAudio(juce::OutputStream& out, const PcmBuffer& pcm, int channels)
{
    if (channels < 1 || channels > 2 || pcm.size() % (size_t) channels != 0) return false;
    const auto frames = pcm.size() / (size_t) channels;
    juce::MemoryBlock encoded;
    if (frames > 0)
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(encoded, false);
        auto writer = flac.createWriterFor(stream, juce::AudioFormatWriterOptions{}
            .withSampleRate(44100.0) // nominal: the player uses the Nord rate, not this header field
            .withNumChannels(channels)
            .withBitsPerSample(16)
            .withQualityOptionIndex(5));
        if (writer == nullptr) return false;
        constexpr int chunk = 8192;
        std::vector<int> left((size_t) chunk), right((size_t) chunk);
        const int* channelData[] { left.data(), channels == 2 ? right.data() : nullptr, nullptr };
        for (size_t frame = 0; frame < frames; frame += chunk)
        {
            const auto count = (int) std::min<size_t>(chunk, frames - frame);
            for (int i = 0; i < count; ++i)
            {
                // JUCE integer audio is left-justified in 32 bits.
                const auto index = (frame + (size_t) i) * (size_t) channels;
                left[(size_t) i] = (int) pcm[index] * 65536;
                if (channels == 2) right[(size_t) i] = (int) pcm[index + 1] * 65536;
            }
            if (!writer->write(channelData, count)) return false;
        }
        writer.reset(); // finalises the FLAC stream and releases the memory stream
        if (encoded.getSize() == 0) return false;
    }
    out.writeInt64((juce::int64) frames);
    out.writeInt64(pcmHash(pcm.data(), pcm.size()));
    out.writeInt64((juce::int64) encoded.getSize());
    return encoded.getSize() == 0 || out.write(encoded.getData(), encoded.getSize());
}

struct BlockInfo
{
    juce::uint64 frames = 0, flacBytes = 0;
    juce::int64 hash = 0;
};

inline bool readBlockInfo(juce::InputStream& in, int channels, BlockInfo& info)
{
    info.frames = (juce::uint64) in.readInt64();
    info.hash = in.readInt64();
    info.flacBytes = (juce::uint64) in.readInt64();
    return info.frames <= blockLimitBytes / ((juce::uint64) channels * sizeof(std::int16_t))
        && info.flacBytes <= blockLimitBytes
        && info.flacBytes <= (juce::uint64) juce::jmax<juce::int64>(0, in.getNumBytesRemaining())
        && (info.frames == 0) == (info.flacBytes == 0);
}

inline bool readAudio(juce::InputStream& in, const BlockInfo& info, int channels, std::vector<std::int16_t>& pcm)
{
    pcm.clear();
    if (info.frames == 0) return info.hash == pcmHash(nullptr, 0);
    juce::MemoryBlock encoded((size_t) info.flacBytes);
    if (!readExact(in, encoded.getData(), (int) info.flacBytes)) return false;
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader(
        flac.createReaderFor(new juce::MemoryInputStream(encoded, false), true));
    if (reader == nullptr || (int) reader->numChannels != channels || reader->bitsPerSample != 16
        || (juce::uint64) reader->lengthInSamples != info.frames)
        return false;
    pcm.resize((size_t) info.frames * (size_t) channels);
    constexpr int chunk = 8192;
    std::vector<int> left((size_t) chunk), right((size_t) chunk);
    int* channelData[] { left.data(), channels == 2 ? right.data() : nullptr };
    for (juce::uint64 frame = 0; frame < info.frames; frame += chunk)
    {
        const auto count = (int) std::min<juce::uint64>(chunk, info.frames - frame);
        if (!reader->read(channelData, channels, (juce::int64) frame, count, false)) return false;
        for (int i = 0; i < count; ++i)
        {
            const auto index = ((size_t) frame + (size_t) i) * (size_t) channels;
            pcm[index] = (std::int16_t) (left[(size_t) i] / 65536);
            if (channels == 2) pcm[index + 1] = (std::int16_t) (right[(size_t) i] / 65536);
        }
    }
    return pcmHash(pcm.data(), pcm.size()) == info.hash;
}

inline bool readFileHeader(juce::InputStream& in, juce::uint32 kind)
{
    return (juce::uint32) in.readInt() == magic
        && (juce::uint32) in.readInt() == formatVersion
        && (juce::uint32) in.readInt() == kind;
}

// Writes through a temporary file next to the destination and only publishes a complete file.
class PendingFile
{
public:
    ~PendingFile() { discard(); }

    juce::Result open(const juce::File& destinationFile)
    {
        discard();
        target = destinationFile;
        temp = target.getSiblingFile(target.getFileName() + ".part");
        temp.deleteFile();
        stream = std::make_unique<juce::FileOutputStream>(temp);
        if (!stream->openedOk())
        {
            discard();
            return failure("Não foi possível criar o arquivo convertido.");
        }
        return juce::Result::ok();
    }

    juce::FileOutputStream* get() const noexcept { return stream.get(); }
    const juce::File& temporary() const noexcept { return temp; }

    bool close()
    {
        if (stream == nullptr) return false;
        stream->flush();
        const auto ok = stream->getStatus().wasOk();
        stream.reset();
        return ok;
    }

    juce::Result publish()
    {
        if (target.existsAsFile() && !target.deleteFile())
            return failure("Não foi possível substituir o arquivo convertido.");
        if (!temp.moveFileTo(target))
            return failure("Não foi possível concluir o arquivo convertido.");
        temp = juce::File();
        return juce::Result::ok();
    }

private:
    void discard() noexcept
    {
        stream.reset();
        if (temp != juce::File()) temp.deleteFile();
        temp = juce::File();
    }

    juce::File target, temp;
    std::unique_ptr<juce::FileOutputStream> stream;
};

// ---- piano -------------------------------------------------------------------------------
// Receives the strokes straight from the decoder (or any other producer), so converting needs about one stroke of RAM.
class PianoWriter final : public NordPianoDecoder::StrokeSink
{
public:
    explicit PianoWriter(const juce::File& destinationFile) : destination(destinationFile) {}

    bool available(std::uint64_t) override { return true; }

    juce::Result begin(const NordPianoDecoder::Instrument& header, std::uint32_t keptStrokes) override
    {
        if (const auto opened = pending.open(destination); opened.failed()) return opened;
        auto& out = *pending.get();
        out.writeInt((int) magic);
        out.writeInt((int) formatVersion);
        out.writeInt((int) pianoKind);
        writeText(out, header.name);
        writeText(out, header.voicing);
        out.writeInt((int) header.streamVersion);
        out.writeInt(header.channels);
        out.writeInt(header.gainTenthsDb);
        patchPosition = out.getPosition();
        out.writeInt(0); // signedTenthsDbTrim, patched in end()
        out.writeInt(0); // velocityScale, patched in end()
        out.write(header.keyMap.data(), header.keyMap.size());
        out.write(header.fineTune.data(), header.fineTune.size());
        out.writeInt((int) keptStrokes);
        channels = header.channels;
        expected = keptStrokes;
        written = 0;
        return juce::Result::ok();
    }

    juce::Result stroke(NordPianoDecoder::Stroke& stroke) override
    {
        auto* out = pending.get();
        if (out == nullptr || written >= expected) return damaged();
        out->writeInt((int) stroke.rootNote);
        out->writeInt((int) stroke.bank);
        out->writeInt((int) stroke.velocityLayer);
        out->writeInt((int) stroke.trimDb);
        out->writeInt((int) stroke.loopStartFrame);
        out->writeInt((int) stroke.loopEndFrame);
        if (!writeAudio(*out, stroke.interleavedPcm, channels) || !out->getStatus().wasOk())
            return failure("Falha ao gravar o piano convertido (disco cheio?).");
        ++written;
        return juce::Result::ok();
    }

    // Completes the temporary file. Call publish() after checking it.
    juce::Result end(const NordPianoDecoder::Instrument& finished) override
    {
        auto* out = pending.get();
        if (out == nullptr || written != expected) return damaged();
        out->writeInt((int) endMarker);
        const auto endPosition = out->getPosition();
        out->setPosition(patchPosition);
        out->writeInt(finished.signedTenthsDbTrim ? 1 : 0);
        out->writeInt(finished.velocityScale);
        out->setPosition(endPosition);
        if (!pending.close()) return failure("Falha ao gravar o piano convertido (disco cheio?).");
        complete = true;
        return juce::Result::ok();
    }

    bool completed() const noexcept { return complete; }
    const juce::File& temporary() const noexcept { return pending.temporary(); }
    juce::Result publish() { return complete ? pending.publish() : damaged(); }

private:
    juce::File destination;
    PendingFile pending;
    juce::int64 patchPosition = 0;
    int channels = 0;
    std::uint32_t expected = 0, written = 0;
    bool complete = false;
};

// Writes an instrument that is already in memory (used by tests and small tools).
inline juce::Result writePiano(const juce::File& destination, const NordPianoDecoder::Instrument& instrument)
{
    PianoWriter writer(destination);
    if (const auto result = writer.begin(instrument, (std::uint32_t) instrument.strokes.size()); result.failed())
        return result;
    for (const auto& source : instrument.strokes)
    {
        auto copy = source;
        if (const auto result = writer.stroke(copy); result.failed()) return result;
    }
    if (const auto result = writer.end(instrument); result.failed()) return result;
    return writer.publish();
}

// Reads a .ckpiano. With a sink that accepts the audio, each stroke is handed over and freed (the returned instrument
// then has the stroke list without PCM, like NordPianoDecoder::decodeFile); otherwise everything stays in RAM within
// maxDecodedBytes. checkOnly verifies every block and keeps nothing.
inline juce::Result readPiano(const juce::File& file, NordPianoDecoder::Instrument& destination,
                              std::uint64_t maxDecodedBytes, NordPianoDecoder::StrokeSink* sink = nullptr,
                              bool checkOnly = false)
{
    destination = {};
    juce::FileInputStream in(file);
    if (!in.openedOk()) return failure("Não foi possível ler o arquivo de piano da Classic.");
    if (!readFileHeader(in, pianoKind))
        return failure("Este .ckpiano não é válido ou foi criado por uma versão mais nova da Classic Player.");

    NordPianoDecoder::Instrument parsed;
    if (!readText(in, parsed.name) || !readText(in, parsed.voicing)) return damaged();
    parsed.streamVersion = (std::uint16_t) in.readInt();
    parsed.channels = in.readInt();
    parsed.gainTenthsDb = in.readInt();
    parsed.signedTenthsDbTrim = in.readInt() != 0;
    parsed.velocityScale = in.readInt();
    if (parsed.channels < 1 || parsed.channels > 2 || parsed.velocityScale < 1
        || !readExact(in, parsed.keyMap.data(), (int) parsed.keyMap.size())
        || !readExact(in, parsed.fineTune.data(), (int) parsed.fineTune.size()))
        return damaged();
    const auto count = in.readInt();
    if (count < 1 || count > 100000) return damaged();

    // First pass: block headers only, to know the decoded size before allocating or writing anything.
    const auto firstBlock = in.getPosition();
    std::uint64_t totalBytes = 0;
    for (int i = 0; i < count; ++i)
    {
        in.skipNextBytes(6 * 4);
        BlockInfo info;
        if (!readBlockInfo(in, parsed.channels, info)) return damaged();
        totalBytes += info.frames * (std::uint64_t) parsed.channels * sizeof(std::int16_t);
        if (totalBytes > totalLimitBytes || !in.setPosition(in.getPosition() + (juce::int64) info.flacBytes))
            return damaged();
    }
    if ((juce::uint32) in.readInt() != endMarker || !in.setPosition(firstBlock)) return damaged();

    const auto streaming = !checkOnly && sink != nullptr && sink->available(totalBytes);
    if (!checkOnly && !streaming && totalBytes > maxDecodedBytes)
        return failure("Este piano não cabe na memória disponível deste computador.");
    if (streaming)
        if (const auto began = sink->begin(parsed, (std::uint32_t) count); began.failed()) return began;

    std::array<bool, 128> rootsWithAttack {};
    parsed.strokes.reserve((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        NordPianoDecoder::Stroke stroke;
        const auto root = in.readInt(), bank = in.readInt(), layer = in.readInt(), trim = in.readInt();
        const auto loopStart = in.readInt(), loopEnd = in.readInt();
        BlockInfo info;
        if (root < 0 || root > 127 || bank < 0 || bank > 2 || layer < 0 || layer > 255
            || trim < 0 || trim > 0xffff || !readBlockInfo(in, parsed.channels, info)
            || loopStart < 0 || loopEnd < loopStart || (juce::uint64) loopEnd > info.frames)
            return damaged();
        stroke.rootNote = (std::uint8_t) root;
        stroke.bank = (std::uint8_t) bank;
        stroke.velocityLayer = (std::uint8_t) layer;
        stroke.trimDb = (std::uint16_t) trim;
        stroke.loopStartFrame = (std::uint32_t) loopStart;
        stroke.loopEndFrame = (std::uint32_t) loopEnd;
        if (!readAudio(in, info, parsed.channels, stroke.interleavedPcm.vec())) return damaged();
        if (streaming)
            if (const auto stored = sink->stroke(stroke); stored.failed()) return stored;
        if (streaming || checkOnly)
            std::vector<std::int16_t>().swap(stroke.interleavedPcm.vec());
        if (bank == 0) rootsWithAttack[(size_t) root] = true;
        parsed.strokes.push_back(std::move(stroke));
    }
    for (const auto root : parsed.keyMap)
        if (root != 0xff && (root >= rootsWithAttack.size() || !rootsWithAttack[root])) return damaged();
    if (streaming)
        if (const auto finished = sink->end(parsed); finished.failed()) return finished;
    destination = std::move(parsed);
    return juce::Result::ok();
}

// ---- sample ------------------------------------------------------------------------------
inline juce::Result writeSample(const juce::File& destination, const NordSampleDecoder::Instrument& instrument,
                                PendingFile& pending)
{
    if (const auto opened = pending.open(destination); opened.failed()) return opened;
    auto& out = *pending.get();
    out.writeInt((int) magic);
    out.writeInt((int) formatVersion);
    out.writeInt((int) sampleKind);
    writeText(out, instrument.name);
    out.write(instrument.gain.data(), instrument.gain.size() * sizeof(float));
    out.write(instrument.detune.data(), instrument.detune.size() * sizeof(float));
    out.writeInt((int) instrument.zones.size());
    for (const auto& zone : instrument.zones)
    {
        out.writeInt(zone.root); out.writeInt(zone.low); out.writeInt(zone.high);
        out.writeInt(zone.velocityLow); out.writeInt(zone.velocityHigh); out.writeInt(zone.channels);
        out.writeFloat(zone.gain);
        out.writeInt64((juce::int64) zone.secondStart);
        out.writeInt64((juce::int64) zone.loopStart);
        out.writeInt64((juce::int64) zone.loopEnd);
        out.writeInt64((juce::int64) zone.clippedSamples);
        if (!writeAudio(out, zone.pcm, zone.channels))
            return failure("Falha ao gravar a amostra convertida (disco cheio?).");
    }
    out.writeInt((int) endMarker);
    return pending.close() ? juce::Result::ok() : failure("Falha ao gravar a amostra convertida (disco cheio?).");
}

inline juce::Result writeSample(const juce::File& destination, const NordSampleDecoder::Instrument& instrument)
{
    PendingFile pending;
    if (const auto result = writeSample(destination, instrument, pending); result.failed()) return result;
    return pending.publish();
}

inline juce::Result readSample(const juce::File& file, NordSampleDecoder::Instrument& destination,
                               std::uint64_t maxDecodedBytes, bool checkOnly = false)
{
    destination = {};
    juce::FileInputStream in(file);
    if (!in.openedOk()) return failure("Não foi possível ler o arquivo de amostra da Classic.");
    if (!readFileHeader(in, sampleKind))
        return failure("Este .cksample não é válido ou foi criado por uma versão mais nova da Classic Player.");

    NordSampleDecoder::Instrument parsed;
    if (!readText(in, parsed.name)
        || !readExact(in, parsed.gain.data(), (int) (parsed.gain.size() * sizeof(float)))
        || !readExact(in, parsed.detune.data(), (int) (parsed.detune.size() * sizeof(float))))
        return damaged();
    const auto count = in.readInt();
    if (count < 1 || count > 100000) return damaged();
    parsed.zones.reserve((size_t) count);
    std::uint64_t totalBytes = 0;
    for (int i = 0; i < count; ++i)
    {
        NordSampleDecoder::Zone zone;
        zone.root = in.readInt(); zone.low = in.readInt(); zone.high = in.readInt();
        zone.velocityLow = in.readInt(); zone.velocityHigh = in.readInt(); zone.channels = in.readInt();
        zone.gain = in.readFloat();
        const auto secondStart = in.readInt64(), loopStart = in.readInt64(), loopEnd = in.readInt64();
        const auto clipped = in.readInt64();
        BlockInfo info;
        if (zone.channels < 1 || zone.channels > 2 || !readBlockInfo(in, zone.channels, info)) return damaged();
        const auto frames = (juce::int64) info.frames;
        if (secondStart < 0 || secondStart > frames || loopStart < 0 || loopStart > frames
            || loopEnd < 0 || loopEnd > frames || clipped < 0)
            return damaged();
        zone.secondStart = (size_t) secondStart;
        zone.loopStart = (size_t) loopStart;
        zone.loopEnd = (size_t) loopEnd;
        zone.clippedSamples = (size_t) clipped;
        totalBytes += info.frames * (std::uint64_t) zone.channels * sizeof(std::int16_t);
        if (totalBytes > totalLimitBytes) return damaged();
        if (!checkOnly && totalBytes > maxDecodedBytes)
            return failure("Este timbre não cabe na memória disponível deste computador.");
        if (!readAudio(in, info, zone.channels, zone.pcm.vec())) return damaged();
        if (checkOnly) std::vector<std::int16_t>().swap(zone.pcm.vec());
        parsed.zones.push_back(std::move(zone));
    }
    if ((juce::uint32) in.readInt() != endMarker) return damaged();
    destination = std::move(parsed);
    return juce::Result::ok();
}
}
