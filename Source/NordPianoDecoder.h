#pragma once

#include <juce_core/juce_core.h>
#include "PcmBuffer.h"
#include <array>
#include <cstdint>
#include <limits>
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
    std::uint32_t loopStartFrame = 0;
    std::uint32_t loopEndFrame = 0;
    float peakDb = -120.0f; // peak of the first 0.3 s in dBFS (measured at load, not stored in the file)
    PcmBuffer interleavedPcm;

    int frameCount(int channels) const
    {
        return channels > 0 ? static_cast<int>(interleavedPcm.size() / static_cast<size_t>(channels)) : 0;
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
    int velocityScale = 31;
    std::array<std::uint8_t, 128> keyMap {};
    std::array<std::int8_t, 128> fineTune {};
    std::vector<Stroke> strokes;
    bool levelsMeasured = false; // peakDb of every stroke is valid
};

// Receives the strokes one at a time instead of keeping every decoded stroke in RAM. decodeFile uses it only
// when the whole piano would not fit the memory budget and the sink says it can take over (see available()).
// Each stroke is handed over right after it is decoded and its PCM is freed once stroke() returns, so the peak
// stays near one stroke instead of the whole piano. The returned Instrument then holds the stroke list without PCM.
struct StrokeSink
{
    virtual ~StrokeSink() = default;
    // True when the sink can store `decodedBytes` of audio (cache enabled, mapping on, enough disk space).
    virtual bool available(std::uint64_t decodedBytes) = 0;
    // Called once before the first stroke: every field of `header` except signedTenthsDbTrim and velocityScale.
    virtual juce::Result begin(const Instrument& header, std::uint32_t keptStrokes) = 0;
    virtual juce::Result stroke(Stroke& stroke) = 0;
    // Called after the last stroke with the final header (signedTenthsDbTrim and velocityScale are now known).
    virtual juce::Result end(const Instrument& finished) = 0;
};

juce::Result decodeFile(const juce::File& file, Instrument& destination,
                        std::uint64_t maxDecodedBytes = std::numeric_limits<std::uint64_t>::max(),
                        StrokeSink* sink = nullptr);
}
