#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: au_host_smoke_test <Classic Player.component>\n";
        return 64;
    }

    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    const juce::File bundle(juce::String::fromUTF8(argv[1]));
    if (!bundle.isDirectory())
    {
        std::cerr << "Audio Unit bundle not found\n";
        return 1;
    }

    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    if (descriptions.isEmpty())
    {
        std::cerr << "Host did not discover an instrument in the Audio Unit bundle\n";
        return 2;
    }

    for (const auto* description : descriptions)
    {
        juce::String error;
        auto instance = format.createInstanceFromDescription(*description, 44100.0, 512, error);
        if (instance == nullptr)
        {
            std::cerr << "Failed to instantiate " << description->name.toStdString()
                      << ": " << error.toStdString() << "\n";
            return 3;
        }
        std::cout << "Host discovered and instantiated: " << description->name.toStdString() << "\n";
    }
    return 0;
}
