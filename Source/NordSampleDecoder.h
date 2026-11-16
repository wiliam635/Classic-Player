#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <cstdint>
#include <limits>
#include <cmath>
#include <algorithm>

namespace NordSampleDecoder
{
constexpr double sampleRate = 35002.0;

// Zone audio is stored as 16-bit integers (half the RAM of float). The vendor data is
// integer valued in 16-bit units; only the soft knee above -0.45 dBFS and the loop-seam
// correction produce fractions, and rounding those is at most half an LSB (-96 dBFS).
using Pcm = std::int16_t;
constexpr float pcmToFloat = 1.0f / 32768.0f;
inline Pcm floatToPcm(float value) noexcept
{
    return static_cast<Pcm>(std::clamp<long>(std::lround(value * 32768.0f), -32768L, 32767L));
}

struct Zone
{
    int root = 60, low = 0, high = 127, velocityLow = 0, velocityHigh = 127, channels = 1;
    float gain = 1.0f;
    size_t secondStart = 0, loopStart = 0, loopEnd = 0, clippedSamples = 0;
    std::vector<Pcm> pcm;
    size_t frames() const noexcept { return pcm.size() / static_cast<size_t>(channels); }
};

struct Instrument
{
    juce::String name;
    std::array<float, 128> gain {}, detune {};
    std::vector<Zone> zones;
};

juce::Result decode(const juce::File&, Instrument&,
                    std::uint64_t maxDecodedBytes = std::numeric_limits<std::uint64_t>::max());
}
