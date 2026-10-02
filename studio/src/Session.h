#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace classicplayer
{
struct SessionClip
{
    juce::String name { "Audio Clip" };
    juce::String filePath;
    double startSeconds { 0.0 };
    double lengthSeconds { 0.0 };
    double sampleRate { 44100.0 };
    int numChannels { 2 };

    juce::ValueTree toValueTree() const;
    static SessionClip fromValueTree(const juce::ValueTree&);
};

struct SessionTrack
{
    juce::String name { "Track 1" };
    bool instrument { true };
    bool muted { false };
    bool solo { false };
    float volume { 1.0f };
    float pan { 0.0f };
    // Empty identifier means that the track is not connected to a plug-in
    // yet.  The state is stored as base64 so sessions remain self-contained
    // without copying large sample libraries or plug-in binaries.
    juce::String instrumentFormat { "Internal" };
    juce::String instrumentIdentifier;
    juce::String instrumentName;
    juce::String instrumentStateBase64;
    juce::Array<SessionClip> clips;

    juce::ValueTree toValueTree() const;
    static SessionTrack fromValueTree(const juce::ValueTree&);
};

class Session
{
public:
    static constexpr int currentFormatVersion = 2;

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
