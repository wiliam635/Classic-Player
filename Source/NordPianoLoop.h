#pragma once

#include "NordPianoDecoder.h"
#include <juce_core/juce_core.h>
#include <cmath>

// Loop-seam preparation of an NPNO stroke, shared by the player and the SF2 export.
namespace NordPianoLoop
{
constexpr double crossfadeSeconds = 0.55;

inline void prepare(NordPianoDecoder::Stroke& stroke, int channels)
{
    if (channels < 1 || channels > 2) return;
    {
        const auto start = static_cast<int>(stroke.loopStartFrame);
        const auto end = static_cast<int>(stroke.loopEndFrame); // exclusive
        const auto length = end - start;
        if (stroke.bank == 2 || start < 0 || length < 8
            || end > stroke.frameCount(channels))
            return;
        const auto maximum = static_cast<int>(length * 0.45);
        const auto aim = juce::jmin(maximum, juce::roundToInt(
            crossfadeSeconds * NordPianoDecoder::sampleRate));
        const auto frequency = 440.0 * std::pow(2.0, (stroke.rootNote - 69) / 12.0);
        const auto period = NordPianoDecoder::sampleRate / frequency;
        const auto low = juce::jmax(2, juce::roundToInt(aim - period));
        const auto high = juce::jmin(maximum, juce::roundToInt(aim + period));
        const auto step = juce::jmax(1, juce::roundToInt(period / 24.0));
        if (low > high) return;
        const auto read = [&](int frame, int channel)
        {
            return static_cast<double>(stroke.interleavedPcm[
                static_cast<size_t>(frame * channels + channel)]);
        };
        // Preserve the NPNO loop search and equal-power normalization from
        // Classic Player 3.0 so velocity layers retain their original sustain tone.
        auto fade = low;
        auto bestScore = -2.0;
        auto correlation = 0.0;
        // One alignment and one gain law for both channels preserve stereo.
        for (int candidate = low; candidate <= high; candidate += step)
        {
            double dot = 0.0, headPower = 0.0, tailPower = 0.0;
            for (int frame = 0; frame < candidate; frame += 8)
                for (int channel = 0; channel < channels; ++channel)
                {
                    const auto head = read(start + frame, channel);
                    const auto tail = read(end - candidate + frame, channel);
                    dot += head * tail;
                    headPower += head * head;
                    tailPower += tail * tail;
                }
            const auto norm = std::sqrt(headPower * tailPower);
            const auto corr = norm > 0.0 ? dot / norm : 1.0;
            const auto score = corr - 0.01 * std::abs(candidate - aim) / juce::jmax(1, aim);
            if (score > bestScore)
            {
                bestScore = score;
                fade = candidate;
                correlation = juce::jlimit(0.0, 1.0, corr);
            }
        }
        const auto blend = [&](int frame, int channel, bool powerCompensation)
        {
            const auto weight = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::pi * frame / (fade - 1));
            const auto norm = powerCompensation
                ? std::sqrt((1.0 - weight) * (1.0 - weight) + weight * weight
                            + 2.0 * correlation * weight * (1.0 - weight)) : 1.0;
            return ((1.0 - weight) * read(end - fade + frame, channel)
                    + weight * read(start + frame, channel)) / norm;
        };
        bool powerCompensation = true;
        for (int frame = 0; frame < fade && powerCompensation; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                if (std::abs(blend(frame, channel, true)) > 32767.0)
                {
                    powerCompensation = false;
                    break;
                }
        for (int frame = 0; frame < fade; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                stroke.interleavedPcm[static_cast<size_t>((end - fade + frame) * channels + channel)]
                    = static_cast<std::int16_t>(std::round(blend(frame, channel, powerCompensation)));
        // The head has already played inside the overlap: resume after it.
        stroke.loopStartFrame += static_cast<std::uint32_t>(fade);
    }
}
}
