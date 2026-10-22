#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

// When the master limiter is bypassed, leave finite audio untouched (including
// peaks above 0 dBFS) while preventing NaN/Inf from reaching the host.
inline void sanitizeOutput(juce::AudioBuffer<float>& buffer) noexcept
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(samples[sample])) samples[sample] = 0.0f;
    }
}

// Final digital ceiling after the master limiter. Normally the limiter does
// all gain reduction; this guard catches overshoots and non-finite samples
// before they reach the host, audio device, or recording writer.
inline void applyOutputSafety(juce::AudioBuffer<float>& buffer, float ceilingDb) noexcept
{
    const auto ceiling = juce::Decibels::decibelsToGain(juce::jlimit(-12.0f, 0.0f, ceilingDb));
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = samples[sample];
            samples[sample] = std::isfinite(value)
                ? juce::jlimit(-ceiling, ceiling, value) : 0.0f;
        }
    }
}
