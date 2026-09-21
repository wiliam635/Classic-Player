#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

// Small allocation-free three-band layer EQ.  It is deliberately shared by
// all engines so a layer sounds the same when its source is changed.
struct LayerEqState
{
    struct BiquadState
    {
        float input1 = 0.0f, input2 = 0.0f;
        float output1 = 0.0f, output2 = 0.0f;

        void reset() noexcept { input1 = input2 = output1 = output2 = 0.0f; }
    };

    std::array<BiquadState, 2> low {}, mid {}, high {};

    void reset() noexcept
    {
        for (auto& state : low) state.reset();
        for (auto& state : mid) state.reset();
        for (auto& state : high) state.reset();
    }

    float process(float input, int channel, float lowDb, float midDb, float highDb,
                  double sampleRate, float lowFrequency = 220.0f,
                  float midFrequency = 1200.0f, float highFrequency = 4200.0f,
                  float lowQ = 0.707f, float midQ = 1.0f, float highQ = 0.707f) noexcept
    {
        const auto safeRate = juce::jmax(1.0, sampleRate);
        const auto safeLow = juce::jlimit(40.0f, 2000.0f, lowFrequency);
        const auto safeMid = juce::jlimit(safeLow + 20.0f, 12000.0f, midFrequency);
        const auto safeHigh = juce::jlimit(safeMid + 20.0f, 20000.0f, highFrequency);
        const auto index = (size_t) juce::jlimit(0, 1, channel);

        const auto applyPeaking = [safeRate](float sample, float frequency, float gainDb, float q,
                                             BiquadState& state) noexcept
        {
            const auto safeFrequency = juce::jlimit(20.0f, 0.49f * (float) safeRate, frequency);
            const auto safeQ = juce::jlimit(0.1f, 20.0f, q);
            const auto amplitude = juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 18.0f, gainDb));
            const auto omega = juce::MathConstants<float>::twoPi * safeFrequency / (float) safeRate;
            const auto alpha = std::sin(omega) / (2.0f * safeQ);
            const auto cosine = std::cos(omega);
            const auto a0 = 1.0f + alpha / amplitude;
            const auto b0 = (1.0f + alpha * amplitude) / a0;
            const auto b1 = (-2.0f * cosine) / a0;
            const auto b2 = (1.0f - alpha * amplitude) / a0;
            const auto a1 = (-2.0f * cosine) / a0;
            const auto a2 = (1.0f - alpha / amplitude) / a0;
            const auto output = b0 * sample + b1 * state.input1 + b2 * state.input2
                              - a1 * state.output1 - a2 * state.output2;
            state.input2 = state.input1;
            state.input1 = sample;
            state.output2 = state.output1;
            state.output1 = output;
            return output;
        };

        auto output = applyPeaking(input, safeLow, lowDb, lowQ, low[index]);
        output = applyPeaking(output, safeMid, midDb, midQ, mid[index]);
        return applyPeaking(output, safeHigh, highDb, highQ, high[index]);
    }
};
