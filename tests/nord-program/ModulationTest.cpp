#include "../../Source/NordModulation.h"
#include <iostream>
#include "../../Source/NordMorph.h"
#include "../../Source/NordSplit.h"
#include <stdexcept>
int main()
{
    try
    {
        for (int width : {6, 12})
        {
            NordSplit lower, upper;
            lower.highPoint = upper.lowPoint = 60;
            lower.highWidth = upper.lowWidth = width;
            for (int note = 0; note < 128; ++note)
                if (std::abs(lower.gain(note) + upper.gain(note) - 1) > 1.e-6f)
                    throw std::runtime_error("split overlap has a hole or double gain");
            if (lower.gain(60-width) != 1 || lower.gain(60+width) != 0
                || upper.gain(60-width) != 0 || upper.gain(60+width) != 1)
                throw std::runtime_error("split width mismatch");
        }
        NordMorphState morph;
        NordMorphDepth positive {1, 0, 0}, negative {-1, 0, 0};
        if (morph.apply(0, positive, 1) != 0) throw std::runtime_error("morph initial value");
        morph.controller(1, 127);
        if (morph.apply(0, positive, 1) != 1 || morph.apply(1, negative, 1) != 0)
            throw std::runtime_error("signed volume morph failed");
        morph.controller(121, 0);
        if (morph.apply(0, positive, 1) != 0) throw std::runtime_error("CC121 did not reset morph");
        morph.pressure(127); morph.controller(11, 127);
        if (morph.apply(0, {0, 0.25f, 0.25f}, 100) != 50)
            throw std::runtime_error("pressure/pedal morph failed");
        if (morph.apply(2, {}, 1) != 2) throw std::runtime_error("inactive morph clipped legacy gain");
        for (const auto rate : {44100.0, 48000.0, 96000.0})
            for (const int type : {1, 2})
            {
                NordModulation effect;
                double difference = 0, stereo = 0;
                for (int i = 0; i < int(rate*2); ++i)
                {
                    const auto x = float(0.2*std::sin(i*0.057) + 0.1*std::sin(i*0.19));
                    const auto out = effect.process({x,x}, type, 1, 0.8f, rate);
                    if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || std::abs(out[0]) > 1 || std::abs(out[1]) > 1)
                        throw std::runtime_error("unstable allpass modulation");
                    difference += std::abs(out[0]-x);
                    stereo += std::abs(out[0]-out[1]);
                }
                if (difference < 1 || stereo < 1) throw std::runtime_error("inaudible modulation");
                effect.reset();
                for (int i = 0; i < 2048; ++i)
                    if (effect.process({0,0}, type, 1, 0.8f, rate) != std::array<float,2>{0,0})
                        throw std::runtime_error("reset leaked old audio");
                if (effect.process({0.2f,0.1f}, type, 0, 0.8f, rate) != std::array<float,2>{0.2f,0.1f})
                    throw std::runtime_error("bypass changed input");
            }
        std::cout << "Modulation stability, audible output, reset and bypass passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
