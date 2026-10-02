#include "InstrumentHost.h"

#include <algorithm>

namespace classicplayer
{
InstrumentHost::InstrumentHost()
{
    // JUCE 9 exposes default-format registration as a free helper. It honors
    // the JUCE_PLUGINHOST_* definitions configured for the Studio target,
    // keeping VST3 first-class and enabling AU on macOS.
    juce::addDefaultFormatsToManager(formatManager);
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

juce::Array<juce::PluginDescription> InstrumentHost::scanInstalledInstruments() const
{
    juce::Array<juce::File> roots;

   #if JUCE_WINDOWS
    roots.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
    roots.add(juce::File("C:\\Program Files\\VST3"));
   #elif JUCE_MAC
    roots.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    roots.add(juce::File("/Library/Audio/Plug-Ins/Components"));
    const auto userPlugins = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                 .getChildFile("Library")
                                 .getChildFile("Audio")
                                 .getChildFile("Plug-Ins");
    roots.add(userPlugins.getChildFile("VST3"));
    roots.add(userPlugins.getChildFile("Components"));
   #endif

    juce::Array<juce::File> candidates;
    for (const auto& root : roots)
    {
        if (!root.isDirectory()) continue;
        root.findChildFiles(candidates, juce::File::findFilesAndDirectories, true, "*.vst3");
       #if JUCE_MAC
        root.findChildFiles(candidates, juce::File::findFilesAndDirectories, true, "*.component");
       #endif
    }

    juce::Array<juce::PluginDescription> result;
    for (const auto& candidate : candidates)
    {
        const auto identifier = candidate.getFullPathName();
        for (auto* format : formatManager.getFormats())
        {
            if (!format->fileMightContainThisPluginType(identifier)) continue;

            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile(descriptions, identifier);
            for (auto* description : descriptions)
            {
                if (description == nullptr || !description->isInstrument) continue;

                const auto alreadyAdded = std::any_of(result.begin(), result.end(),
                    [&description](const auto& existing)
                    {
                        return existing.fileOrIdentifier == description->fileOrIdentifier
                            && existing.pluginFormatName == description->pluginFormatName;
                    });
                if (!alreadyAdded) result.add(*description);
            }
        }
    }

    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right)
              {
                  return left.name.compareNatural(right.name) < 0;
              });
    return result;
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
