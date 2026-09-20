#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

// Small allocation-free three-band layer EQ.  It is deliberately shared by
// all engines so a layer sounds the same when its source is changed.
struct LayerEqState
{
    std::array<float, 2> low {};
    std::array<float, 2> highInput {};
    std::array<float, 2> highOutput {};

    void reset() noexcept
    {
        low = {};
        highInput = {};
        highOutput = {};
    }

    float process(float input, int channel, float lowDb, float midDb, float highDb,
                  double sampleRate) noexcept
    {
        const auto safeRate = juce::jmax(1.0, sampleRate);
        const auto lowCoefficient = std::exp(-juce::MathConstants<float>::twoPi * 220.0f
                                             / static_cast<float>(safeRate));
        const auto highCoefficient = std::exp(-juce::MathConstants<float>::twoPi * 4200.0f
                                              / static_cast<float>(safeRate));
        auto& lowState = low[(size_t) juce::jlimit(0, 1, channel)];
        auto& previousInput = highInput[(size_t) juce::jlimit(0, 1, channel)];
        auto& previousOutput = highOutput[(size_t) juce::jlimit(0, 1, channel)];
        lowState = (1.0f - lowCoefficient) * input + lowCoefficient * lowState;
        const auto high = highCoefficient * (previousOutput + input - previousInput);
        previousInput = input;
        previousOutput = high;
        const auto mid = input - lowState - high;
        return lowState * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, lowDb))
             + mid * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, midDb))
             + high * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, highDb));
    }
};
