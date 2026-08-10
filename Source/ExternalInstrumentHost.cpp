#include "ExternalInstrumentHost.h"

ExternalInstrumentHost::ExternalInstrumentHost()
{
    // JUCE registers only formats enabled by the target compile definitions.
    // The Classic Player enables VST3 on Windows and VST3/AU on macOS.
    juce::addDefaultFormatsToManager(formatManager);
}

ExternalInstrumentHost::~ExternalInstrumentHost()
{
    unload();
}

juce::Result ExternalInstrumentHost::loadInstrument(const juce::File& pluginFile,
                                                    double sampleRate,
                                                    int maximumBlockSize)
{
    if (!pluginFile.exists())
        return juce::Result::fail("Instrumento virtual não encontrado.");

    juce::OwnedArray<juce::PluginDescription> candidates;
    const auto identifier = pluginFile.getFullPathName();

    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType(identifier))
            format->findAllTypesForFile(candidates, identifier);

    auto* selected = candidates.getFirst();
    if (selected == nullptr)
        return juce::Result::fail("O arquivo não contém um VST3/AU compatível.");

    if (!selected->isInstrument)
        return juce::Result::fail("O plugin selecionado é um efeito. Escolha um instrumento virtual.");

    juce::String error;
    auto next = formatManager.createPluginInstance(*selected, sampleRate, maximumBlockSize, error);
    if (next == nullptr)
        return juce::Result::fail(error.isNotEmpty() ? error : "Não foi possível abrir o instrumento virtual.");

    unload();
    instance = std::move(next);
    description = *selected;
    pluginPath = identifier;
    prepare(sampleRate, maximumBlockSize);
    return juce::Result::ok();
}

void ExternalInstrumentHost::unload()
{
    if (instance != nullptr)
    {
        instance->releaseResources();
        instance.reset();
    }
    description = {};
    pluginPath.clear();
}

juce::String ExternalInstrumentHost::getName() const
{
    return instance != nullptr ? description.name : juce::String{};
}

void ExternalInstrumentHost::prepare(double sampleRate, int maximumBlockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize = maximumBlockSize;
    if (instance != nullptr)
        instance->prepareToPlay(sampleRate, maximumBlockSize);
}

void ExternalInstrumentHost::releaseResources()
{
    if (instance != nullptr)
        instance->releaseResources();
}

void ExternalInstrumentHost::process(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi)
{
    output.clear();
    if (instance != nullptr)
        instance->processBlock(output, midi);
}

bool ExternalInstrumentHost::hasEditor() const
{
    return instance != nullptr && instance->hasEditor();
}

juce::AudioProcessorEditor* ExternalInstrumentHost::createEditor()
{
    return hasEditor() ? instance->createEditorIfNeeded() : nullptr;
}
