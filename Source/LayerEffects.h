#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

// Small allocation-free three-band layer EQ.  It is deliberately shared by
// all engines so a layer sounds the same when its source is changed.
struct LayerEqState
{
    std::array<float, 2> low {};
    std::array<float, 2> midLow {};
    std::array<float, 2> highInput {};
    std::array<float, 2> highOutput {};

    void reset() noexcept
    {
        low = {};
        midLow = {};
        highInput = {};
        highOutput = {};
    }

    float process(float input, int channel, float lowDb, float midDb, float highDb,
                  double sampleRate, float lowFrequency = 220.0f,
                  float midFrequency = 1200.0f, float highFrequency = 4200.0f) noexcept
    {
        const auto safeRate = juce::jmax(1.0, sampleRate);
        const auto safeLow = juce::jlimit(40.0f, 2000.0f, lowFrequency);
        const auto safeMid = juce::jlimit(safeLow + 20.0f, 12000.0f, midFrequency);
        const auto safeHigh = juce::jlimit(safeMid + 20.0f, 20000.0f, highFrequency);
        const auto lowCoefficient = std::exp(-juce::MathConstants<float>::twoPi * safeLow
                                             / static_cast<float>(safeRate));
        const auto highCoefficient = std::exp(-juce::MathConstants<float>::twoPi * safeHigh
                                              / static_cast<float>(safeRate));
        auto& lowState = low[(size_t) juce::jlimit(0, 1, channel)];
        auto& midState = midLow[(size_t) juce::jlimit(0, 1, channel)];
        auto& previousInput = highInput[(size_t) juce::jlimit(0, 1, channel)];
        auto& previousOutput = highOutput[(size_t) juce::jlimit(0, 1, channel)];
        lowState = (1.0f - lowCoefficient) * input + lowCoefficient * lowState;
        const auto midCoefficient = std::exp(-juce::MathConstants<float>::twoPi * safeMid
                                             / static_cast<float>(safeRate));
        midState = (1.0f - midCoefficient) * input + midCoefficient * midState;
        const auto high = highCoefficient * (previousOutput + input - previousInput);
        previousInput = input;
        previousOutput = high;
        const auto mid = midState - lowState;
        return lowState * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, lowDb))
             + mid * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, midDb))
             + high * juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, highDb));
    }
};
