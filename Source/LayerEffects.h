#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdint>
#include "NordModulation.h"

struct NordEffectSettings
{
    float tremoloMix = 0.0f, tremoloRate = 4.0f;
    float flangerMix = 0.0f, flangerRate = 0.35f;
    float chorusMix = 0.0f, chorusRate = 0.8f, chorusDepth = 8.0f;
    float delayMix = 0.0f, delayBpm = 120.0f, delayFeedback = 30.0f;
    float delayRate = 0.0f; // NS4P dlyRateClkDiv, normalized from 14 bits
    bool delaySync = false, delayPingPong = false, delayAnalog = false;
    int delayFeedbackFilter = 0; // 0 off; remaining modes are adapted filters
    bool ampEnabled = false;
    int ampModel = 0; // Known WX labels: Twin, Small, JC; exact enum mapping inferred
    float ampDrive = 0.0f;
    int modulationType = 0; // 0 off, 1 phaser, 2 vibe, 3 ensemble, 4 spin
    float modulationMix = 0.0f, modulationRate = 0.8f;
};

// Shared layer Attack scale. Preserve the existing 0..50 ms range for old
// programs, then extend the upper half smoothly to 2 s for pads and strings.
inline double layerAttackSeconds(float control) noexcept
{
    const auto value = juce::jlimit(0.0f, 100.0f, control);
    if (value <= 50.0f)
        return static_cast<double>(value) * 0.001;
    return 0.05 * std::pow(40.0, static_cast<double>(value - 50.0f) / 50.0);
}

// Shared layer Release scale: 0..50 maps to 5..500 ms and 50..100 to
// 0.5..10 s. Keep the envelope feel aligned across sampler and synth layers.
inline double layerReleaseSeconds(float control) noexcept
{
    const auto value = juce::jlimit(0.0f, 100.0f, control);
    return value <= 50.0f
        ? juce::jmax(0.005, static_cast<double>(value) * 0.01)
        : 0.5 * std::pow(20.0, static_cast<double>(value - 50.0f) / 50.0);
}

// Convert the UI amount to complementary external mix coefficients. The
// LayerReverb wrapper uses wetLevel as the target wet share.
inline void setLayerReverbMix(juce::Reverb::Parameters& parameters,
                              float amount, float maximumWet = 0.72f) noexcept
{
    const auto wet = juce::jlimit(0.0f, 1.0f, amount)
                   * juce::jlimit(0.0f, 1.0f, maximumWet);
    parameters.wetLevel = wet;
    parameters.dryLevel = 1.0f - wet;
}

// Delay and reverb returns can be silent for a buffer before another reflection.
// Keep processing for two quiet seconds after the last voice or audible tail.
inline void updateLayerEffectTail(bool& pending, std::uint64_t& quietSamples,
                                  bool hasVoices, float peak, int samples,
                                  double sampleRate) noexcept
{
    if (hasVoices || peak > 1.0e-6f)
    {
        pending = true;
        quietSamples = 0;
    }
    else if (pending)
    {
        quietSamples += static_cast<std::uint64_t>(juce::jmax(0, samples));
        if (quietSamples >= static_cast<std::uint64_t>(sampleRate * 2.0))
            pending = false;
    }
}

