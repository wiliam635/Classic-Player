#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Uso: vst3_host_smoke_test <pasta Classic Player.vst3>\n";
        return 64;
    }

    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    const juce::File bundle(juce::String::fromUTF8(argv[1]));
    if (!bundle.isDirectory())
    {
        std::cerr << "Bundle VST3 nao encontrado\n";
        return 1;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    if (descriptions.isEmpty())
    {
        std::cerr << "O host nao descobriu nenhum instrumento no bundle VST3\n";
        return 2;
    }

    for (const auto* description : descriptions)
    {
        juce::String error;
        auto instance = format.createInstanceFromDescription(*description, 44100.0, 512, error);
        if (instance == nullptr)
        {
            std::cerr << "Falha ao instanciar " << description->name.toStdString()
                      << ": " << error.toStdString() << "\n";
            return 3;
        }
        std::cout << "Host descobriu e instanciou: " << description->name.toStdString() << "\n";
    }
    return 0;
}
