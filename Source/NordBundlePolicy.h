#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace NordBundlePolicy
{
// NS4P stores pairs of variants. WX importProgram maps raw/2 through
// {0,1,3,2,4,5} into Phaser/Flanger/Chorus/Vibe/Ensemble/Spin.
enum class Modulation2 { phaser, flanger, chorus, vibe, ensemble, spin, unknown };
inline Modulation2 modulation2(std::int64_t raw) noexcept
{
    return raw >= 0 && raw < 12 ? static_cast<Modulation2>(raw / 2) : Modulation2::unknown;
}
inline bool isTremolo(std::int64_t raw) noexcept { return raw == 2 || raw == 3; }
inline bool secondVariant(std::int64_t raw) noexcept { return (raw & 1) != 0; }

// Our decoded-PCM safety policy, not a claim about WX memory management.
// Reserve most system RAM for the OS, the current program and the host DAW.
inline std::uint64_t decodedBudgetBytes(int memoryMiB) noexcept
{
    const auto miB = memoryMiB > 0 ? std::clamp(memoryMiB / 4, 256, 3072) : 768;
    return static_cast<std::uint64_t>(miB) * 1024u * 1024u;
}
inline float eqDecibels(std::int64_t quarterDb) noexcept
{
    return static_cast<float>(std::clamp<std::int64_t>(quarterDb, -60, 60)) * 0.25f;
}
}