// Smoothed, independent room reverb return.
struct LayerReverb
{
    LayerReverb() noexcept { wetMix.reset(1024); wetMix.setCurrentAndTargetValue(0.0f); }
    void setSampleRate(double rate)
    {
        // Set the internal return fully wet before JUCE resets its smoothers.
        // Otherwise its default dry signal fades out only after processing
        // starts, briefly adding gain to the layer when reverb is enabled.
        auto parameters = reverb.getParameters();
        parameters.wetLevel = 1.0f / 3.0f;
        parameters.dryLevel = 0.0f;
        reverb.setParameters(parameters);
        reverb.setSampleRate(rate);
    }
    void reset() noexcept
    {
        reverb.reset();
        wetMix.setCurrentAndTargetValue(0.0f);
        targetWet = 0.0f;
        reverbParametersReady = false;
        cachedRoomParameters.fill(-1.0f);
    }
    void setParameters(juce::Reverb::Parameters parameters)
    {
        const auto nextWet = juce::jlimit(0.0f, 1.0f, parameters.wetLevel);
        if (nextWet != targetWet)
        {
            if (nextWet > 0.0f && targetWet <= 0.0f) reverb.reset();
            targetWet = nextWet;
            wetMix.setTargetValue(targetWet);
        }
        const std::array<float, 4> roomParameters {
            parameters.roomSize, parameters.damping, parameters.width, parameters.freezeMode
        };
        if (!reverbParametersReady || roomParameters != cachedRoomParameters)
        {
            parameters.wetLevel = 1.0f / 3.0f;
            parameters.dryLevel = 0.0f;
            reverb.setParameters(parameters);
            cachedRoomParameters = roomParameters;
            reverbParametersReady = true;
        }
    }
    bool shouldProcess() const noexcept
    {
        return targetWet > 0.0f || wetMix.isSmoothing() || wetMix.getCurrentValue() > 0.0f;
    }
    void processStereo(float* left, float* right, int samples) noexcept
    {
        if (left == nullptr || right == nullptr || samples <= 0 || !shouldProcess()) return;
        constexpr int chunkSize = 64;
        float wetLeft[chunkSize], wetRight[chunkSize];
        for (int start = 0; start < samples; start += chunkSize)
        {
            const auto count = juce::jmin(chunkSize, samples - start);
            std::copy_n(left + start, count, wetLeft);
            std::copy_n(right + start, count, wetRight);
            reverb.processStereo(wetLeft, wetRight, count);
            for (int i = 0; i < count; ++i)
            {
                const auto wet = wetMix.getNextValue();
                const auto dry = std::sqrt(juce::jmax(0.0f, 1.0f - wet * wet));
                left[start + i] = left[start + i] * dry + wetLeft[i] * wet;
                right[start + i] = right[start + i] * dry + wetRight[i] * wet;
            }
        }
    }
private:
    juce::Reverb reverb;
    juce::SmoothedValue<float> wetMix;
    std::array<float, 4> cachedRoomParameters { -1.0f, -1.0f, -1.0f, -1.0f };
    float targetWet = 0.0f;
    bool reverbParametersReady = false;
};

// Audio-thread-safe, allocation-free processing. Buffers are allocated only
// in prepare(), never while rendering notes or changing the panel controls.
struct NordLayerEffects
{
    void prepare(double rate)
    {
        sampleRate = juce::jmax(1.0, rate);
        const auto modulationLength = (size_t) juce::jmax(64, juce::roundToInt(sampleRate * 0.06));
        const auto delayLength = (size_t) juce::jmax(64, juce::roundToInt(sampleRate * 2.5));
        for (auto& channel : modulation) channel.assign(modulationLength, 0.0f);
        for (auto& channel : delay) channel.assign(delayLength, 0.0f);
        for (auto& filter : feedbackFilterState) filter = {};
        tremoloMixSmoother.reset(sampleRate, 0.01);
        flangerMixSmoother.reset(sampleRate, 0.01);
        chorusMixSmoother.reset(sampleRate, 0.01);
        extendedMixSmoother.reset(sampleRate, 0.01);
        delayMixSmoother.reset(sampleRate, 0.01);
        delayFeedbackSmoother.reset(sampleRate, 0.02);
        driveSmoother.reset(sampleRate, 0.02);
        tremoloMixSmoother.setCurrentAndTargetValue(0.0f);
        flangerMixSmoother.setCurrentAndTargetValue(0.0f);
        chorusMixSmoother.setCurrentAndTargetValue(0.0f);
        extendedMixSmoother.setCurrentAndTargetValue(0.0f);
        delayMixSmoother.setCurrentAndTargetValue(0.0f);
        delayFeedbackSmoother.setCurrentAndTargetValue(0.0f);
        driveSmoother.setCurrentAndTargetValue(0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : modulation) std::fill(channel.begin(), channel.end(), 0.0f);
        for (auto& channel : delay) std::fill(channel.begin(), channel.end(), 0.0f);
        modulationPosition = delayPosition = 0;
        delayValidSamples = 0;
        delayWasActive = false;
        tremoloPhase = flangerPhase = chorusPhase = 0.0;
        for (auto& filter : feedbackFilterState) filter = {};
        tremoloMixSmoother.setCurrentAndTargetValue(0.0f);
        flangerMixSmoother.setCurrentAndTargetValue(0.0f);
        chorusMixSmoother.setCurrentAndTargetValue(0.0f);
        extendedMixSmoother.setCurrentAndTargetValue(0.0f);
        delayMixSmoother.setCurrentAndTargetValue(0.0f);
        delayFeedbackSmoother.setCurrentAndTargetValue(0.0f);
        driveSmoother.setCurrentAndTargetValue(0.0f);
        extendedModulation.reset();
    }

