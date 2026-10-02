#pragma once

#include <atomic>
#include <cstdint>

namespace classicplayer
{
class TransportState
{
public:
    void setSampleRate(double value) noexcept;
    double sampleRate() const noexcept { return sampleRateValue.load(std::memory_order_relaxed); }

    void play() noexcept { playing.store(true, std::memory_order_release); }
    void pause() noexcept { playing.store(false, std::memory_order_release); }
    void stop() noexcept;

    void setPositionSeconds(double value) noexcept;
    void setPositionSamples(std::int64_t value) noexcept;
    // Called by the audio callback. The UI only observes this state.
    void advanceSamples(std::int64_t samples) noexcept;
    void advance(double seconds) noexcept;

    bool isPlaying() const noexcept { return playing.load(std::memory_order_acquire); }
    std::int64_t positionSamples() const noexcept { return samplePosition.load(std::memory_order_acquire); }
    double position() const noexcept;
private:
    std::atomic<bool> playing { false };
    std::atomic<std::int64_t> samplePosition { 0 };
    std::atomic<double> sampleRateValue { 44100.0 };
};
}
