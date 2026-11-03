#pragma once
#include <algorithm>

// Stage 4 manual v1.6X, Keyboard Split: widths extend +/-6 or +/-12
// semitones from the boundary. Classic uses complementary linear gains.
struct NordSplit
{
    int lowPoint = -1, highPoint = -1;
    int lowWidth = 0, highWidth = 0;
    float gain(int inputNote) const noexcept
    {
        float value = 1;
        if (lowPoint >= 0 && lowWidth > 0)
            value *= std::clamp((inputNote - lowPoint + float(lowWidth)) / (2*lowWidth), 0.0f, 1.0f);
        if (highPoint >= 0 && highWidth > 0)
            value *= std::clamp((highPoint + float(highWidth) - inputNote) / (2*highWidth), 0.0f, 1.0f);
        return value;
    }
};
