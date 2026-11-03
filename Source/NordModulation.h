#pragma once
#include <array>
#include <algorithm>
#include <cmath>

// Independent Classic implementations. These are not models measured against
// Nord hardware. All memory is fixed and processing performs no allocation.
class NordModulation
{
public:
    enum Type { off, phaser, vibe, ensemble, spin };
    void reset() noexcept { phase = 0; state = {}; coefficients = {}; increments = {}; countdown = 0; previousType = 0; initialized = false; }
    std::array<float, 2> process(std::array<float, 2> input, int type,
                                float mix, float rate, double sampleRate) noexcept
    {
        mix = std::clamp(mix, 0.0f, 1.0f);
        if ((type != phaser && type != vibe) || mix == 0) return input;
        if (previousType != type) { reset(); previousType = type; }
        constexpr double pi = 3.14159265358979323846;
        const double increment = 2*pi*std::clamp(rate, 0.05f, 12.0f)/sampleRate;
        if (countdown == 0)
        {
          for (int c = 0; c < 2; ++c)
          {
            const auto lfo = std::sin(phase + (c ? pi*0.5 : 0));

            // Unequal stage frequencies give the asymmetric notches of a
            // vibe; the phaser uses six evenly spaced all-pass stages.
            const int stages = type == vibe ? 4 : 6;
            for (int stage = 0; stage < stages; ++stage)
            {
                const double base = type == vibe ? (160.0 * std::pow(2.7, stage))
                                                  : (220.0 * std::pow(1.6, stage));
                const double hz = std::clamp(base * std::pow(3.0, lfo), 30.0, sampleRate * 0.4);
                const double t = std::tan(pi*hz/sampleRate);
                const float a = float((1-t)/(1+t));
                if (!initialized) coefficients[c][stage] = a;
                increments[c][stage] = (a - coefficients[c][stage]) / 32.0f;
            }
          }
          initialized = true;
          countdown = 32;
        }
        --countdown;
        for (int c = 0; c < 2; ++c)
        {
            float wet = input[c];
            for (int stage = 0; stage < (type == vibe ? 4 : 6); ++stage)
            {
                coefficients[c][stage] += increments[c][stage];
                const float a = coefficients[c][stage];
                const float y = -a*wet + state[c][stage];
                state[c][stage] = wet + a*y;
                wet = y;
            }
            input[c] = input[c]*(1 - 0.5f*mix) + wet*(0.5f*mix);
        }
        phase += increment;
        if (phase >= 2*pi) phase -= 2*pi;
        return input;
    }
private:
    std::array<std::array<float, 6>, 2> state {};
    std::array<std::array<float, 6>, 2> coefficients {}, increments {};
    int countdown = 0, previousType = 0;
    bool initialized = false;
    double phase = 0;
};
