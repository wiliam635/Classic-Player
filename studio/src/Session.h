#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace classicplayer
{
struct SessionTrack
{
    juce::String name { "Track 1" };
    bool instrument { true };
    bool muted { false };
    bool solo { false };
    float volume { 1.0f };
    float pan { 0.0f };

    juce::ValueTree toValueTree() const;
    static SessionTrack fromValueTree(const juce::ValueTree&);
};

class Session
{
public:
    static constexpr int currentFormatVersion = 1;

    Session();
    void clear();
    bool save(const juce::File&) const;
    bool load(const juce::File&);
    juce::ValueTree toValueTree() const;
    bool fromValueTree(const juce::ValueTree&);

    double tempoBpm { 120.0 };
    int numerator { 4 };
    int denominator { 4 };
    double sampleRate { 44100.0 };
    juce::Array<SessionTrack> tracks;
};
}
