#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstdint>

#if JUCE_MAC
 #include <mach/mach.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

// Physical memory the system can hand out right now without swapping: free plus reclaimable
// (inactive, purgeable, speculative) pages on macOS, available physical memory on Windows.
// Returns 0 when it cannot be determined, which callers treat as "no extra limit".
namespace SystemMemory
{
inline std::uint64_t availableBytes() noexcept
{
#if JUCE_MAC
    vm_statistics64_data_t stats {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS)
        return 0;
    vm_size_t pageSize = 0;
    if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS || pageSize == 0) return 0;
    const auto pages = static_cast<std::uint64_t>(stats.free_count) + stats.inactive_count
                     + stats.purgeable_count + stats.speculative_count;
    return pages * static_cast<std::uint64_t>(pageSize);
#elif JUCE_WINDOWS
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<std::uint64_t>(status.ullAvailPhys) : 0;
#else
    return 0;
#endif
}

// Caps a "fraction of installed RAM" budget by what is actually free, but never below 1.5 GiB so
// a machine with a full file cache can still import a normal instrument.
inline std::uint64_t limitByAvailable(std::uint64_t budgetBytes) noexcept
{
    const auto available = availableBytes();
    if (available == 0) return budgetBytes;
    constexpr std::uint64_t floorBytes = 1536ull * 1024ull * 1024ull;
    return std::min(budgetBytes, std::max(available, floorBytes));
}
}
