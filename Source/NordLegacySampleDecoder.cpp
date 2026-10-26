#include "NordLegacySampleDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
struct LegacyStroke
{
    std::uint8_t rootNote = 60, zoneLowNote = 0, zoneHighNote = 127;
    std::uint8_t velocityLow = 0, velocityHigh = 127, sampleChannels = 0;
    float zoneGain = 1.0f;
    std::uint32_t loopStartFrame = 0, loopEndFrame = 0;
    bool loopOnRelease = false;
    std::vector<std::int16_t> interleavedPcm;
};

struct LegacyInstrument
{
    juce::String name;
    std::uint16_t streamVersion = 0;
    int channels = 0;
    bool usesZones = false;
    float instrumentGain = 1.0f;
    std::array<float, 128> keyGain {}, keyTuneSemitones {};
    std::array<std::uint8_t, 128> keyMap {};
    std::vector<LegacyStroke> strokes;
};

using Bytes = std::uint8_t;
constexpr std::size_t maxFileBytes = 512u * 1024u * 1024u;
constexpr std::uint64_t bytesPerMiB = 1024ull * 1024ull;
constexpr std::uint64_t maxDecodedBytes = 512ull * bytesPerMiB;
constexpr std::uint32_t gainUnity = 1u << 20;
constexpr std::size_t pointerWrapWords = 1u << 16;

struct Section
{
    std::array<char, 4> tag {};
    std::uint32_t version = 0;
    std::size_t payloadAt = 0; // Offset from the beginning of the CBIN body.
    std::size_t payloadBytes = 0;

    bool is(const char* value) const
    {
        return std::memcmp(tag.data(), value, 4) == 0;
    }
};

struct StrokeSection
{
    const Section* section = nullptr;
    std::uint32_t id = 0;
    std::uint8_t root = 60;
};

struct Record
{
    std::size_t wordAt = 0;
    std::size_t firstField = 0;
    std::size_t count = 0;
    std::size_t spanWords = 0;
    std::uint8_t width = 0;
    std::uint8_t order = 0;
    bool oneToOne = false;
};

struct StreamPlan
{
    std::vector<Record> records;
    std::size_t fields = 0;
    std::size_t loopStart = 0;
    int channels = 0;
};

struct Zone
{
    std::uint32_t strokeId = 0;
    std::uint8_t root = 60;
    std::uint8_t low = 0;
    std::uint8_t high = 127;
    std::uint8_t velocityLow = 0;
    std::uint8_t velocityHigh = 127;
    float gain = 1.0f;
};

juce::Result fail(juce::String message)
{
    return juce::Result::fail(std::move(message));
}

std::uint16_t readBe16(const Bytes* data)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t readBe24(const Bytes* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 16)
         | (static_cast<std::uint32_t>(data[1]) << 8) | data[2];
}

