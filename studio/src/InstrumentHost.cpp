#include "InstrumentHost.h"

namespace classicplayer
{
InstrumentHost::InstrumentHost()
{
    // The available formats are controlled by JUCE_PLUGINHOST_* definitions
    // in the Studio target.  This keeps VST3 first-class on both platforms and
    // enables AU when the target is built on macOS.
    formatManager.addDefaultFormats();
}

InstrumentHost::~InstrumentHost()
{
    unload();
}

bool InstrumentHost::load(const juce::PluginDescription& descriptionToLoad,
                          double sampleRate, int maximumBlockSize,
                          juce::String& errorMessage)
{
    unload();
    auto candidate = formatManager.createPluginInstance(descriptionToLoad,
                                                         sampleRate,
                                                         maximumBlockSize,
                                                         errorMessage);
    if (candidate == nullptr) return false;

    loadedDescription = descriptionToLoad;
    instance = std::move(candidate);
    prepareToPlay(sampleRate, maximumBlockSize);
    return true;
}

void InstrumentHost::unload()
{
    releaseResources();
    instance.reset();
    loadedDescription = {};
}

void InstrumentHost::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    if (instance == nullptr) return;
    instance->prepareToPlay(sampleRate, maximumBlockSize);
    prepared = true;
}

void InstrumentHost::releaseResources()
{
    if (instance != nullptr && prepared)
        instance->releaseResources();
    prepared = false;
}

void InstrumentHost::processBlock(juce::AudioBuffer<float>& audio,
                                  juce::MidiBuffer& midi)
{
    if (instance == nullptr)
    {
        audio.clear();
        return;
    }
    instance->processBlock(audio, midi);
}

juce::String InstrumentHost::pluginName() const
{
    return instance != nullptr ? instance->getName() : loadedDescription.name;
}

bool InstrumentHost::saveState(juce::MemoryBlock& destination)
{
    if (instance == nullptr) return false;
    destination.reset();
    instance->getStateInformation(destination);
    return destination.getSize() > 0;
}

bool InstrumentHost::restoreState(const void* data, int sizeInBytes)
{
    if (instance == nullptr || data == nullptr || sizeInBytes <= 0) return false;
    instance->setStateInformation(data, sizeInBytes);
    return true;
}
}
