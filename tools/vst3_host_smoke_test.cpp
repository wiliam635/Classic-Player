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
        if (!instance->hasEditor())
        {
            std::cerr << "VST3 sem editor grafico: " << description->name.toStdString() << "\n";
            return 4;
        }
        auto* editor = instance->createEditorIfNeeded();
        if (editor == nullptr)
        {
            std::cerr << "Host nao conseguiu abrir o editor VST3\n";
            return 5;
        }
        editor->setSize(1000, 650);
        if (editor->getWidth() != 1000 || editor->getHeight() != 650)
        {
            std::cerr << "Editor VST3 nao aceitou redimensionamento\n";
            return 6;
        }
        juce::DocumentWindow hostWindow("Classic Player VST3 smoke test",
                                        juce::Colours::black,
                                        juce::DocumentWindow::closeButton);
        hostWindow.setContentNonOwned(editor, true);
        hostWindow.centreWithSize(1000, 650);
        hostWindow.setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1500);
        const auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
        hostWindow.setVisible(false);
        hostWindow.clearContentComponent();
        if (!snapshot.isValid())
        {
            std::cerr << "Editor VST3 nao produziu imagem\n";
            return 7;
        }
        std::cout << "Host abriu, redimensionou e desenhou: "
                  << description->name.toStdString() << "\n";
    }
    return 0;
}
