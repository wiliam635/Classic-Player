#pragma once
// Android replacement for the desktop SystemMemory.h: the decoder only needs limitByAvailable().
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

namespace SystemMemory
{
inline std::uint64_t availableBytes() noexcept
{
    std::ifstream in("/proc/meminfo");
    std::string key, unit; unsigned long long value = 0;
    while (in >> key >> value >> unit)
        if (key == "MemAvailable:") return static_cast<std::uint64_t>(value) * 1024ull;
    return 0;
}
inline std::uint64_t limitByAvailable(std::uint64_t wanted) noexcept
{
    const auto available = availableBytes();
    return available == 0 ? wanted : std::min(wanted, available);
}
}
