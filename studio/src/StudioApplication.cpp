#include "StudioApplication.h"

namespace classicplayer
{
class StudioApplication::MainWindow final : public juce::DocumentWindow,
                                             private juce::Timer
{
public:
    MainWindow(Session& sessionToUse, TransportState& transportToUse,
               MixerState& mixerToUse, InstrumentHost& hostToUse,
               AudioEngine& audioToUse)
        : DocumentWindow("Classic Player Studio", juce::Colours::darkgrey, DocumentWindow::allButtons),
          sessionState(sessionToUse), transportState(transportToUse), mixerState(mixerToUse),
          instrumentHost(hostToUse), audioEngine(audioToUse)
    {
        auto* content = new juce::Component();
        content->setSize(900, 540);

        title.setText("CLASSIC PLAYER STUDIO", juce::dontSendNotification);
        title.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        title.setBounds(30, 24, 400, 36);
        content->addAndMakeVisible(title);

        status.setText("Sessão vazia · fundação da DAW", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        status.setBounds(30, 72, 620, 28);
        content->addAndMakeVisible(status);

        mixerSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        mixerSummary.setBounds(30, 102, 760, 24);
        content->addAndMakeVisible(mixerSummary);

        scanPlugins.setButtonText("SCAN INSTRUMENTS");
        scanPlugins.onClick = [this]
        {
            availableInstruments = instrumentHost.scanInstalledInstruments();
            pluginSummary.setText(availableInstruments.isEmpty()
                                      ? "Instrumentos: nenhum VST3/AU encontrado"
                                      : "Instrumentos: " + juce::String(availableInstruments.size())
                                            + " encontrado(s) · " + availableInstruments.getFirst().name,
                                  juce::dontSendNotification);
        };
        scanPlugins.setBounds(30, 168, 190, 36);
        content->addAndMakeVisible(scanPlugins);

        loadPlugin.setButtonText("LOAD FIRST INSTRUMENT");
        loadPlugin.onClick = [this]
        {
            audioEngine.stop();
            if (availableInstruments.isEmpty())
                availableInstruments = instrumentHost.scanInstalledInstruments();

            if (availableInstruments.isEmpty())
            {
                pluginSummary.setText("Instrumentos: faça uma varredura antes de carregar",
                                      juce::dontSendNotification);
                return;
            }

            juce::String error;
            if (instrumentHost.load(availableInstruments.getFirst(), sessionState.sampleRate, 512, error))
                pluginSummary.setText("Carregado: " + instrumentHost.pluginName(),
                                      juce::dontSendNotification);
            else
                pluginSummary.setText("Falha ao carregar: " + error,
                                      juce::dontSendNotification);
        };
        loadPlugin.setBounds(230, 168, 190, 36);
        content->addAndMakeVisible(loadPlugin);

        pluginSummary.setText("Instrumentos: varredura não executada", juce::dontSendNotification);
        pluginSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        pluginSummary.setBounds(435, 172, 355, 28);
        content->addAndMakeVisible(pluginSummary);

        newSession.setButtonText("NEW SESSION");
        newSession.onClick = [this]
        {
            audioEngine.stop();
            sessionState.clear();
            transportState.stop();
            transportState.setSampleRate(sessionState.sampleRate);
            mixerState.syncFromSession(sessionState.tracks);
            instrumentHost.unload();
            pluginSummary.setText("Sessão nova", juce::dontSendNotification);
        };
        newSession.setBounds(30, 268, 120, 36);
        content->addAndMakeVisible(newSession);

        openSession.setButtonText("OPEN SESSION");
        openSession.onClick = [this]
        {
            audioEngine.stop();
            auto chooser = std::make_shared<juce::FileChooser>(
                "Abrir sessão do Classic Player Studio", juce::File(), "*.cpsession");
            juce::Component::SafePointer<MainWindow> safeThis(this);
            chooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectFiles,
                                 [safeThis, chooser](const juce::FileChooser& completedChooser)
            {
                if (safeThis == nullptr)
                    return;

                auto& owner = *safeThis;
                const auto result = completedChooser.getResult();
                if (result == juce::File())
                    return;

                if (owner.sessionState.load(result))
                {
                    owner.transportState.stop();
                    owner.transportState.setSampleRate(owner.sessionState.sampleRate);
                    owner.mixerState.syncFromSession(owner.sessionState.tracks);
                    owner.instrumentHost.unload();
                    owner.pluginSummary.setText("Sessão aberta: " + result.getFileName(),
                                                juce::dontSendNotification);
                }
                else
                    owner.pluginSummary.setText("Não foi possível abrir a sessão",
                                                juce::dontSendNotification);
            });
        };
        openSession.setBounds(160, 268, 130, 36);
        content->addAndMakeVisible(openSession);

        saveSession.setButtonText("SAVE SESSION");
        saveSession.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser>(
                "Salvar sessão do Classic Player Studio", juce::File(), "*.cpsession");
            juce::Component::SafePointer<MainWindow> safeThis(this);
            chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                     | juce::FileBrowserComponent::canSelectFiles,
                                 [safeThis, chooser](const juce::FileChooser& completedChooser)
            {
                if (safeThis == nullptr)
                    return;

                auto& owner = *safeThis;
                const auto result = completedChooser.getResult();
                if (result == juce::File())
                    return;

                if (owner.sessionState.save(result))
                    owner.pluginSummary.setText("Sessão salva: " + result.getFileName(),
                                                juce::dontSendNotification);
                else
                    owner.pluginSummary.setText("Não foi possível salvar a sessão",
                                                juce::dontSendNotification);
            });
        };
        saveSession.setBounds(300, 268, 130, 36);
        content->addAndMakeVisible(saveSession);