std::uint32_t readBe32(const Bytes* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24)
         | (static_cast<std::uint32_t>(data[1]) << 16)
         | (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::uint32_t readLe32(const Bytes* data)
{
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::int32_t signed24(const Bytes* data)
{
    auto value = static_cast<std::int32_t>(readBe24(data));
    if ((value & 0x800000) != 0) value -= 0x1000000;
    return value;
}

std::uint32_t crc32IsoHdlc(const Bytes* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

std::uint16_t crc16CcittFalse(const Bytes* data, std::size_t size)
{
    std::uint16_t crc = 0xffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
    }
    return crc;
}

juce::String asciiField(const Bytes* data, std::size_t size)
{
    std::size_t length = 0;
    while (length < size && data[length] != 0) ++length;
    return juce::String::fromUTF8(reinterpret_cast<const char*>(data), static_cast<int>(length)).trim();
}

bool supportedTag(const std::array<char, 4>& tag, bool wide)
{
    if (wide)
        return tag == std::array<char, 4>{'N','S','M','P'}
            || tag == std::array<char, 4>{'\0','h','d','r'}
            || tag == std::array<char, 4>{'\0','c','a','t'}
            || tag == std::array<char, 4>{'\0','m','a','p'}
            || tag == std::array<char, 4>{'\0','s','t','k'}
            || tag == std::array<char, 4>{'\0','s','t','y'}
            || tag == std::array<char, 4>{'m','e','t','a'};
    return tag == std::array<char, 4>{'N','W','S','\0'}
        || tag == std::array<char, 4>{'h','d','r','\0'}
        || tag == std::array<char, 4>{'c','a','t','\0'}
        || tag == std::array<char, 4>{'m','a','p','\0'}
        || tag == std::array<char, 4>{'s','t','k','\0'}
        || tag == std::array<char, 4>{'s','t','y','\0'};
}

juce::Result parseSections(const Bytes* body, std::size_t bodyBytes, bool wide,
                           std::vector<Section>& sections)
{
    const auto headerBytes = wide ? std::size_t{12} : std::size_t{9};
    std::size_t at = 0;
    while (at < bodyBytes)
    {
        if (bodyBytes - at < headerBytes)
            return fail("A cadeia de seções Nord está truncada.");

        Section section;
        std::memcpy(section.tag.data(), body + at, 4);
        if (!supportedTag(section.tag, wide))
            return fail("O arquivo Nord contém uma seção desconhecida; ele não foi alterado.");

        if (wide)
        {
            section.version = readBe32(body + at + 4);
            const auto length = readBe32(body + at + 8);
            if (length > bodyBytes - at - headerBytes)
                return fail("O tamanho de uma seção Nord excede o arquivo.");
            section.payloadAt = at + headerBytes;
            section.payloadBytes = length;
        }
        else
        {
            if (body[at + 3] != 0)
                return fail("O preenchimento de uma seção NSMP2 é inválido.");
            section.version = body[at + 4];
            const auto length = readBe32(body + at + 5);
            if (length > bodyBytes - at - headerBytes)
                return fail("O tamanho de uma seção Nord excede o arquivo.");
            section.payloadAt = at + headerBytes;
            section.payloadBytes = length;
        }
        const auto next = section.payloadAt + section.payloadBytes;
        if (next < at || next > bodyBytes)
            return fail("O arquivo Nord contém deslocamentos de seção inválidos.");
        sections.push_back(section);
        at = next;
        if (at == bodyBytes) break;
    }

    if (sections.empty() || sections.front().payloadBytes != (wide ? 4u : 0u)
        || !sections.front().is(wide ? "NSMP" : "NWS\0"))
        return fail("O arquivo não começa com uma cadeia de seções Nord reconhecida.");
    return juce::Result::ok();
}

const Section* findSection(const std::vector<Section>& sections, const char* tag)
{
    const auto found = std::find_if(sections.begin(), sections.end(), [tag](const auto& section)
    {
        return section.is(tag);
    });
    return found == sections.end() ? nullptr : &*found;
}

std::vector<StrokeSection> collectStrokes(const Bytes* body, const std::vector<Section>& sections)
{
    std::vector<StrokeSection> result;
    for (const auto& section : sections)
    {
        if (!section.is("stk\0") && !section.is("\0stk")) continue;
        if (section.payloadBytes < 6) return {};
        const auto* payload = body + section.payloadAt;
        result.push_back({ &section, readBe32(payload), payload[5] });
    }
    return result;
}

const StrokeSection* findStroke(const std::vector<StrokeSection>& strokes, std::uint32_t id,
                                bool narrow)
{
    const auto found = std::find_if(strokes.begin(), strokes.end(), [=](const auto& stroke)
    {
        return narrow ? static_cast<std::uint8_t>(stroke.id) == static_cast<std::uint8_t>(id)
                      : stroke.id == id;
    });
    return found == strokes.end() ? nullptr : &*found;
}

bool validZoneTarget(const Bytes* body, const std::vector<StrokeSection>& strokes,
                     std::uint32_t id, std::uint8_t root, bool narrow)
{
    const auto* stroke = findStroke(strokes, id, narrow);
    return stroke != nullptr && stroke->root == root
        && body[stroke->section->payloadAt + 5] == root;
}

bool parseNarrowZones(const Bytes* body, const Section& map,
                      const std::vector<StrokeSection>& strokes,
                      std::vector<Zone>& zones, float& instrumentGain,
                      std::array<float, 128>& keyGain,
                      std::array<float, 128>& keyTune)
{
    const auto* data = body + map.payloadAt;
    if (map.payloadBytes < 786 || data[785] == 0 || data[785] != strokes.size()) return false;
    const auto width = map.version == 9 ? std::size_t{12} : map.version == 10 ? std::size_t{15} : 0;
    const auto count = static_cast<std::size_t>(data[785]);
    if (width == 0 || count > (map.payloadBytes - 786) / width) return false;

    constexpr std::size_t keyTableAt = 15;
    const auto globalGain = readBe24(data) / static_cast<float>(gainUnity);
    const auto globalTune = signed24(data + 3) / 256.0f;
    instrumentGain = juce::jlimit(0.0f, 16.0f, globalGain);
    for (std::size_t note = 0; note < 128; ++note)
    {
        const auto* key = data + keyTableAt + note * 6;
        keyGain[note] = juce::jlimit(0.0f, 16.0f,
            readBe24(key) / static_cast<float>(gainUnity));
        keyTune[note] = globalTune + signed24(key + 3) / 256.0f;
    }

    zones.clear();
    zones.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto* record = data + 786 + i * width;
        const auto id = static_cast<std::uint32_t>(record[2]);
        const auto top = record[9];
        const auto* target = findStroke(strokes, id, true);
        if (target == nullptr || target->root > 127 || top > 127
            || !validZoneTarget(body, strokes, id, target->root, true))
            return false;
        const auto linearGain = readBe24(record + 3) / static_cast<float>(gainUnity);
        zones.push_back({ id, target->root, 0, top, 0, 127,
                          juce::jlimit(0.0f, 32.0f, linearGain) });
    }

    // NSMP2 stores the upper edge of each keyboard zone, ordered high to low.
    std::stable_sort(zones.begin(), zones.end(), [](const Zone& a, const Zone& b)
    {
        return a.high < b.high;
    });
    for (std::size_t i = 0; i < zones.size(); ++i)
    {
        zones[i].low = i == 0 ? 0 : static_cast<std::uint8_t>(zones[i - 1].high + 1);
        if (zones[i].low > zones[i].high) return false;
    }
    return true;
}

bool parseWideZones(const Bytes* body, const Section& map,
                    const std::vector<StrokeSection>& strokes,
                    std::vector<Zone>& zones)
{
    const auto* data = body + map.payloadAt;
    const auto count = strokes.size();
    std::size_t tableAt = 0;
    std::size_t width = 0;
    bool storesLow = false;
    std::size_t idAt = 0;

    if (map.version == 12)
    {
        width = 11;
        idAt = 5;
        bool found = false;
        // V12 zones tile and the map's short trailer varies by a few bytes.
        for (std::size_t tail = 0; tail <= 8 && !found; ++tail)
        {
            if (count > (map.payloadBytes > tail ? map.payloadBytes - tail : 0) / width) continue;
            const auto candidate = map.payloadBytes - tail - count * width;
            if (candidate == 0 || data[candidate - 1] != count) continue;
            bool matches = true;
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto* record = data + candidate + i * width;
                const auto id = readBe32(record + idAt);
                if (record[0] > 127 || record[1] > 127
                    || !validZoneTarget(body, strokes, id, record[0], false))
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                tableAt = candidate;
                found = true;
            }
        }
        if (!found) return false;
    }
    else if (map.version == 14)
    {
        width = 16;
        idAt = 8;
        storesLow = true;
        constexpr std::size_t countAt = 774;
        tableAt = countAt + 1;
        if (map.payloadBytes < tableAt || data[countAt] != count
            || count > (map.payloadBytes - tableAt) / width)
            return false;
    }
    else
        return false; // V21 belongs to NSMP4.

    zones.clear();
    zones.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto* record = data + tableAt + i * width;
        const auto root = record[0];
        const auto top = record[1];
        const auto low = static_cast<std::uint8_t>(storesLow ? record[2] : 0);
        const auto id = readBe32(record + idAt);
        if (root > 127 || top > 127 || low > top
            || !validZoneTarget(body, strokes, id, root, false))
            return false;
        const auto* stroke = findStroke(strokes, id, false);
        const auto* payload = body + stroke->section->payloadAt;
        if (stroke->section->payloadBytes < 61) return false;
        float gainDb = 0.0f;
        // CBIN stores this IEEE-754 field big endian; convert its bits on little-endian hosts.
        const auto gainBits = readBe32(payload + 57);
        std::memcpy(&gainDb, &gainBits, sizeof(gainDb));
        if (!std::isfinite(gainDb)) return false;
        const auto gain = std::pow(10.0f, juce::jlimit(-120.0f, 24.0f, gainDb) / 20.0f);
        zones.push_back({ id, root, low, top,
                          static_cast<std::uint8_t>(storesLow ? record[14] : 0),
                          static_cast<std::uint8_t>(storesLow ? record[15] : 127),
                          juce::jlimit(0.0f, 32.0f, gain) });
    }

    if (!storesLow)
    {
        std::stable_sort(zones.begin(), zones.end(), [](const Zone& a, const Zone& b)
        {
            return a.high < b.high;
        });
        for (std::size_t i = 0; i < zones.size(); ++i)
        {
            zones[i].low = i == 0 ? 0 : static_cast<std::uint8_t>(zones[i - 1].high + 1);
            if (zones[i].low > zones[i].high) return false;
        }
    }
    return true;
}

