#pragma once

#include "NordPianoDecoder.h"
#include "NordSampleDecoder.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <set>

// Fast-load cache for imported NPNO / NSMP instruments.
//
// The first import decodes the vendor format (slow: block parsing, de-interleaving, loop
// crossfades). The decoded and prepared result is then written once to an app-owned file
// (".cpfc") with the PCM stored raw, so every later load is a sequential read into memory.
// The cache is only an accelerator: the original file is never modified, the cache is keyed
// on path + size + modification time, and any mismatch or damage silently falls back to the
// normal decoder. Cache hits reproduce the in-memory instrument bit for bit.
namespace FastLoadCache
{
constexpr juce::uint32 magic = 0x43504643; // "CPFC"
constexpr juce::uint32 formatVersion = 1;   // bump when the decoder/prepare output changes
constexpr juce::int64 maxCacheBytes = 6LL * 1024 * 1024 * 1024;

enum class Kind : juce::uint32 { piano = 1, sample = 2 };

inline bool disabled()
{
    return juce::SystemStats::getEnvironmentVariable("CLASSIC_PLAYER_NO_FAST_CACHE", {}).isNotEmpty();
}

inline juce::File directory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Classic Player").getChildFile("FastCache");
}

inline juce::String sourceKey(const juce::File& source, Kind kind)
{
    return source.getFullPathName() + "|" + juce::String(source.getSize()) + "|"
         + juce::String(source.getLastModificationTime().toMilliseconds()) + "|k"
         + juce::String((int) kind) + "|v" + juce::String((int) formatVersion);
}

inline juce::File cacheFile(const juce::String& key)
{
    return directory().getChildFile(juce::String::toHexString(key.hashCode64()) + juce::String::toHexString(key.length() * 7919 + (juce::int64) key.hashCode()) + ".cpfc");
}

inline std::mutex& inFlightMutex() { static std::mutex m; return m; }
inline std::set<juce::String>& inFlightKeys() { static std::set<juce::String> s; return s; }

// ---- raw helpers -------------------------------------------------------------------------
inline void writeString(juce::OutputStream& out, const juce::String& text)
{
    const auto utf8 = text.toStdString();
    out.writeInt((int) utf8.size());
    out.write(utf8.data(), utf8.size());
}

inline bool readString(juce::InputStream& in, juce::String& text)
{
    const auto size = in.readInt();
    if (size < 0 || size > 4096 || in.getNumBytesRemaining() < size) return false;
    std::string buffer((size_t) size, '\0');
    if (size > 0 && in.read(buffer.data(), size) != size) return false;
    text = juce::String::fromUTF8(buffer.data(), size);
    return true;
}

// juce::InputStream::read takes an int size; read huge blocks in 1 GiB-safe chunks.
inline bool readBytes(juce::InputStream& in, void* destination, juce::uint64 bytes)
{
    auto* out = static_cast<char*>(destination);
    while (bytes > 0)
    {
        const auto chunk = (int) std::min<juce::uint64>(bytes, 64ull * 1024 * 1024);
        if (in.read(out, chunk) != chunk) return false;
        out += chunk;
        bytes -= (juce::uint64) chunk;
    }
    return true;
}

inline void writeBytes(juce::OutputStream& out, const void* data, juce::uint64 bytes)
{
    const auto* in = static_cast<const char*>(data);
    while (bytes > 0)
    {
        const auto chunk = (size_t) std::min<juce::uint64>(bytes, 64ull * 1024 * 1024);
        out.write(in, chunk);
        in += chunk;
        bytes -= (juce::uint64) chunk;
    }
}

template <typename T>
inline bool loadVector(juce::InputStream& in, std::vector<T>& values, juce::uint64& budget)
{
    const auto count = (juce::uint64) in.readInt64();
    if (count > (juce::uint64) in.getNumBytesRemaining() / sizeof(T)) return false;
    const auto bytes = count * sizeof(T);
    if (bytes > budget) return false;
    budget -= bytes;
    values.resize((size_t) count);
    return bytes == 0 || readBytes(in, values.data(), bytes);
}

template <typename T>
inline void saveVector(juce::OutputStream& out, const std::vector<T>& values)
{
    out.writeInt64((juce::int64) values.size());
    writeBytes(out, values.data(), (juce::uint64) values.size() * sizeof(T));
}

