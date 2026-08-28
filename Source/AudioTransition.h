#pragma once
#include <algorithm>
#include <cmath>

// Remove only the discontinuity at a voice replacement/retarget. The new
// oscillator and envelope run immediately; no global attack or glide is added.
struct AudioTransition
{
    float last = 0.0f, correction = 0.0f;
    int remaining = 0, length = 1;
    bool pending = false;

    void retarget(double rate)
    {
        length = std::max(1, static_cast<int>(rate * 0.003));
        pending = true;
    }
    float process(float value)
    {
        if (pending)
        {
            correction = last - value;
            remaining = length;
            pending = false;
        }
        if (remaining > 0)
        {
            const auto t = static_cast<float>(remaining--) / static_cast<float>(length);
            value += correction * t * t * (3.0f - 2.0f * t);
        }
        return last = value;
    }
};