std::uint32_t readWord(const Bytes* stream, std::size_t index, std::size_t wordBytes)
{
    const auto* word = stream + index * wordBytes;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < wordBytes; ++i) value = (value << 8) | word[i];
    return value;
}

struct RecordHead
{
    bool oneToOne = false;
    std::uint8_t width = 0;
    bool reserved = false;
    std::uint8_t order = 0;
    std::size_t count = 0;
};

RecordHead readRecordHead(std::uint32_t raw)
{
    return { ((raw >> 23) & 1u) != 0,
             static_cast<std::uint8_t>(((raw >> 19) & 0x0fu) + 1),
             ((raw >> 17) & 1u) != 0,
             static_cast<std::uint8_t>((raw >> 14) & 0x07u),
             static_cast<std::size_t>(raw & 0x3fffu) };
}

bool isTerminator(std::uint32_t raw, std::size_t cell, int& channels)
{
    const auto header = readRecordHead(raw);
    const bool common = (raw >> 24) == 0 && header.oneToOne && header.width == 1
                     && ((raw >> 18) & 1u) == 0 && !header.reserved && header.order == 0;
    if (common && header.count == cell) { channels = 1; return true; }
    if (common && header.count == 2 * cell) { channels = 2; return true; }
    return false;
}

std::size_t resolvePointer(std::uint16_t pointer, std::size_t strokeAt,
                           std::size_t headerBytes, std::size_t wordBytes)
{
    const auto base = ((strokeAt + headerBytes) / wordBytes) % pointerWrapWords;
    return (static_cast<std::size_t>(pointer) + pointerWrapWords - base) % pointerWrapWords;
}

