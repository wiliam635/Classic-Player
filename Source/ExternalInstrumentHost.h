#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

// Hosts one external virtual instrument for a Classic Player layer.
// Instances are created only from the standalone application's UI thread.
class ExternalInstrumentHost
{
public:
    ExternalInstrumentHost();
    ~ExternalInstrumentHost();

    juce::Result loadInstrument(const juce::File& pluginFile,
                                double sampleRate,
                                int maximumBlockSize);
    void unload();

    // Move a loaded instrument when a layer is removed and the final layer
    // takes its place. The destination keeps its own registered formats.
    void moveFrom(ExternalInstrumentHost& source);

    bool isLoaded() const noexcept { return instance != nullptr; }
    juce::String getName() const;
    juce::String getPath() const { return pluginPath; }

    void prepare(double sampleRate, int maximumBlockSize);
    void releaseResources();
    void process(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi);

    bool hasEditor() const;
    juce::AudioProcessorEditor* createEditor();

private:
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription description;
    juce::String pluginPath;
    double preparedSampleRate = 44100.0;
    int preparedBlockSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExternalInstrumentHost)
};
