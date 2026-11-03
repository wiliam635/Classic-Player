#pragma once
#include <array>
#include <algorithm>

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
};
class NordMorphState
{
public:
    void reset() noexcept { controls = {}; }
    void controller(int number, int value) noexcept
    {
        const float normalized = std::clamp(value, 0, 127)/127.0f;
        if (number == 1) controls[0] = normalized;
        else if (number == 11) controls[2] = normalized;
        else if (number == 121) reset();
    }
    void pressure(int value) noexcept { controls[1] = std::clamp(value, 0, 127)/127.0f; }
    float apply(float base, const NordMorphDepth& depth, float maximum) const noexcept
    { return depth.active() ? std::clamp(base + maximum*depth.offset(controls), 0.0f, maximum) : base; }
private:
    std::array<float, 3> controls {};
};
