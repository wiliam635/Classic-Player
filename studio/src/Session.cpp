#include "Session.h"

namespace classicplayer
{
namespace
{
constexpr auto sessionType = "ClassicPlayerStudioSession";
constexpr auto trackType = "Track";
}

juce::ValueTree SessionTrack::toValueTree() const
{
    juce::ValueTree tree(trackType);
    tree.setProperty("name", name, nullptr);
    tree.setProperty("instrument", instrument, nullptr);
    tree.setProperty("muted", muted, nullptr);
    tree.setProperty("solo", solo, nullptr);
    tree.setProperty("volume", volume, nullptr);
    tree.setProperty("pan", pan, nullptr);
    tree.setProperty("instrumentFormat", instrumentFormat, nullptr);
    tree.setProperty("instrumentIdentifier", instrumentIdentifier, nullptr);
    tree.setProperty("instrumentName", instrumentName, nullptr);
    tree.setProperty("instrumentStateBase64", instrumentStateBase64, nullptr);
    return tree;
}

SessionTrack SessionTrack::fromValueTree(const juce::ValueTree& tree)
{
    SessionTrack result;
    result.name = tree.getProperty("name", result.name).toString();
    result.instrument = static_cast<bool>(tree.getProperty("instrument", result.instrument));
    result.muted = static_cast<bool>(tree.getProperty("muted", result.muted));
    result.solo = static_cast<bool>(tree.getProperty("solo", result.solo));
    result.volume = static_cast<float>(tree.getProperty("volume", result.volume));
    result.pan = static_cast<float>(tree.getProperty("pan", result.pan));
    result.instrumentFormat = tree.getProperty("instrumentFormat", result.instrumentFormat).toString();
    result.instrumentIdentifier = tree.getProperty("instrumentIdentifier", result.instrumentIdentifier).toString();
    result.instrumentName = tree.getProperty("instrumentName", result.instrumentName).toString();
    result.instrumentStateBase64 = tree.getProperty("instrumentStateBase64", result.instrumentStateBase64).toString();
    return result;
}

Session::Session() { clear(); }

void Session::clear()
{
    tempoBpm = 120.0;
    numerator = 4;
    denominator = 4;
    sampleRate = 44100.0;
    tracks.clear();
    tracks.add(SessionTrack{});
}

juce::ValueTree Session::toValueTree() const
{
    juce::ValueTree tree(sessionType);
    tree.setProperty("formatVersion", currentFormatVersion, nullptr);
    tree.setProperty("tempoBpm", tempoBpm, nullptr);
    tree.setProperty("numerator", numerator, nullptr);
    tree.setProperty("denominator", denominator, nullptr);
    tree.setProperty("sampleRate", sampleRate, nullptr);
    for (const auto& track : tracks) tree.addChild(track.toValueTree(), -1, nullptr);
    return tree;
}

bool Session::fromValueTree(const juce::ValueTree& tree)
{
    if (!tree.isValid() || !tree.hasType(sessionType)) return false;
    const auto version = static_cast<int>(tree.getProperty("formatVersion", 0));
    if (version < 1 || version > currentFormatVersion) return false;

    tempoBpm = juce::jlimit(20.0, 300.0, static_cast<double>(tree.getProperty("tempoBpm", 120.0)));
    numerator = juce::jlimit(1, 32, static_cast<int>(tree.getProperty("numerator", 4)));
    denominator = juce::jlimit(1, 32, static_cast<int>(tree.getProperty("denominator", 4)));
    sampleRate = juce::jlimit(8000.0, 192000.0, static_cast<double>(tree.getProperty("sampleRate", 44100.0)));

    juce::Array<SessionTrack> loaded;
    for (int i = 0; i < tree.getNumChildren(); ++i)
        if (tree.getChild(i).hasType(trackType)) loaded.add(SessionTrack::fromValueTree(tree.getChild(i)));
    tracks = std::move(loaded);
    if (tracks.isEmpty()) tracks.add(SessionTrack{});
    return true;
}

bool Session::save(const juce::File& file) const
{
    if (file.getParentDirectory().createDirectory().failed()) return false;
    auto xml = toValueTree().createXml();
    return xml != nullptr && xml->writeTo(file);
}

bool Session::load(const juce::File& file)
{
    auto xml = juce::XmlDocument::parse(file);
    return xml != nullptr && fromValueTree(juce::ValueTree::fromXml(*xml));
}
}
