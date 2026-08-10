#include "ExternalInstrumentHost.h"
#include <utility>
#include <algorithm>

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

juce::Array<juce::File> ExternalInstrumentHost::findInstalledInstruments()
{
    juce::Array<juce::File> roots;
   #if JUCE_WINDOWS
    roots.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
    const auto commonFiles = juce::SystemStats::getEnvironmentVariable("COMMONPROGRAMFILES", {});
    if (commonFiles.isNotEmpty())
        roots.addIfNotAlreadyThere(juce::File(commonFiles).getChildFile("VST3"));
   #elif JUCE_MAC
    roots.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    roots.add(juce::File("/Library/Audio/Plug-Ins/Components"));
    const auto userPlugins = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                 .getChildFile("Library").getChildFile("Audio").getChildFile("Plug-Ins");
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

    juce::Array<juce::File> result;
    for (const auto& candidate : candidates)
        if (!result.contains(candidate))
            result.add(candidate);

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
    {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    return result;
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

void ExternalInstrumentHost::moveFrom(ExternalInstrumentHost& source)
{
    if (this == &source) return;

    unload();
    instance = std::move(source.instance);
    description = source.description;
    pluginPath = source.pluginPath;
    preparedSampleRate = source.preparedSampleRate;
    preparedBlockSize = source.preparedBlockSize;

    source.description = {};
    source.pluginPath.clear();
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
