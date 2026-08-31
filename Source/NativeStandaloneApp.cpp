#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

// Keep JUCE's standalone audio, MIDI, settings and processor lifecycle, but
// configure the OS window decoration BEFORE the first visible frame.
class ClassicPlayerStandaloneApp final : public juce::JUCEApplication
{
public:
    ClassicPlayerStandaloneApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = juce::CharPointer_UTF8(JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName = "~/.config";
       #endif
        properties.setStorageParameters(options);
    }

    const juce::String getApplicationName() override { return juce::CharPointer_UTF8(JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}

    void initialise(const juce::String&) override
    {
        auto holder = createHolder();
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            headlessHolder = std::move(holder);
            return;
        }

        window = std::make_unique<juce::StandaloneFilterWindow>(getApplicationName(),
            juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
            std::move(holder));
        jassert(!window->isVisible());
        window->setUsingNativeTitleBar(true);
        jassert(window->isUsingNativeTitleBar());
        window->setVisible(true);
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr) window->getPluginHolder()->savePluginState();
        if (headlessHolder != nullptr) headlessHolder->savePluginState();
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            juce::Timer::callAfterDelay(100, []
            {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
            return;
        }
        quit();
    }

    void shutdown() override
    {
        headlessHolder.reset();
        window.reset();
        properties.saveIfNeeded();
    }

private:
    std::unique_ptr<juce::StandalonePluginHolder> createHolder()
    {
        juce::Array<juce::StandalonePluginHolder::PluginInOuts> channels;
       #ifdef JucePlugin_PreferredChannelConfigurations
        const juce::StandalonePluginHolder::PluginInOuts configurations[] { JucePlugin_PreferredChannelConfigurations };
        channels.addArray(configurations, juce::numElementsInArray(configurations));
       #endif
        return std::make_unique<juce::StandalonePluginHolder>(properties.getUserSettings(), false,
            juce::String{}, nullptr, channels, false);
    }

    juce::ApplicationProperties properties;
    std::unique_ptr<juce::StandaloneFilterWindow> window;
    std::unique_ptr<juce::StandalonePluginHolder> headlessHolder;
};

// JUCE's standalone wrapper supplies the platform entry point. Only replace
// its application factory; VST3/AU targets do not compile this source file.
JUCE_CREATE_APPLICATION_DEFINE(ClassicPlayerStandaloneApp)