        startAudio.setButtonText("START AUDIO");
        startAudio.onClick = [this]
        {
            juce::String error;
            if (audioEngine.start(sessionState.sampleRate, 512, error))
                audioSummary.setText("Áudio: ativo · " + juce::String(audioEngine.sampleRate(), 0)
                                         + " Hz / " + juce::String(audioEngine.bufferSize()) + " samples",
                                     juce::dontSendNotification);
            else
                audioSummary.setText("Áudio: falha · " + error, juce::dontSendNotification);
        };
        startAudio.setBounds(440, 268, 130, 36);
        content->addAndMakeVisible(startAudio);

        stopAudio.setButtonText("STOP AUDIO");
        stopAudio.onClick = [this]
        {
            audioEngine.stop();
            audioSummary.setText("Áudio: parado", juce::dontSendNotification);
        };
        stopAudio.setBounds(580, 268, 130, 36);
        content->addAndMakeVisible(stopAudio);

        audioSummary.setText("Áudio: parado", juce::dontSendNotification);
        audioSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        audioSummary.setBounds(440, 308, 350, 24);
        content->addAndMakeVisible(audioSummary);

        play.setButtonText("PLAY");
        play.onClick = [this] { transportState.play(); refresh(); };
        play.setBounds(30, 316, 100, 36);
        content->addAndMakeVisible(play);

        pause.setButtonText("PAUSE");
        pause.onClick = [this] { transportState.pause(); refresh(); };
        pause.setBounds(140, 316, 100, 36);
        content->addAndMakeVisible(pause);

        stop.setButtonText("STOP");
        stop.onClick = [this] { transportState.stop(); refresh(); };
        stop.setBounds(250, 316, 100, 36);
        content->addAndMakeVisible(stop);

        startTimerHz(20);
        setContentOwned(content, true);
        centreWithSize(900, 540);
        setResizable(true, true);
        setUsingNativeTitleBar(true);
    }
private:
    void timerCallback() override { refresh(); }
    void refresh()
    {
        status.setText(juce::String(transportState.isPlaying() ? "Tocando" : "Parado")
                           + " · posição " + juce::String(transportState.position(), 2)
                           + " s · " + juce::String(sessionState.tracks.size()) + " pista(s)",
                       juce::dontSendNotification);
        mixerSummary.setText("MIXER · " + juce::String(mixerState.size())
                                 + " canal(is) · master "
                                 + juce::String(mixerState.masterGainDb(), 1) + " dB"
                                 + (mixerState.anySoloed() ? " · solo ativo" : ""),
                             juce::dontSendNotification);
    }
    Session& sessionState;
    TransportState& transportState;
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    AudioEngine& audioEngine;
    juce::Label title, status, mixerSummary;
    juce::Label pluginSummary;
    juce::Label audioSummary;
    juce::TextButton scanPlugins, loadPlugin, newSession, openSession, saveSession;
    juce::TextButton startAudio, stopAudio;
    juce::TextButton play, pause, stop;
    juce::Array<juce::PluginDescription> availableInstruments;
};

StudioApplication::~StudioApplication() = default;

void StudioApplication::initialise(const juce::String&)
{
    transport.setSampleRate(session.sampleRate);
    mixer.syncFromSession(session.tracks);
    window = std::make_unique<MainWindow>(session, transport, mixer, instrumentHost, audioEngine);
    window->setVisible(true);
}

void StudioApplication::shutdown()
{
    audioEngine.stop();
    window.reset();
}
}