inline void touch(const juce::File& file)
{
    file.setLastModificationTime(juce::Time::getCurrentTime());
}

inline void prune()
{
    auto files = directory().findChildFiles(juce::File::findFiles, false, "*.cpfc");
    juce::int64 total = 0;
    for (const auto& f : files) total += f.getSize();
    if (total <= maxCacheBytes) return;
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getLastModificationTime() < b.getLastModificationTime(); });
    for (const auto& f : files)
    {
        if (total <= maxCacheBytes * 3 / 4) break;
        total -= f.getSize();
        f.deleteFile();
    }
}

// Writes on a background thread through a temporary file, then renames atomically.
template <typename Writer>
inline void storeAsync(const juce::File& source, Kind kind, Writer writer)
{
    if (disabled()) return;
    const auto key = sourceKey(source, kind);
    {
        const std::lock_guard<std::mutex> guard(inFlightMutex());
        if (!inFlightKeys().insert(key).second) return;
    }
    juce::Thread::launch([key, writer = std::move(writer), kind]
    {
        const auto finish = [&] { const std::lock_guard<std::mutex> guard(inFlightMutex()); inFlightKeys().erase(key); };
        auto target = cacheFile(key);
        auto folder = directory();
        if (!folder.createDirectory().wasOk()) { finish(); return; }
        const auto temp = folder.getChildFile(target.getFileNameWithoutExtension() + ".tmp");
        bool ok = false;
        {
            juce::FileOutputStream out(temp);
            if (out.openedOk())
            {
                out.setPosition(0);
                out.truncate();
                out.writeInt((int) magic);
                out.writeInt((int) formatVersion);
                out.writeInt((int) kind);
                writeString(out, key);
                writer(out);
                out.flush();
                ok = out.getStatus().wasOk();
            }
        }
        if (ok) { target.deleteFile(); ok = temp.moveFileTo(target); }
        if (!ok) temp.deleteFile();
        else prune();
        finish();
    });
}

inline bool openChecked(const juce::File& source, Kind kind, juce::FileInputStream& in)
{
    if (disabled() || !in.openedOk()) return false;
    if ((juce::uint32) in.readInt() != magic || (juce::uint32) in.readInt() != formatVersion
        || (juce::uint32) in.readInt() != (juce::uint32) kind)
        return false;
    juce::String storedKey;
    return readString(in, storedKey) && storedKey == sourceKey(source, kind);
}

// ---- NPNO piano --------------------------------------------------------------------------
inline void storePiano(const juce::File& source, std::shared_ptr<NordPianoDecoder::Instrument> instrument)
{
    storeAsync(source, Kind::piano, [instrument](juce::OutputStream& out)
    {
        writeString(out, instrument->name);
        writeString(out, instrument->voicing);
        out.writeInt((int) instrument->streamVersion);
        out.writeInt(instrument->channels);
        out.writeInt(instrument->gainTenthsDb);
        out.writeInt(instrument->signedTenthsDbTrim ? 1 : 0);
        out.writeInt(instrument->velocityScale);
        out.write(instrument->keyMap.data(), instrument->keyMap.size());
        out.write(instrument->fineTune.data(), instrument->fineTune.size());
        out.writeInt((int) instrument->strokes.size());
        for (const auto& stroke : instrument->strokes)
        {
            out.writeInt((int) stroke.rootNote);
            out.writeInt((int) stroke.bank);
            out.writeInt((int) stroke.velocityLayer);
            out.writeInt((int) stroke.trimDb);
            out.writeInt((int) stroke.loopStartFrame);
            out.writeInt((int) stroke.loopEndFrame);
            saveVector(out, stroke.interleavedPcm);
        }
        out.writeInt((int) 0x454E4421); // end marker "END!"
    });
}

