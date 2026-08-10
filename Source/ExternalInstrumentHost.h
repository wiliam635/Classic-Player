#pragma once

#include <JuceHeader.h>

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
