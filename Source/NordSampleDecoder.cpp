// NSMP4 stroke codec adapted from nord-format 0.8.0, John Moore, BSD-3-Clause.
// See THIRD_PARTY_NORD_SAMPLE_LICENSE.txt. Validate with real instruments.
#include "NordSampleDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace
{
using Byte = std::uint8_t;
std::uint32_t be(const Byte* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
         | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint32_t le(const Byte* p)
{
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16)
         | (std::uint32_t(p[1]) << 8) | p[0];
}
int u16(const Byte* p) { return int(p[0]) * 256 + p[1]; }
int u24(const Byte* p) { return int(p[0]) * 65536 + int(p[1]) * 256 + p[2]; }
int s24(const Byte* p) { const auto value = u24(p); return (value & 0x800000) ? value - 0x1000000 : value; }
float f32(const Byte* p)
{
    const auto bits = be(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

std::uint32_t crc(const Byte* bytes, size_t length)
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

struct Section
{
    const Byte* data = nullptr;
    size_t length = 0, at = 0;
    std::uint32_t version = 0;
};

void decodeStroke(const Section& section, NordSampleDecoder::Zone& zone)
{
    require(section.length >= 72 && (section.length - 68) % 4 == 0, "NSMP4 stroke truncado.");
    const auto* stream = section.data + 68;
    const auto words = (section.length - 68) / 4;
    const auto base = ((section.at + 68) / 4) % 65536;
    const auto pointer = [&](int offset)
    {
        return (size_t(u16(section.data + offset)) + 65536 - base) % 65536;
    };
    const auto first = pointer(20);
    auto last = pointer(47);
    while (last + 65536 < words) last += 65536;
    require(first < words && last < words
                && (be(stream + last * 4) == 0x800020u || be(stream + last * 4) == 0x800040u),
            "Diretorio NSMP4 invalido.");
    zone.channels = be(stream + last * 4) == 0x800040u ? 2 : 1;

    const auto peak = std::max(1, std::abs(s24(section.data + 13)));
    int peakBits = 0;
    for (auto value = peak; value != 0; value >>= 1) ++peakBits;
    const auto shift = int(section.data[12]) + peakBits - 22
        - ((peak & (peak - 1)) == 0 ? 1 : 0);
    require(shift >= -32 && shift <= 32, "Escala NSMP4 invalida.");
    const auto gainDb = f32(section.data + 57);
    require(! std::isnan(gainDb) && gainDb <= 48.0f, "Ganho NSMP4 invalido.");
    zone.gain = std::isinf(gainDb) && gainDb < 0.0f
        ? 0.0f : std::pow(10.0f, gainDb / 20.0f);

    std::int64_t history[2][4] {};
    constexpr int coefficients[5][4] {
        { 0, 0, 0, 0 }, { -1, 0, 0, 0 }, { -2, 1, 0, 0 },
        { -3, 3, -1, 0 }, { -4, 6, -4, 1 }
    };
    const auto marked = pointer(38);
    auto word = first;
    bool hasLoop = false;
    size_t clipped = 0;
    while (word < last)
    {
        const auto raw = be(stream + word * 4);
        const bool direct = ((raw >> 23) & 1u) != 0;
        const int width = int((raw >> 19) & 15u) + 1;
        const int order = direct ? 0 : int((raw >> 14) & 7u);
        const size_t count = raw & 0x3fffu;
        const auto channels = static_cast<size_t>(zone.channels);
        const size_t perChannel = count / channels;
        require((raw >> 24) == 0 && (raw & 0x20000u) == 0 && order <= 4 && count > 0
                    && count % channels == 0
                    && (direct || count % static_cast<size_t>(32 * zone.channels) == 0),
                "Registro NSMP4 nao suportado.");
        const auto channelWords = (perChannel * static_cast<size_t>(width) + 31) / 32;
        const auto span = zone.channels == 2 ? 1 + 2 * channelWords
            : 1 + (count * static_cast<size_t>(width) + 31) / 32;
        require(span <= last - word && zone.pcm.size() + count <= 35002u * 120u * 2u,
                "Registro NSMP4 fora dos limites.");
        if (word == marked || (raw & 0x40000u) != 0)
        {
            zone.loopStart = zone.frames();
            hasLoop = true;
        }
        const auto before = zone.pcm.size();
        zone.pcm.resize(before + count);
        for (int channel = 0; channel < zone.channels; ++channel)
            for (size_t sample = 0; sample < perChannel; ++sample)
            {
                std::uint32_t packed = 0;
                const auto bitStart = sample * static_cast<size_t>(width);
                for (int bit = 0; bit < width; ++bit)
                {
                    const auto position = bitStart + static_cast<size_t>(bit);
                    const auto dataWord = word + 1 + (zone.channels == 2
                        ? (position / 32) * 2 + static_cast<size_t>(channel) : position / 32);
                    const auto byte = dataWord * 4 + (position % 32) / 8;
                    packed = (packed << 1) | ((stream[byte] >> (7 - position % 8)) & 1u);
                }
                std::int64_t value = (packed & (1u << (width - 1))) != 0
                    ? std::int64_t(packed) - (1ll << width) : packed;
                for (int predictor = 0; predictor < order; ++predictor)
                    value -= coefficients[order][predictor] * history[channel][predictor];
                require(value > -(1ll << 40) && value < (1ll << 40), "Preditor NSMP4 divergente.");
                for (int index = 3; index > 0; --index) history[channel][index] = history[channel][index - 1];
                history[channel][0] = value;
                const auto scaled = std::floor(std::ldexp(static_cast<double>(value), shift));
                if (scaled < -32768.0 || scaled > 32767.0) ++clipped;
                zone.pcm[before + sample * channels + static_cast<size_t>(channel)]
                    = static_cast<float>(std::clamp(scaled, -32768.0, 32767.0) / 32768.0);
            }
        word += span;
        while (word < last && be(stream + word * 4) == 0) ++word;
    }
    require(word == last && ! zone.pcm.empty(), "Fim do stroke NSMP4 invalido.");
    require(clipped * 20 <= zone.pcm.size(), "NSMP4 com saturacao excessiva; reproducao recusada.");
    zone.clippedSamples = clipped;
    if (hasLoop && zone.loopStart + 8 < zone.frames()) zone.loopEnd = zone.frames();
}
}

juce::Result NordSampleDecoder::decode(const juce::File& file, Instrument& output)
{
    output = {};
    try
    {
        require(file.existsAsFile() && file.getSize() >= 100 && file.getSize() <= 256ll * 1024 * 1024,
                "Arquivo NSMP4 invalido ou grande demais.");
        juce::MemoryBlock bytes;
        require(file.loadFileAsData(bytes), "Falha ao ler NSMP4.");
        const auto* data = static_cast<const Byte*>(bytes.getData());
        const auto bodyLength = bytes.getSize() - 44;
        require(std::memcmp(data, "CBIN", 4) == 0 && le(data + 4) == 1
                    && std::memcmp(data + 8, "nsmp", 4) == 0
                    && le(data + 20) >= 400 && le(data + 20) < 500,
                "Geracao NSMP4 nao suportada.");
        data += 44;
        require(crc(data, bodyLength) == le(data - 20), "Checksum NSMP4 invalido.");

        Section map, header;
        std::map<std::uint32_t, Section> strokes;
        size_t offset = 0;
        while (offset < bodyLength)
        {
            require(bodyLength - offset >= 12, "Secao NSMP4 truncada.");
            const auto length = static_cast<size_t>(be(data + offset + 8));
            require(length <= bodyLength - offset - 12, "Tamanho de secao NSMP4 invalido.");
            const Section section { data + offset + 12, length, offset + 12, be(data + offset + 4) };
            if (offset == 0) require(std::memcmp(data, "NSMP", 4) == 0, "Corpo NSMP4 invalido.");
            if (std::memcmp(data + offset, "\0map", 4) == 0) map = section;
            if (std::memcmp(data + offset, "\0hdr", 4) == 0) header = section;
            if (std::memcmp(data + offset, "\0stk", 4) == 0)
            {
                require(length >= 68, "Stroke NSMP4 curto.");
                require(strokes.emplace(be(section.data), section).second,
                        "ID de stroke NSMP4 duplicado.");
            }
            offset += 12 + length;
        }
        require(map.data != nullptr && header.data != nullptr && header.length >= 76
                    && map.version == 21 && map.length >= 1318,
                "Mapa NSMP4 nao suportado.");
        const auto count = static_cast<size_t>(map.data[1317]);
        require(count > 0 && count <= 128 && 1318 + count * 16 <= map.length,
                "Zonas NSMP4 invalidas.");

        Instrument decoded;
        decoded.name = juce::String::fromUTF8(reinterpret_cast<const char*>(header.data + 10), 66).trim();
        const auto globalGain = static_cast<float>(u24(map.data)) / 1048576.0f;
        for (int note = 0; note < 128; ++note)
        {
            const auto* record = map.data + 6 + static_cast<size_t>(note) * 10;
            decoded.gain[static_cast<size_t>(note)] = globalGain
                * static_cast<float>(u24(record)) / 1048576.0f;
            decoded.detune[static_cast<size_t>(note)] = static_cast<float>(s24(record + 3)) / 256.0f;
        }
        size_t totalSamples = 0;
        for (size_t zoneIndex = 0; zoneIndex < count; ++zoneIndex)
        {
            const auto* record = map.data + 1318 + zoneIndex * 16;
            Zone zone;
            zone.root = record[0]; zone.high = record[1]; zone.low = record[2];
            zone.velocityLow = record[14]; zone.velocityHigh = record[15];
            require(zone.root <= 127 && zone.low <= zone.high && zone.high <= 127
                        && zone.velocityLow <= zone.velocityHigh && zone.velocityHigh <= 127,
                    "Limites de zona NSMP4 invalidos.");
            const auto found = strokes.find(be(record + 8));
            require(found != strokes.end() && found->second.data[5] == zone.root,
                    "Stroke da zona NSMP4 nao encontrado.");
            decodeStroke(found->second, zone);
            totalSamples += zone.pcm.size();
            require(totalSamples <= 128u * 1024u * 1024u, "NSMP4 excede o limite de PCM.");
            decoded.zones.push_back(std::move(zone));
        }
        output = std::move(decoded);
        return juce::Result::ok();
    }
    catch (const std::exception& error)
    {
        return juce::Result::fail(juce::String::fromUTF8(error.what()));
    }
}
