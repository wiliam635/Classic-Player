#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

struct NordMorphDepth
{
    float wheel = 0, pressure = 0, pedal = 0;
    bool active() const noexcept { return wheel != 0 || pressure != 0 || pedal != 0; }
    float offset(const std::array<float, 3>& controls) const noexcept
    { return wheel*controls[0] + pressure*controls[1] + pedal*controls[2]; }
};
struct NordMorphSettings
{
    NordMorphDepth volume, cutoff;
    NordMorphDepth tremoloAmount, tremoloRate, flangerAmount, flangerRate;
    NordMorphDepth chorusAmount, chorusRate, modulationAmount, modulationRate;
    NordMorphDepth delayMix, delayFeedback, delayRate, ampDrive, ampMidFrequency, reverb;
    NordMorphDepth synthResonance, synthFilterEnvelope, synthLfoAmount, synthLfoRate;
    NordMorphDepth synthOscControl, synthOscEnvelope;
    NordMorphDepth arpRate, arpRange;
    NordMorphDepth leslieSpeed;
    std::array<NordMorphDepth, 9> drawbars {};
    bool active() const noexcept
    {
        return volume.active() || cutoff.active() || tremoloAmount.active() || tremoloRate.active()
            || flangerAmount.active() || flangerRate.active() || chorusAmount.active() || chorusRate.active()
            || modulationAmount.active() || modulationRate.active() || delayMix.active()
            || delayFeedback.active() || delayRate.active() || ampDrive.active() || ampMidFrequency.active() || reverb.active()
            || synthResonance.active() || synthFilterEnvelope.active() || synthLfoAmount.active()
            || synthLfoRate.active() || synthOscControl.active() || synthOscEnvelope.active()
            || arpRate.active() || arpRange.active() || leslieSpeed.active()
            || std::any_of(drawbars.begin(), drawbars.end(), [](const auto& depth) { return depth.active(); });
    }
};
class NordMorphState
{
public:
    void reset() noexcept { controls = {}; targets = {}; }
    void controller(int number, int value) noexcept
    {
        const float normalized = std::clamp(value, 0, 127)/127.0f;
        if (number == 1) targets[0] = normalized;
        else if (number == 11) targets[2] = normalized;
        else if (number == 121) reset();
    }
    void pressure(int value) noexcept { targets[1] = std::clamp(value, 0, 127)/127.0f; }
    void advance(int frames, double sampleRate) noexcept
    {
        if (frames <= 0 || sampleRate <= 0.0) return;
        const auto smoothing = 1.0f - std::exp(-static_cast<float>(frames / (sampleRate * 0.01)));
        for (std::size_t i = 0; i < controls.size(); ++i)
            controls[i] += (targets[i] - controls[i]) * smoothing;
    }
    float apply(float base, const NordMorphDepth& depth, float maximum) const noexcept
    { return depth.active() ? std::clamp(base + maximum*depth.offset(controls), 0.0f, maximum) : base; }
    float applySigned(float base, const NordMorphDepth& depth, float magnitude) const noexcept
    { return depth.active() ? std::clamp(base + magnitude*depth.offset(controls), -magnitude, magnitude) : base; }
private:
    std::array<float, 3> controls {};
    std::array<float, 3> targets {};
};
