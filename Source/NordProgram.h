#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Independent reader for Stage 4 programs. Coordinates are format facts;
// playback is implemented by Classic's own engines, not by Nord/WX code.
namespace NordProgram
{
struct Field { const char* name; unsigned block, offset, width; int64_t bias; unsigned kind; };
inline constexpr Field fields[] {
#include "NordProgramFields.inc"
};
struct Program
{
    std::map<std::string, int64_t> values;
    int64_t get(const std::string& key, int64_t fallback = 0) const
    { const auto i = values.find(key); return i == values.end() ? fallback : i->second; }
};
inline bool read(const uint8_t* bytes, size_t size, Program& output, std::string& error)
{
    output.values.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (bytes == nullptr || size < 48 || size > 65536 || std::memcmp(bytes, "CBIN", 4) != 0
        || std::memcmp(bytes + 8, "ns4p", 4) != 0)
        return fail("Arquivo NS4P invalido ou formato nao reconhecido.");
    std::array<size_t, 24> starts {}, lengths {};
    size_t offset = 48, count = 0;
    while (offset < size)
    {
        if (count == starts.size()) return fail("Layout NS4P nao suportado: mais de 24 blocos.");
        const auto length = static_cast<size_t>(bytes[offset++]);
        if (length > size - offset) return fail("Bloco NS4P truncado.");
        starts[count] = offset; lengths[count++] = length; offset += length;
    }
    if (count != starts.size()) return fail("Layout NS4P nao suportado: esperado Stage 4 com 24 blocos.");
    for (const auto& f : fields)
    {
        if (f.block >= count || f.width > 32 || f.offset + f.width > lengths[f.block] * 8)
            continue; // Fields absent from a shorter block keep their explicit defaults.
        uint64_t value = 0;
        for (unsigned bit = 0; bit < f.width; ++bit)
        {
            const auto position = starts[f.block] * 8 + f.offset + bit;
            value = (value << 1) | ((bytes[position / 8] >> (7 - position % 8)) & 1u);
        }
        int64_t decoded = static_cast<int64_t>(value);
        if (f.kind == 1) decoded += f.bias;
        else if (f.kind == 2 && f.width != 0 && (value & (uint64_t(1) << (f.width - 1))))
            decoded -= int64_t(1) << f.width;
        output.values.emplace(f.name, decoded);
    }
    error.clear();
    return true;
}

// Nord program name identifiers: Murmur2 variant with big-endian words,
// length seed and little-endian tail. Unsigned arithmetic is intentional.
inline uint32_t soundId(const std::string& name) noexcept
{
    constexpr uint32_t m = 0x5bd1e995u;
    uint32_t hash = static_cast<uint32_t>(name.size());
    size_t pos = 0;
    const auto* b = reinterpret_cast<const uint8_t*>(name.data());
    while (name.size() - pos >= 4)
    {
        uint32_t word = (uint32_t(b[pos]) << 24) | (uint32_t(b[pos+1]) << 16)
                      | (uint32_t(b[pos+2]) << 8) | b[pos+3];
        word *= m; word ^= word >> 24; word *= m;
        hash = hash * m ^ word; pos += 4;
    }
    for (size_t i = 0; i < name.size() - pos; ++i) hash ^= uint32_t(b[pos+i]) << (8*i);
    if (pos != name.size()) hash *= m;
    const bool zero = hash == 0;
    hash ^= hash >> 13; hash *= m; hash ^= hash >> 15;
    return zero ? 1u : hash;
}
}
