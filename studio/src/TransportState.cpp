#include "TransportState.h"

#include <algorithm>
#include <cmath>

namespace classicplayer
{
void TransportState::setSampleRate(double value) noexcept
{
    const auto safeValue = std::isfinite(value) && value > 0.0 ? value : 44100.0;
    sampleRateValue.store(safeValue, std::memory_order_release);
}

void TransportState::stop() noexcept
{
    playing.store(false, std::memory_order_release);
    samplePosition.store(0, std::memory_order_release);
}

void TransportState::setPositionSeconds(double value) noexcept
{
    const auto safeValue = std::isfinite(value) ? std::max(0.0, value) : 0.0;
    setPositionSamples(static_cast<std::int64_t>(std::llround(safeValue * sampleRate())));
}

void TransportState::setPositionSamples(std::int64_t value) noexcept
{
    samplePosition.store(std::max<std::int64_t>(0, value), std::memory_order_release);
}

void TransportState::advanceSamples(std::int64_t samples) noexcept
{
    if (!isPlaying() || samples <= 0) return;
    samplePosition.fetch_add(samples, std::memory_order_acq_rel);
}

void TransportState::advance(double seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    advanceSamples(static_cast<std::int64_t>(std::llround(seconds * sampleRate())));
}

double TransportState::position() const noexcept
{
    return static_cast<double>(positionSamples()) / sampleRate();
}
}
