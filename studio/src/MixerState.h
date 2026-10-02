#pragma once

#include "Session.h"
#include <vector>

namespace classicplayer
{
/** Runtime mixer state kept separate from a Classic Player layer's volume. */
struct MixerChannel
{
    juce::String name { "Track 1" };
    float gainDb { 0.0f };
    float pan { 0.0f };
    bool muted { false };
    bool solo { false };
    bool recordArmed { false };
    float preFaderPeak { 0.0f };
    float postFaderPeak { 0.0f };

    bool isAudible(bool anotherChannelIsSoloed) const noexcept;
    float linearGain() const noexcept;
};

class MixerState
{
public:
    void clear();
    void syncFromSession(const juce::Array<SessionTrack>& tracks);

    int size() const noexcept { return static_cast<int>(channels.size()); }
    MixerChannel& get(int index);
    const MixerChannel& get(int index) const;

    void addChannel(const juce::String& name, bool instrument = true);
    bool removeChannel(int index);

    bool anySoloed() const noexcept;
    float masterGainDb() const noexcept { return masterGainDbValue; }
    void setMasterGainDb(float value) noexcept;
    float masterLinearGain() const noexcept;
    void setMasterPeak(float value) noexcept;
    float masterPeak() const noexcept { return masterPeakValue; }

private:
    std::vector<MixerChannel> channels;
    float masterGainDbValue { 0.0f };
    float masterPeakValue { 0.0f };
};
}
