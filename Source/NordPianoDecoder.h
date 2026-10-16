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
    std::uint8_t bank = 0;
    std::uint8_t velocityLayer = 0;
    std::uint16_t trimDb = 0;
    float levelGain = 1.0f;
    std::uint32_t loopStartFrame = 0;
    std::uint32_t loopEndFrame = 0;
    std::vector<std::int16_t> interleavedPcm;

    int frameCount(int channels) const
    {
        return channels > 0 ? static_cast<int>(interleavedPcm.size() / static_cast<size_t>(channels)) : 0;
    }
};

struct Instrument
{
    juce::String name;
    juce::String voicing;
    double sampleRateHz = NordPianoDecoder::sampleRate;
    std::uint16_t streamVersion = 0;
    int channels = 0;
    int gainTenthsDb = 0;
    bool signedTenthsDbTrim = false;
    int velocityScale = 31;
    bool applyPianoTimbre = true;
    bool finitePianoDecay = true;
    std::array<std::uint8_t, 128> keyMap {};
    std::array<std::array<std::int16_t, 128>, 128> zoneMap = []
    {
        std::array<std::array<std::int16_t, 128>, 128> map {};
        for (auto& velocityMap : map) velocityMap.fill(-1);
        return map;
    }();
    std::array<std::int8_t, 128> fineTune {};
    std::vector<Stroke> strokes;
};

juce::Result decodeFile(const juce::File& file, Instrument& destination);
}