juce::Result scanStream(const Bytes* body, const Section& section, std::size_t payloadBytes,
                        std::size_t wordBytes, std::size_t headerBytes, std::size_t cell,
                        StreamPlan& result)
{
    result = {};
    if (payloadBytes < headerBytes || section.payloadBytes < headerBytes + wordBytes)
        return fail("Uma zona Nord contém um cabeçalho de áudio incompleto.");
    const auto* payload = body + section.payloadAt;
    const auto streamBytes = section.payloadBytes - headerBytes;
    if (streamBytes % wordBytes != 0)
        return fail("O fluxo de áudio Nord termina no meio de uma palavra.");
    const auto wordCount = streamBytes / wordBytes;
    if (wordCount == 0) return fail("O fluxo de áudio Nord está vazio.");

    const auto directoryAt = std::size_t{20};
    // Four directory entries are 9 bytes apart, but the fourth entry begins at
    // index 3; only its 2-byte pointer must fit inside the 51-byte v2 header.
    if (headerBytes < directoryAt + 3 * 9 + 2)
        return fail("O diretório de áudio Nord está truncado.");
    const auto pointerAt = [=](std::size_t index)
    {
        return readBe16(payload + directoryAt + index * 9);
    };
    const auto first = resolvePointer(pointerAt(0), section.payloadAt, headerBytes, wordBytes);
    auto last = resolvePointer(pointerAt(3), section.payloadAt, headerBytes, wordBytes);
    while (last + pointerWrapWords < wordCount) last += pointerWrapWords;
    if (first >= wordCount || last >= wordCount || first >= last)
        return fail("Os ponteiros do diretório Nord não correspondem ao fluxo de áudio.");

    int channels = 0;
    if (!isTerminator(readWord(payload + headerBytes, last, wordBytes), cell, channels))
        return fail("O fim da gravação Nord não possui um terminador reconhecido.");
    result.channels = channels;

    const auto marked = resolvePointer(pointerAt(2), section.payloadAt, headerBytes, wordBytes);
    auto wordAt = first;
    std::size_t fields = 0;
    while (wordAt < last)
    {
        auto raw = readWord(payload + headerBytes, wordAt, wordBytes);
        if (raw == 0)
        {
            ++wordAt;
            continue;
        }
        const auto head = readRecordHead(raw);
        if ((raw >> 24) != 0 || head.reserved || head.order > 4 || head.count == 0
            || (channels == 2 && head.count % 2 != 0)
            || (!head.oneToOne && head.count % cell != 0))
            return fail("Uma palavra do fluxo Nord usa um registro não suportado.");

        const auto spanBits = wordBytes * 8 + head.count * static_cast<std::size_t>(head.width);
        const auto spanWords = (spanBits + wordBytes * 8 - 1) / (wordBytes * 8);
        if (spanWords == 0 || wordAt + spanWords > last
            || fields > std::numeric_limits<std::size_t>::max() - head.count)
            return fail("Um registro de áudio Nord ultrapassa os limites da zona.");
        result.records.push_back({ wordAt, fields, head.count, spanWords,
                                   head.width, head.order, head.oneToOne });
        fields += head.count;
        wordAt += spanWords;
    }

    result.fields = fields;
    result.loopStart = fields;
    // NSMP2 stores this pointer as 16-bit word position even when a stroke's
    // compressed stream is longer than 65536 words. Match it modulo that wrap
    // against block starts; otherwise loops after the first 64K words are
    // mistaken for one-shot samples and their notes end at the PCM boundary.
    if (marked != last % pointerWrapWords)
    {
        const auto found = std::find_if(result.records.begin(), result.records.end(),
            [marked](const Record& record)
            {
                return record.oneToOne && record.wordAt >= marked
                    && (record.wordAt - marked) % pointerWrapWords == 0;
            });
        if (found != result.records.end()) result.loopStart = found->firstField;
    }
    if (fields == 0 || fields % static_cast<std::size_t>(channels) != 0)
        return fail("A duração da zona Nord é inválida.");
    (void) payloadBytes;
    return juce::Result::ok();
}

