#include "../../Source/NordModulation.h"
#include <iostream>
#include <stdexcept>
int main()
{
    try
    {
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
