#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace classicplayer
{
/**
    Small host boundary used by Studio instrument tracks.

    The first Studio milestone hosts the already-tested Classic Player VST3
    (and AU on macOS) through JUCE.  The processor itself remains unchanged;
    this boundary makes it possible to replace file-based hosting with the
    shared Classic Player engine later without changing session or mixer code.
*/
class InstrumentHost final
{
public:
    InstrumentHost();
    ~InstrumentHost();

    InstrumentHost(const InstrumentHost&) = delete;
    InstrumentHost& operator=(const InstrumentHost&) = delete;

    bool load(const juce::PluginDescription&, double sampleRate, int maximumBlockSize,
              juce::String& errorMessage);
    // Scan standard VST3/AU locations for instrument descriptions that can
    // be presented by the Studio track selector.
    juce::Array<juce::PluginDescription> scanInstalledInstruments() const;
    void unload();

    void prepareToPlay(double sampleRate, int maximumBlockSize);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&);

    bool isLoaded() const noexcept { return instance != nullptr; }
    const juce::PluginDescription& description() const noexcept { return loadedDescription; }
    juce::String pluginName() const;

    bool saveState(juce::MemoryBlock& destination);
    bool restoreState(const void* data, int sizeInBytes);

private:
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription loadedDescription;
    bool prepared = false;
};
}