std::int32_t readSignedField(const Bytes* stream, std::size_t bitAt, std::uint8_t width)
{
    std::uint32_t value = 0;
    for (std::size_t bit = 0; bit < width; ++bit)
    {
        const auto index = bitAt + bit;
        value = (value << 1) | ((stream[index / 8] >> (7 - index % 8)) & 1u);
    }
    const auto sign = std::uint32_t{1} << (width - 1);
    return (value & sign) != 0
        ? static_cast<std::int32_t>(value) - static_cast<std::int32_t>(std::uint32_t{1} << width)
        : static_cast<std::int32_t>(value);
}

std::int64_t predict(std::int32_t residual, std::array<std::int64_t, 4>& history,
                     unsigned order)
{
    static constexpr std::int32_t coefficients[5][4] = {
        { 0, 0, 0, 0 }, { -1, 0, 0, 0 }, { -2, 1, 0, 0 },
        { -3, 3, -1, 0 }, { -4, 6, -4, 1 }
    };
    std::int64_t predicted = 0;
    for (unsigned i = 0; i < order; ++i)
        predicted += static_cast<std::int64_t>(coefficients[order][i]) * history[i];
    const auto value = static_cast<std::int64_t>(residual) - predicted;
    for (std::size_t i = history.size() - 1; i > 0; --i) history[i] = history[i - 1];
    history[0] = value;
    return value;
}

