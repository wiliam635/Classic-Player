#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <cstring>

// Small allocation-free three-band layer EQ.  It is deliberately shared by
// all engines so a layer sounds the same when its source is changed.
struct LayerEqState
{
    struct BiquadCoefficients
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        bool enabled = false;
    };

    struct BiquadState
    {
        float input1 = 0.0f, input2 = 0.0f;
        float output1 = 0.0f, output2 = 0.0f;

        void reset() noexcept { input1 = input2 = output1 = output2 = 0.0f; }
    };

    std::array<BiquadState, 2> low {}, mid {}, high {};
    std::array<BiquadCoefficients, 3> coefficients {};
    double cachedSampleRate = 0.0;
    std::array<float, 9> cachedParameters {};
    bool coefficientsReady = false;

    void reset() noexcept
    {
        for (auto& state : low) state.reset();
        for (auto& state : mid) state.reset();
        for (auto& state : high) state.reset();
    }

    void setParameters(float lowDb, float midDb, float highDb, double sampleRate,
                       float lowFrequency = 220.0f, float midFrequency = 1200.0f,
                       float highFrequency = 4200.0f, float lowQ = 0.707f,
                       float midQ = 1.0f, float highQ = 0.707f) noexcept
    {
        const std::array<float, 9> parameters {
            lowDb, midDb, highDb, lowFrequency, midFrequency, highFrequency,
            lowQ, midQ, highQ
        };
        const auto sameRate = std::memcmp(&sampleRate, &cachedSampleRate, sizeof(sampleRate)) == 0;
        const auto sameParameters = std::memcmp(parameters.data(), cachedParameters.data(),
                                                sizeof(parameters)) == 0;
        if (coefficientsReady && sameRate && sameParameters)
            return;

        const auto safeRate = juce::jmax(1.0, sampleRate);
        const auto safeLow = juce::jlimit(40.0f, 2000.0f, lowFrequency);
        const auto safeMid = juce::jlimit(safeLow + 20.0f, 12000.0f, midFrequency);
        const auto safeHigh = juce::jlimit(safeMid + 20.0f, 20000.0f, highFrequency);
        const auto makePeaking = [safeRate](float frequency, float gainDb, float q) noexcept
        {
            const auto safeFrequency = juce::jlimit(20.0f, 0.49f * (float) safeRate, frequency);
            const auto safeQ = juce::jlimit(0.1f, 20.0f, q);
            // RBJ peaking-EQ coefficients use A = 10^(dB/40), not the
            // ordinary amplitude conversion 10^(dB/20). Using the latter
            // doubles the requested boost/cut and disagrees with the graph.
            const auto amplitude = juce::Decibels::decibelsToGain(
                0.5f * juce::jlimit(-18.0f, 18.0f, gainDb));
            const auto omega = juce::MathConstants<float>::twoPi * safeFrequency / (float) safeRate;
            const auto alpha = std::sin(omega) / (2.0f * safeQ);
            const auto cosine = std::cos(omega);
            const auto a0 = 1.0f + alpha / amplitude;
            const auto b0 = (1.0f + alpha * amplitude) / a0;
            const auto b1 = (-2.0f * cosine) / a0;
            const auto b2 = (1.0f - alpha * amplitude) / a0;
            const auto a1 = (-2.0f * cosine) / a0;
            const auto a2 = (1.0f - alpha / amplitude) / a0;
            return BiquadCoefficients { b0, b1, b2, a1, a2,
                                        std::abs(gainDb) > 1.0e-6f };
        };

        coefficients[0] = makePeaking(safeLow, lowDb, lowQ);
        coefficients[1] = makePeaking(safeMid, midDb, midQ);
        coefficients[2] = makePeaking(safeHigh, highDb, highQ);
        cachedSampleRate = sampleRate;
        cachedParameters = parameters;
        coefficientsReady = true;
    }

    float process(float input, int channel) noexcept
    {
        const auto index = (size_t) juce::jlimit(0, 1, channel);
        auto output = processBandOrBypass(input, coefficients[0], low[index]);
        output = processBandOrBypass(output, coefficients[1], mid[index]);
        return processBandOrBypass(output, coefficients[2], high[index]);
    }

private:
    static float processBandOrBypass(float sample, const BiquadCoefficients& coefficients,
                                     BiquadState& state) noexcept
    {
        if (coefficients.enabled)
            return processBand(sample, coefficients, state);

        // A zero-gain peaking band is an identity filter. Preserve its delay
        // history while bypassed so enabling it again mid-performance does
        // not revive stale samples or introduce a discontinuity.
        state.input2 = state.input1;
        state.input1 = sample;
        state.output2 = state.output1;
        state.output1 = sample;
        return sample;
    }

    static float processBand(float sample, const BiquadCoefficients& coefficients,
                             BiquadState& state) noexcept
    {
        const auto output = coefficients.b0 * sample + coefficients.b1 * state.input1
                          + coefficients.b2 * state.input2 - coefficients.a1 * state.output1
                          - coefficients.a2 * state.output2;
        state.input2 = state.input1;
        state.input1 = sample;
        state.output2 = state.output1;
        state.output1 = output;
        return output;
    }
};
