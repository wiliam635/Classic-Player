#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

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