juce::Result decodeStream(const Bytes* body, const Section& section, std::size_t headerBytes,
                          std::size_t wordBytes, std::size_t cell, bool signedPeak,
                          const StreamPlan& plan, LegacyStroke& output)
{
    const auto* payload = body + section.payloadAt;
    const auto peakMagnitude = std::max<std::uint32_t>(1, readBe24(payload + 13));
    const auto peak = signedPeak && peakMagnitude >= (1u << 23)
        ? (1u << 24) - peakMagnitude : peakMagnitude;
    std::uint32_t bitCount = 0;
    for (auto value = peak; value != 0; value >>= 1) ++bitCount;
    const auto exactPower = (peak & (peak - 1)) == 0 ? 1 : 0;
    const auto shift = static_cast<int>(payload[12]) + static_cast<int>(bitCount) - 22 - exactPower;
    if (shift < -32 || shift > 32)
        return fail("O fator de escala do áudio Nord está fora do limite seguro.");

    const auto samples = plan.fields;
    if (samples > std::numeric_limits<std::size_t>::max() / sizeof(std::int16_t))
        return fail("Uma zona Nord excede o limite de memória.");
    output.interleavedPcm.resize(samples);
    output.sampleChannels = static_cast<std::uint8_t>(plan.channels);
    output.loopStartFrame = static_cast<std::uint32_t>(plan.loopStart / plan.channels);
    output.loopEndFrame = static_cast<std::uint32_t>(plan.fields / plan.channels);
    output.loopOnRelease = output.loopStartFrame < output.loopEndFrame;

    const auto* stream = payload + headerBytes;
    const auto wordBits = wordBytes * 8;
    std::array<std::array<std::int64_t, 4>, 2> histories {};
    for (const auto& record : plan.records)
    {
        const auto count = record.count;
        const auto perChannel = count / static_cast<std::size_t>(plan.channels);
        const auto baseBit = (record.wordAt + 1) * wordBits;
        for (std::size_t k = 0; k < count; ++k)
        {
            std::size_t fieldIndex = k;
            if (plan.channels == 2)
                fieldIndex = 2 * (k % perChannel) + k / perChannel;
            const auto residual = readSignedField(stream,
                baseBit + fieldIndex * record.width, record.width);
            const auto channel = k / perChannel;
            const auto withinChannel = k % perChannel;
            const auto order = record.oneToOne ? 0u : static_cast<unsigned>(record.order);
            const auto reconstructed = predict(residual, histories[channel], order);
            const auto scaled = shift >= 0
                ? reconstructed * (std::int64_t{1} << shift)
                : reconstructed >> -shift;
            const auto sample = std::clamp<std::int64_t>(scaled, -32768, 32767);
            const auto frame = record.firstField / static_cast<std::size_t>(plan.channels)
                             + withinChannel;
            output.interleavedPcm[frame * static_cast<std::size_t>(plan.channels) + channel]
                = static_cast<std::int16_t>(sample);
        }
    }
    (void) cell;
    return juce::Result::ok();
}

std::uint64_t decodedBudget(std::size_t sourceBytes)
{
    const auto memoryMiB = juce::SystemStats::getMemorySizeInMegabytes();
    const auto totalBytes = memoryMiB > 0
        ? static_cast<std::uint64_t>(memoryMiB) * bytesPerMiB : 4ull * 1024ull * bytesPerMiB;
    const auto totalBudget = std::min(maxDecodedBytes, totalBytes / 3);
    return totalBudget > sourceBytes ? totalBudget - sourceBytes : 0;
}

} // namespace

