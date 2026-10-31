#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

using VelocityTable = std::array<std::uint8_t, 128>;

inline VelocityTable linearVelocityTable() noexcept
{
    VelocityTable table {};
    for (int value = 1; value < 128; ++value)
        table[(size_t) value] = static_cast<std::uint8_t>(value);
    return table;
}

inline VelocityTable shapedVelocityTable(int minimum, int maximum, float slope) noexcept
{
    minimum = std::clamp(minimum, 1, 127);
    maximum = std::clamp(maximum, minimum, 127);
    slope = std::clamp(slope, -100.0f, 100.0f);
    const auto exponent = std::exp2(-slope / 50.0f);
    VelocityTable table {};
    for (int value = 1; value < 128; ++value)
    {
        const auto input = (value - 1.0f) / 126.0f;
        const auto output = minimum + (maximum - minimum) * std::pow(input, exponent);
        table[(size_t) value] = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(output)), 1, 127));
    }
    return table;
}
