#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <vector>

namespace NordPianoDecoder
{
constexpr int sampleRate = 35002;

struct Stroke
{
    std::uint8_t rootNote = 60;
    std::uint8_t zoneLowNote = 0;
    std::uint8_t zoneHighNote = 127;
    std::uint8_t velocityLow = 0;
    std::uint8_t velocityHigh = 127;
    std::uint8_t bank = 0;
    std::uint8_t velocityLayer = 0;
    std::uint8_t sampleChannels = 0;
    std::uint16_t trimDb = 0;
    float zoneGain = 1.0f;
    std::uint32_t loopStartFrame = 0;
    std::uint32_t loopEndFrame = 0;
    bool loopOnRelease = false;
    std::vector<std::int16_t> interleavedPcm;

    int frameCount(int channels) const
    {
        const auto effectiveChannels = sampleChannels > 0 ? sampleChannels : channels;
        return effectiveChannels > 0
            ? static_cast<int>(interleavedPcm.size() / static_cast<size_t>(effectiveChannels)) : 0;
    }
};

struct Instrument
{
    juce::String name;
    juce::String voicing;
    std::uint16_t streamVersion = 0;
    int channels = 0;
    int gainTenthsDb = 0;
    bool signedTenthsDbTrim = false;
    bool usesZones = false;
    float instrumentGain = 1.0f;
    int velocityScale = 31;
    std::array<std::uint8_t, 128> keyMap {};
    std::array<std::int8_t, 128> fineTune {};
    std::array<float, 128> keyGain = []
    {
        std::array<float, 128> values {};
        values.fill(1.0f);
        return values;
    }();
    std::array<float, 128> keyTuneSemitones {};
    std::vector<Stroke> strokes;
};

juce::Result decodeFile(const juce::File& file, Instrument& destination);
}
