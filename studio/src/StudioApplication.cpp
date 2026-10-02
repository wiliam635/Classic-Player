#include "StudioApplication.h"

namespace classicplayer
{
class StudioApplication::MainWindow final : public juce::DocumentWindow,
                                             private juce::Timer
{
public:
    MainWindow(StudioApplication& owner, Session& session, TransportState& transport,
               MixerState& mixer, InstrumentHost& host)
        : DocumentWindow("Classic Player Studio", juce::Colours::darkgrey, DocumentWindow::allButtons),
          app(owner), sessionState(session), transportState(transport), mixerState(mixer), instrumentHost(host)
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
            const auto found = instrumentHost.scanInstalledInstruments();
            pluginSummary.setText(found.isEmpty()
                                      ? "Instrumentos: nenhum VST3/AU encontrado"
                                      : "Instrumentos: " + juce::String(found.size())
                                            + " encontrado(s) · " + found.getFirst().name,
                                  juce::dontSendNotification);
        };
        scanPlugins.setBounds(30, 168, 190, 36);
        content->addAndMakeVisible(scanPlugins);

        pluginSummary.setText("Instrumentos: varredura não executada", juce::dontSendNotification);
        pluginSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        pluginSummary.setBounds(235, 172, 555, 28);
        content->addAndMakeVisible(pluginSummary);

        play.setButtonText("PLAY");
        play.onClick = [this] { transportState.play(); refresh(); };
        play.setBounds(30, 220, 100, 36);
        content->addAndMakeVisible(play);

        pause.setButtonText("PAUSE");
        pause.onClick = [this] { transportState.pause(); refresh(); };
        pause.setBounds(140, 220, 100, 36);
        content->addAndMakeVisible(pause);

        stop.setButtonText("STOP");
        stop.onClick = [this] { transportState.stop(); refresh(); };
        stop.setBounds(250, 220, 100, 36);
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
    StudioApplication& app;
    Session& sessionState;
    TransportState& transportState;
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    juce::Label title, status, mixerSummary;
    juce::Label pluginSummary;
    juce::TextButton scanPlugins, play, pause, stop;
};

StudioApplication::~StudioApplication() = default;

void StudioApplication::initialise(const juce::String&)
{
    transport.setSampleRate(session.sampleRate);
    mixer.syncFromSession(session.tracks);
    window = std::make_unique<MainWindow>(*this, session, transport, mixer, instrumentHost);
    window->setVisible(true);
}

void StudioApplication::shutdown() { window.reset(); }
}