    void process(juce::AudioBuffer<float>& buffer, const NordEffectSettings& settings) noexcept
    {
        if (buffer.getNumChannels() < 1 || modulation[0].empty() || delay[0].empty()) return;
        const auto tremoloTarget = juce::jlimit(0.0f, 1.0f, settings.tremoloMix * 0.01f);
        const auto flangerTarget = juce::jlimit(0.0f, 1.0f, settings.flangerMix * 0.01f);
        const auto chorusTarget = juce::jlimit(0.0f, 1.0f, settings.chorusMix * 0.01f);
        const auto chorusDepthMs = juce::jlimit(0.0f, 20.0f, settings.chorusDepth);
        const auto delayMixTarget = juce::jlimit(0.0f, 1.0f, settings.delayMix * 0.01f);
        const auto extendedMixTarget = juce::jlimit(0.0f, 1.0f, settings.modulationMix * 0.01f);
        const auto drive = settings.ampEnabled ? juce::jlimit(0.0f, 1.0f, settings.ampDrive * 0.01f) : 0.0f;
        tremoloMixSmoother.setTargetValue(tremoloTarget);
        flangerMixSmoother.setTargetValue(flangerTarget);
        chorusMixSmoother.setTargetValue(chorusTarget);
        extendedMixSmoother.setTargetValue(extendedMixTarget);
        delayMixSmoother.setTargetValue(delayMixTarget);
        delayFeedbackSmoother.setTargetValue(juce::jlimit(0.0f, 0.95f, settings.delayFeedback * 0.0095f));
        driveSmoother.setTargetValue(drive);
        if (tremoloTarget + flangerTarget + chorusTarget + delayMixTarget + extendedMixTarget + drive <= 0.0f
            && !tremoloMixSmoother.isSmoothing() && !flangerMixSmoother.isSmoothing()
            && !chorusMixSmoother.isSmoothing() && !extendedMixSmoother.isSmoothing()
            && !delayMixSmoother.isSmoothing() && !driveSmoother.isSmoothing())
        {
            delayValidSamples = 0;
            delayWasActive = false;
            for (auto& filter : feedbackFilterState) filter = {};
            return;
        }
        const auto delaySeconds = delayTimeSeconds(settings);
        const auto tempoSamples = juce::jlimit(2, (int) delay[0].size() - 2,
            juce::roundToInt(sampleRate * delaySeconds));
        const auto twopi = juce::MathConstants<double>::twoPi;
        const auto channels = juce::jmin(2, buffer.getNumChannels());
        for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
        {
            const auto tremolo = tremoloMixSmoother.getNextValue();
            const auto flanger = flangerMixSmoother.getNextValue();
            const auto chorus = chorusMixSmoother.getNextValue();
            const auto delayMix = delayMixSmoother.getNextValue();
            const auto extendedMix = extendedMixSmoother.getNextValue();
            const auto tremoloLfo = tremolo > 0.0f ? (float) (0.5 + 0.5 * std::sin(tremoloPhase)) : 1.0f;
            const auto flangerLfo = flanger > 0.0f ? (float) (0.5 + 0.5 * std::sin(flangerPhase)) : 0.0f;
            const auto chorusLfo = chorus > 0.0f ? (float) (0.5 + 0.5 * std::sin(chorusPhase)) : 0.0f;
            const auto dryLeft = buffer.getSample(0, frame);
            const auto dryRight = buffer.getNumChannels() > 1 ? buffer.getSample(1, frame) : dryLeft;
            auto modulated = extendedModulation.process({dryLeft, dryRight},
                settings.modulationType >= 3 ? 0 : settings.modulationType,
                extendedMix, settings.modulationRate, sampleRate);
            const auto feedback = delayFeedbackSmoother.getNextValue();
            const auto driveAmount = driveSmoother.getNextValue();
            if (delayMix > 0.0f && !delayWasActive)
            {
                delayValidSamples = 0;
                for (auto& filter : feedbackFilterState) filter = {};
            }
            delayWasActive = delayMix > 0.0f;
            float echoes[2] {};
            for (int channel = 0; channel < channels; ++channel)
            {
                if (delayValidSamples >= tempoSamples)
                {
                    const auto readPosition = (delayPosition + delay[(size_t) channel].size()
                                               - (size_t) tempoSamples) % delay[(size_t) channel].size();
                    echoes[channel] = delay[(size_t) channel][readPosition];
                }
            }
            for (int channel = 0; channel < channels; ++channel)
            {
                auto value = modulated[(size_t) channel];
                if (settings.modulationType == 4 && extendedMix > 0)
                {
                    // Stereo rotating-speaker approximation: moving delay
                    // supplies Doppler modulation, opposite gains supply pan.
                    const auto angle = chorusPhase + channel * twopi * 0.5;
                    const auto rotated = read(modulation[(size_t)channel], modulationPosition,
                        sampleRate * (0.004 + 0.0015 * std::sin(angle)));
                    const auto amplitude = float(0.75 + 0.25 * std::cos(angle));
                    value = value * (1.0f - extendedMix) + rotated * amplitude * extendedMix;
                }
                if (settings.modulationType == 3 && extendedMix > 0)
                {
                    float voices = 0;
                    for (int voice = 0; voice < 3; ++voice)
                    {
                        const double offset = twopi * (double(voice) / 3.0 + channel * 0.17);
                        const auto lfo = std::sin(chorusPhase + offset);
                        voices += read(modulation[(size_t)channel], modulationPosition,
                            sampleRate * (0.018 + 0.004 * lfo));
                    }
                    value = value * (1.0f - 0.5f*extendedMix) + voices*(extendedMix/6.0f);
                }
                if (tremolo > 0.0f) value *= 1.0f - tremolo + tremolo * tremoloLfo;
                auto& modLine = modulation[(size_t) channel];
                if (flanger > 0.0f)
                {
                    const auto tap = read(modLine, modulationPosition, sampleRate * (0.001 + 0.005 * flangerLfo));
                    value += flanger * 0.55f * tap;
                }
                if (chorus > 0.0f)
                {
                    const auto phase = channel == 0 ? chorusLfo : 1.0f - chorusLfo;
                    const auto tap = read(modLine, modulationPosition,
                                          sampleRate * (0.016 + 0.001 * chorusDepthMs * phase));
                    value = value * (1.0f - 0.35f * chorus) + tap * (0.35f * chorus);
                }
                if (driveAmount > 0.0001f)
                    value = applyAmp(value, driveAmount, settings.ampModel);
                modLine[modulationPosition] = settings.modulationType >= 3
                    ? modulated[(size_t) channel] : value;
                auto& delayLine = delay[(size_t) channel];
                if (delayMix > 0.0f)
                {
                    const auto feedbackSource = settings.delayPingPong && channels > 1
                        ? echoes[1 - channel] : echoes[channel];
                    const auto filtered = filterFeedback(feedbackSource, channel,
                                                         settings.delayFeedbackFilter);
                    const auto feedbackInput = settings.delayAnalog
                        ? std::tanh(filtered * (1.0f + feedback * 1.4f)) : filtered;
                    delayLine[delayPosition] = value + feedbackInput * feedback;
                    value += delayMix * echoes[channel];
                }
                buffer.setSample(channel, frame, value);
            }
            if (delayMix > 0.0f)
                delayValidSamples = juce::jmin((int) delay[0].size(), delayValidSamples + 1);
            else
            {
                delayValidSamples = 0;
                for (auto& filter : feedbackFilterState) filter = {};
            }
            modulationPosition = (modulationPosition + 1) % modulation[0].size();
            delayPosition = (delayPosition + 1) % delay[0].size();
            if (tremolo > 0.0f)
                tremoloPhase = wrap(tremoloPhase + twopi * juce::jlimit(0.2f, 12.0f, settings.tremoloRate) / sampleRate);
            if (flanger > 0.0f)
                flangerPhase = wrap(flangerPhase + twopi * juce::jlimit(0.05f, 5.0f, settings.flangerRate) / sampleRate);
            if (chorus > 0.0f || (settings.modulationType >= 3 && extendedMix > 0))
                chorusPhase = wrap(chorusPhase + twopi * juce::jlimit(0.05f, 5.0f,
                    settings.modulationType >= 3 ? settings.modulationRate : settings.chorusRate) / sampleRate);
        }
    }

private:
    static double wrap(double phase) noexcept
    {
        return phase >= juce::MathConstants<double>::twoPi ? phase - juce::MathConstants<double>::twoPi : phase;
    }
    static float read(const std::vector<float>& line, size_t write, double distance) noexcept
    {
        const auto position = (double) write + (double) line.size() - juce::jlimit(1.0, (double) line.size() - 2.0, distance);
        const auto whole = (size_t) position % line.size();
        const auto next = (whole + 1) % line.size();
        const auto fraction = (float) (position - std::floor(position));
        return line[whole] + (line[next] - line[whole]) * fraction;
    }
    double delayTimeSeconds(const NordEffectSettings& settings) const noexcept
    {
        const auto rate = (double) juce::jlimit(0.0f, 1.0f, settings.delayRate);
        double seconds = 0.02 * std::pow(100.0, rate); // REA-derived unsynced WX curve
        if (settings.delaySync)
        {
            // Exact DelayDivisions table read from the WX Stage-IV binary.
            static constexpr double divisions[13] { 2.0, 1.5, 1.0, 2.0 / 3.0,
                0.75, 0.5, 7.0 / 12.0, 1.0 / 3.0, 3.0 / 8.0, 0.25,
                1.0 / 6.0, 3.0 / 16.0, 0.125 };
            const auto index = juce::jlimit(0, 12, juce::roundToInt(rate * 12.0));
            seconds = divisions[index] * 60.0 / juce::jlimit(30.0f, 300.0f, settings.delayBpm);
        }
        return juce::jlimit(2.0 / sampleRate, 2.5, seconds);
    }
    float filterFeedback(float input, int channel, int mode) noexcept
    {
        auto& state = feedbackFilterState[(size_t) juce::jlimit(0, 1, channel)];
        const auto low = state.low += 0.22f * (input - state.low);
        const auto high = input - state.dc;
        state.dc = input * 0.995f + state.dc * 0.005f;
        switch (juce::jlimit(0, 3, mode))
        {
            case 1: return low;
            case 2: return high;
            case 3:
                state.bandDc += 0.22f * (high - state.bandDc);
                return state.bandDc;
            default: return input;
        }
    }
    static float applyAmp(float input, float amount, int model) noexcept
    {
        const auto gain = 1.0f + 11.0f * amount;
        const auto drive = input * gain;
        const auto shaped = model == 1 ? std::atan(drive) * (2.0f / juce::MathConstants<float>::pi)
            : model == 2 ? drive / (1.0f + std::abs(drive))
            : std::tanh(drive);
        const auto norm = model == 1 ? std::atan(gain) * (2.0f / juce::MathConstants<float>::pi)
            : model == 2 ? gain / (1.0f + gain) : std::tanh(gain);
        const auto saturated = shaped / juce::jmax(0.1f, norm);
        return input * (1.0f - amount) + saturated * amount;
    }
    struct FeedbackFilterState { float low = 0.0f, dc = 0.0f, bandDc = 0.0f; };
    NordModulation extendedModulation;
    std::array<std::vector<float>, 2> modulation, delay;
    std::array<FeedbackFilterState, 2> feedbackFilterState {};
    juce::SmoothedValue<float> tremoloMixSmoother, flangerMixSmoother;
    juce::SmoothedValue<float> chorusMixSmoother, extendedMixSmoother, delayMixSmoother;
    juce::SmoothedValue<float> delayFeedbackSmoother, driveSmoother;
    size_t modulationPosition = 0, delayPosition = 0;
    int delayValidSamples = 0;
    bool delayWasActive = false;
    double sampleRate = 44100.0, tremoloPhase = 0.0, flangerPhase = 0.0, chorusPhase = 0.0;
};

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
    std::array<float, 3> cachedSafeFrequencies {};
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

        const std::array<float, 3> safeFrequencies { safeLow, safeMid, safeHigh };
        // A moving knob changes one band at a time. Keep the other filters'
        // coefficients and state untouched on the real-time audio thread.
        for (size_t band = 0; band < coefficients.size(); ++band)
            if (!coefficientsReady || !sameRate
                || parameters[band] != cachedParameters[band]
                || parameters[band + 6] != cachedParameters[band + 6]
                || safeFrequencies[band] != cachedSafeFrequencies[band])
                coefficients[band] = makePeaking(safeFrequencies[band], parameters[band],
                                                 parameters[band + 6]);
        cachedSampleRate = sampleRate;
        cachedParameters = parameters;
        cachedSafeFrequencies = safeFrequencies;
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
