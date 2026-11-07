#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <cstdint>
#include <limits>

namespace NordSampleDecoder
{
constexpr double sampleRate = 35002.0;

struct Zone
{
    int root = 60, low = 0, high = 127, velocityLow = 0, velocityHigh = 127, channels = 1;
    float gain = 1.0f;
    size_t secondStart = 0, loopStart = 0, loopEnd = 0, clippedSamples = 0;
    std::vector<float> pcm;
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
