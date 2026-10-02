#include "MixerState.h"

#include <algorithm>
#include <cmath>

namespace classicplayer
{
namespace
{
constexpr float minimumGainDb = -120.0f;
constexpr float maximumGainDb = 12.0f;

float decibelsToGain(float decibels) noexcept
{
    return decibels <= minimumGainDb ? 0.0f : std::pow(10.0f, decibels / 20.0f);
}
}

bool MixerChannel::isAudible(bool anotherChannelIsSoloed) const noexcept
{
    return !muted && (!anotherChannelIsSoloed || solo);
}

float MixerChannel::linearGain() const noexcept
{
    return decibelsToGain(gainDb);
}

void MixerState::clear()
{
    channels.clear();
    masterGainDbValue = 0.0f;
    masterPeakValue = 0.0f;
}

void MixerState::syncFromSession(const juce::Array<SessionTrack>& tracks)
{
    channels.clear();
    channels.reserve(static_cast<size_t>(tracks.size()));
    for (const auto& track : tracks)
    {
        MixerChannel channel;
        channel.name = track.name;
        channel.gainDb = track.volume <= 0.0f
            ? minimumGainDb
            : juce::jlimit(minimumGainDb, maximumGainDb,
                          20.0f * std::log10(track.volume));
        channel.pan = juce::jlimit(-1.0f, 1.0f, track.pan);
        channel.muted = track.muted;
        channel.solo = track.solo;
        channels.push_back(std::move(channel));
    }
    if (channels.empty()) addChannel("Track 1");
}

MixerChannel& MixerState::get(int index)
{
    jassert(juce::isPositiveAndBelow(index, size()));
    return channels[static_cast<size_t>(index)];
}

const MixerChannel& MixerState::get(int index) const
{
    jassert(juce::isPositiveAndBelow(index, size()));
    return channels[static_cast<size_t>(index)];
}

void MixerState::addChannel(const juce::String& name, bool instrument)
{
    MixerChannel channel;
    channel.name = name.isNotEmpty() ? name : "Track " + juce::String(size() + 1);
    channel.gainDb = instrument ? 0.0f : 0.0f;
    channels.push_back(std::move(channel));
}

bool MixerState::removeChannel(int index)
{
    if (!juce::isPositiveAndBelow(index, size())) return false;
    channels.erase(channels.begin() + index);
    if (channels.empty()) addChannel("Track 1");
    return true;
}

bool MixerState::anySoloed() const noexcept
{
    return std::any_of(channels.begin(), channels.end(),
                       [](const auto& channel) { return channel.solo; });
}

void MixerState::setMasterGainDb(float value) noexcept
{
    masterGainDbValue = juce::jlimit(minimumGainDb, maximumGainDb, value);
}

float MixerState::masterLinearGain() const noexcept
{
    return decibelsToGain(masterGainDbValue);
}

void MixerState::setMasterPeak(float value) noexcept
{
    masterPeakValue = juce::jlimit(0.0f, 1.0f, value);
}
}