inline bool loadPiano(const juce::File& source, NordPianoDecoder::Instrument& instrument,
                      juce::uint64 maxDecodedBytes)
{
    juce::FileInputStream in(cacheFile(sourceKey(source, Kind::piano)));
    if (!openChecked(source, Kind::piano, in)) return false;
    NordPianoDecoder::Instrument loaded;
    juce::uint64 budget = maxDecodedBytes;
    if (!readString(in, loaded.name) || !readString(in, loaded.voicing)) return false;
    loaded.streamVersion = (std::uint16_t) in.readInt();
    loaded.channels = in.readInt();
    loaded.gainTenthsDb = in.readInt();
    loaded.signedTenthsDbTrim = in.readInt() != 0;
    loaded.velocityScale = in.readInt();
    if (!readBytes(in, loaded.keyMap.data(), loaded.keyMap.size())
        || !readBytes(in, loaded.fineTune.data(), loaded.fineTune.size())) return false;
    const auto strokeCount = in.readInt();
    if (strokeCount < 0 || strokeCount > 100000) return false;
    loaded.strokes.resize((size_t) strokeCount);
    for (auto& stroke : loaded.strokes)
    {
        stroke.rootNote = (std::uint8_t) in.readInt();
        stroke.bank = (std::uint8_t) in.readInt();
        stroke.velocityLayer = (std::uint8_t) in.readInt();
        stroke.trimDb = (std::uint16_t) in.readInt();
        stroke.loopStartFrame = (std::uint32_t) in.readInt();
        stroke.loopEndFrame = (std::uint32_t) in.readInt();
        if (!loadVector(in, stroke.interleavedPcm, budget)) return false;
    }
    if ((juce::uint32) in.readInt() != 0x454E4421u) return false;
    touch(cacheFile(sourceKey(source, Kind::piano)));
    instrument = std::move(loaded);
    return true;
}

// ---- NSMP samples ------------------------------------------------------------------------
inline void storeSample(const juce::File& source, std::shared_ptr<NordSampleDecoder::Instrument> instrument)
{
    storeAsync(source, Kind::sample, [instrument](juce::OutputStream& out)
    {
        writeString(out, instrument->name);
        out.write(instrument->gain.data(), instrument->gain.size() * sizeof(float));
        out.write(instrument->detune.data(), instrument->detune.size() * sizeof(float));
        out.writeInt((int) instrument->zones.size());
        for (const auto& zone : instrument->zones)
        {
            out.writeInt(zone.root); out.writeInt(zone.low); out.writeInt(zone.high);
            out.writeInt(zone.velocityLow); out.writeInt(zone.velocityHigh); out.writeInt(zone.channels);
            out.write(&zone.gain, sizeof(float));
            out.writeInt64((juce::int64) zone.secondStart);
            out.writeInt64((juce::int64) zone.loopStart);
            out.writeInt64((juce::int64) zone.loopEnd);
            out.writeInt64((juce::int64) zone.clippedSamples);
            saveVector(out, zone.pcm);
        }
        out.writeInt((int) 0x454E4421);
    });
}

inline bool loadSample(const juce::File& source, NordSampleDecoder::Instrument& instrument,
                       juce::uint64 maxDecodedBytes)
{
    juce::FileInputStream in(cacheFile(sourceKey(source, Kind::sample)));
    if (!openChecked(source, Kind::sample, in)) return false;
    NordSampleDecoder::Instrument loaded;
    juce::uint64 budget = maxDecodedBytes;
    if (!readString(in, loaded.name)
        || !readBytes(in, loaded.gain.data(), loaded.gain.size() * sizeof(float))
        || !readBytes(in, loaded.detune.data(), loaded.detune.size() * sizeof(float))) return false;
    const auto zoneCount = in.readInt();
    if (zoneCount < 0 || zoneCount > 1000000) return false;
    loaded.zones.resize((size_t) zoneCount);
    for (auto& zone : loaded.zones)
    {
        zone.root = in.readInt(); zone.low = in.readInt(); zone.high = in.readInt();
        zone.velocityLow = in.readInt(); zone.velocityHigh = in.readInt(); zone.channels = in.readInt();
        if (zone.channels < 1 || zone.channels > 2) return false;
        if (!readBytes(in, &zone.gain, sizeof(float))) return false;
        zone.secondStart = (size_t) in.readInt64();
        zone.loopStart = (size_t) in.readInt64();
        zone.loopEnd = (size_t) in.readInt64();
        zone.clippedSamples = (size_t) in.readInt64();
        if (!loadVector(in, zone.pcm, budget)) return false;
    }
    if ((juce::uint32) in.readInt() != 0x454E4421u) return false;
    touch(cacheFile(sourceKey(source, Kind::sample)));
    instrument = std::move(loaded);
    return true;
}
}
