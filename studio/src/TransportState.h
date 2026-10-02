#pragma once

namespace classicplayer
{
class TransportState
{
public:
    void play() noexcept { playing = true; }
    void pause() noexcept { playing = false; }
    void stop() noexcept { playing = false; positionSeconds = 0.0; }
    void setPositionSeconds(double value) noexcept { positionSeconds = value < 0.0 ? 0.0 : value; }
    void advance(double seconds) noexcept { if (playing && seconds > 0.0) positionSeconds += seconds; }
    bool isPlaying() const noexcept { return playing; }
    double position() const noexcept { return positionSeconds; }
private:
    bool playing = false;
    double positionSeconds = 0.0;
};
}