juce::Result NordLegacySampleDecoder::decodeFile(const juce::File& file,
                                                 NordSampleDecoder::Instrument& destination)
{
    destination = {};
    if (!file.existsAsFile()) return fail("Arquivo Nord Sample não encontrado.");
    if (!file.hasFileExtension(".nsmp") && !file.hasFileExtension(".nsmp3"))
        return fail("A layer Nord Sample aceita arquivos .nsmp e .nsmp3.");
    if (file.getSize() < 26 || file.getSize() > static_cast<juce::int64>(maxFileBytes))
        return fail("O arquivo Nord Sample é pequeno demais ou excede o limite de 512 MB.");

    juce::MemoryBlock fileData;
    if (!file.loadFileAsData(fileData)) return fail("Não foi possível ler o arquivo Nord Sample.");
    const auto* bytes = static_cast<const Bytes*>(fileData.getData());
    const auto fileBytes = static_cast<std::size_t>(fileData.getSize());
    if (fileBytes < 24 || std::memcmp(bytes, "CBIN", 4) != 0)
        return fail("O arquivo não possui um cabeçalho CBIN válido.");
    if (std::memcmp(bytes + 8, "nsmp", 4) != 0)
        return fail("O conteúdo CBIN não é um instrumento Nord Sample.");

    const auto containerGeneration = readLe32(bytes + 4);
    const auto contentVersion = readLe32(bytes + 20);
    std::size_t bodyAt = 0;
    std::size_t bodyBytes = 0;
    if (containerGeneration == 0)
    {
        if (fileBytes < 26) return fail("O cabeçalho CBIN tipo 0 está truncado.");
        const auto storedCrc = static_cast<std::uint16_t>(bytes[fileBytes - 2]
                               | (static_cast<std::uint16_t>(bytes[fileBytes - 1]) << 8));
        if (crc16CcittFalse(bytes, fileBytes - 2) != storedCrc)
            return fail("A verificação CRC16 do arquivo Nord Sample falhou.");
        bodyAt = 24;
        bodyBytes = fileBytes - bodyAt - 2;
    }
    else if (containerGeneration == 1)
    {
        if (fileBytes < 44) return fail("O cabeçalho CBIN tipo 1 está truncado.");
        bodyAt = 44;
        bodyBytes = fileBytes - bodyAt;
        if (crc32IsoHdlc(bytes + bodyAt, bodyBytes) != readLe32(bytes + 24))
            return fail("A verificação CRC32 do arquivo Nord Sample falhou.");
    }
    else
        return fail("Esta geração do contêiner Nord não é suportada.");

    if (contentVersion >= 400)
        return fail("O arquivo é NSMP4 (.nsmp4); por enquanto esta layer aceita NSMP2 e NSMP3.");
    // Early type-0 NSMP2 instruments use content version 8; later schemas use
    // format-numbered versions such as 300 for NSMP3.
    const auto narrow = contentVersion < 300;
    const auto wide = contentVersion >= 300 && contentVersion < 400;
    if (!narrow && !wide)
        return fail("A versão do conteúdo Nord Sample não é suportada.");
    if (wide != file.hasFileExtension(".nsmp3"))
        return fail("A extensão do arquivo não corresponde à geração Nord indicada no cabeçalho.");

    std::vector<Section> sections;
    const auto sectionResult = parseSections(bytes + bodyAt, bodyBytes, wide, sections);
    if (sectionResult.failed()) return sectionResult;

    const auto* header = findSection(sections, wide ? "\0hdr" : "hdr\0");
    const auto* map = findSection(sections, wide ? "\0map" : "map\0");
    if (header == nullptr || map == nullptr)
        return fail("O instrumento Nord Sample está sem uma seção de nome ou mapa.");

    const auto strokes = collectStrokes(bytes + bodyAt, sections);
    if (strokes.empty() || strokes.size() > 128)
        return fail("O instrumento Nord Sample não tem zonas válidas ou excede 128 zonas.");

    std::vector<Zone> zones;
    float instrumentGain = 1.0f;
    std::array<float, 128> keyGain {};
    keyGain.fill(1.0f);
    std::array<float, 128> keyTune {};
    const auto zonesOk = wide
        ? parseWideZones(bytes + bodyAt, *map, strokes, zones)
        : parseNarrowZones(bytes + bodyAt, *map, strokes, zones,
                           instrumentGain, keyGain, keyTune);
    if (!zonesOk || zones.size() != strokes.size())
        return fail("O mapa de zonas Nord é incompatível, incompleto ou referencia uma amostra ausente.");

    const auto wordBytes = wide ? std::size_t{4} : std::size_t{3};
    const auto audioHeaderBytes = wide ? std::size_t{68} : std::size_t{51};
    const auto cell = wide ? std::size_t{32} : std::size_t{24};
    std::vector<StreamPlan> plans;
    plans.reserve(zones.size());
    std::uint64_t totalDecoded = 0;
    for (const auto& zone : zones)
    {
        const auto* stroke = findStroke(strokes, zone.strokeId, !wide);
        if (stroke == nullptr) return fail("Uma zona do instrumento aponta para uma amostra inexistente.");
        StreamPlan plan;
        auto result = scanStream(bytes + bodyAt, *stroke->section,
                                 stroke->section->payloadBytes, wordBytes,
                                 audioHeaderBytes, cell, plan);
        if (result.failed()) return result;
        const auto decodedBytes = static_cast<std::uint64_t>(plan.fields) * sizeof(std::int16_t);
        if (plan.fields > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || decodedBytes > std::numeric_limits<std::uint64_t>::max() - totalDecoded)
            return fail("Uma amostra Nord excede a duração máxima suportada.");
        totalDecoded += decodedBytes;
        plans.push_back(std::move(plan));
    }
    const auto budget = decodedBudget(fileBytes);
    if (totalDecoded > budget)
    {
        const auto required = (totalDecoded + bytesPerMiB - 1) / bytesPerMiB;
        const auto allowed = budget / bytesPerMiB;
        return fail("Este instrumento Nord precisa de " + juce::String(static_cast<juce::int64>(required))
            + " MiB descompactados; o limite seguro neste computador é "
            + juce::String(static_cast<juce::int64>(allowed)) + " MiB.");
    }

    LegacyInstrument parsed;
    parsed.usesZones = true;
    parsed.streamVersion = static_cast<std::uint16_t>(std::min<std::uint32_t>(contentVersion, 65535));
    parsed.channels = 2; // Individual strokes may be mono or stereo.
    parsed.instrumentGain = instrumentGain;
    parsed.keyGain = keyGain;
    parsed.keyTuneSemitones = keyTune;
    parsed.keyMap.fill(0xff);
    const auto* headerData = bytes + bodyAt + header->payloadAt;
    if (!wide && header->payloadBytes >= 44)
        parsed.name = asciiField(headerData + 12, 32);
    else if (wide && header->payloadBytes > 10)
        parsed.name = asciiField(headerData + 10, std::min<std::size_t>(66, header->payloadBytes - 10));
    if (parsed.name.isEmpty()) parsed.name = file.getFileNameWithoutExtension();

    parsed.strokes.reserve(zones.size());
    for (std::size_t i = 0; i < zones.size(); ++i)
    {
        const auto& zone = zones[i];
        const auto* stroke = findStroke(strokes, zone.strokeId, !wide);
        LegacyStroke decoded;
        decoded.rootNote = zone.root;
        decoded.zoneLowNote = zone.low;
        decoded.zoneHighNote = zone.high;
        decoded.velocityLow = zone.velocityLow;
        decoded.velocityHigh = zone.velocityHigh;
        decoded.zoneGain = juce::jlimit(0.0f, 32.0f, zone.gain);
        auto result = decodeStream(bytes + bodyAt, *stroke->section,
                                   audioHeaderBytes, wordBytes, cell, wide,
                                   plans[i], decoded);
        if (result.failed()) return result;
        parsed.strokes.push_back(std::move(decoded));
    }

    NordSampleDecoder::Instrument converted;
    converted.name = parsed.name;
    converted.zones.reserve(parsed.strokes.size());
    for (std::size_t note = 0; note < 128; ++note)
    {
        converted.gain[note] = parsed.instrumentGain * parsed.keyGain[note];
        converted.detune[note] = parsed.keyTuneSemitones[note] * 100.0f;
    }
    for (const auto& stroke : parsed.strokes)
    {
        NordSampleDecoder::Zone zone;
        zone.root = stroke.rootNote;
        zone.low = stroke.zoneLowNote;
        zone.high = stroke.zoneHighNote;
        zone.velocityLow = stroke.velocityLow;
        zone.velocityHigh = stroke.velocityHigh;
        zone.channels = stroke.sampleChannels;
        zone.gain = stroke.zoneGain;
        zone.loopStart = stroke.loopStartFrame;
        zone.loopEnd = stroke.loopEndFrame;
        zone.pcm.reserve(stroke.interleavedPcm.size());
        for (const auto sample : stroke.interleavedPcm)
            zone.pcm.push_back(static_cast<float>(sample) / 32768.0f);
        converted.zones.push_back(std::move(zone));
    }
    destination = std::move(converted);
    return juce::Result::ok();
}
