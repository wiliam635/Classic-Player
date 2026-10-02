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
        content->setSize(900, 500);

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
            {
                captureLoadedInstrumentState();
                pluginSummary.setText("Carregado: " + instrumentHost.pluginName(),
                                      juce::dontSendNotification);
            }
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
            syncMixerControlsFromState();
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
                    owner.syncMixerControlsFromState();
                    owner.instrumentHost.unload();
                    owner.availableInstruments.clear();
                    const auto restored = owner.restoreInstrumentFromSession();
                    owner.pluginSummary.setText(
                        restored ? "Sessão aberta e instrumento restaurado: " + result.getFileName()
                                 : "Sessão aberta: " + result.getFileName(),
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

                owner.syncSessionFromMixer();
                owner.captureLoadedInstrumentState();
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

        recordAudio.setButtonText("RECORD INPUT");
        recordAudio.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser>(
                "Gravar entrada de áudio", juce::File(), "*.wav");
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

                if (! owner.audioEngine.isRunning())
                {
                    juce::String startError;
                    if (! owner.audioEngine.start(owner.sessionState.sampleRate, 512, startError))
                    {
                        owner.recordingSummary.setText("Gravação: falha ao iniciar áudio · " + startError,
                                                       juce::dontSendNotification);
                        return;
                    }
                }

                juce::String error;
                if (owner.audioEngine.startRecording(result, error))
                    owner.recordingSummary.setText("Gravando: "
                                                       + owner.audioEngine.recordingFile().getFileName(),
                                                   juce::dontSendNotification);
                else
                    owner.recordingSummary.setText("Gravação: falha · " + error,
                                                   juce::dontSendNotification);
            });
        };
        recordAudio.setBounds(30, 360, 130, 36);
        content->addAndMakeVisible(recordAudio);

        stopRecording.setButtonText("STOP RECORDING");
        stopRecording.onClick = [this]
        {
            audioEngine.stopRecording();
            recordingSummary.setText("Gravação: parada", juce::dontSendNotification);
        };
        stopRecording.setBounds(170, 360, 145, 36);
        content->addAndMakeVisible(stopRecording);

        recordingSummary.setText("Gravação: pronta", juce::dontSendNotification);
        recordingSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        recordingSummary.setBounds(330, 364, 540, 28);
        content->addAndMakeVisible(recordingSummary);

        trackGainLabel.setText("TRACK GAIN", juce::dontSendNotification);
        trackPanLabel.setText("TRACK PAN", juce::dontSendNotification);
        masterGainLabel.setText("MASTER", juce::dontSendNotification);
        for (auto* label : { &trackGainLabel, &trackPanLabel, &masterGainLabel })
        {
            label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            content->addAndMakeVisible(*label);
        }
        trackGainLabel.setBounds(30, 408, 120, 22);
        trackPanLabel.setBounds(230, 408, 120, 22);
        masterGainLabel.setBounds(430, 408, 120, 22);

        setupSlider(trackGain, -60.0, 12.0, 0.0, " dB");
        setupSlider(trackPan, -1.0, 1.0, 0.0, "");
        setupSlider(masterGain, -60.0, 12.0, 0.0, " dB");
        trackGain.setBounds(30, 432, 170, 28);
        trackPan.setBounds(230, 432, 170, 28);
        masterGain.setBounds(430, 432, 170, 28);
        content->addAndMakeVisible(trackGain);
        content->addAndMakeVisible(trackPan);
        content->addAndMakeVisible(masterGain);

        muteTrack.setButtonText("MUTE TRACK 1");
        soloTrack.setButtonText("SOLO TRACK 1");
        muteTrack.setClickingTogglesState(true);
        soloTrack.setClickingTogglesState(true);
        muteTrack.onClick = [this]
        {
            if (mixerState.size() > 0)
                mixerState.get(0).muted = muteTrack.getToggleState();
            syncSessionFromMixer();
            audioEngine.refreshMixerSnapshot();
        };
        soloTrack.onClick = [this]
        {
            if (mixerState.size() > 0)
                mixerState.get(0).solo = soloTrack.getToggleState();
            syncSessionFromMixer();
            audioEngine.refreshMixerSnapshot();
        };
        muteTrack.setBounds(620, 412, 130, 28);
        soloTrack.setBounds(760, 412, 120, 28);
        content->addAndMakeVisible(muteTrack);
        content->addAndMakeVisible(soloTrack);

        meterSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        meterSummary.setBounds(30, 474, 840, 24);
        content->addAndMakeVisible(meterSummary);

        startTimerHz(20);
        setContentOwned(content, true);
        centreWithSize(900, 500);
        setResizable(true, true);
        setUsingNativeTitleBar(true);
    }
private:
    void setupSlider(juce::Slider& slider, double minimum, double maximum,
                     double initial, const juce::String& suffix)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        slider.setRange(minimum, maximum, 0.01);
        slider.setValue(initial, juce::dontSendNotification);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this]
        {
            if (mixerState.size() <= 0)
                return;

            auto& channel = mixerState.get(0);
            channel.gainDb = static_cast<float>(trackGain.getValue());
            channel.pan = static_cast<float>(trackPan.getValue());
            mixerState.setMasterGainDb(static_cast<float>(masterGain.getValue()));
            syncSessionFromMixer();
            audioEngine.refreshMixerSnapshot();
        };
    }

    void syncSessionFromMixer()
    {
        if (sessionState.tracks.isEmpty() || mixerState.size() <= 0)
            return;

        const auto& channel = mixerState.get(0);
        auto& track = sessionState.tracks.getReference(0);
        track.volume = channel.linearGain();
        track.pan = channel.pan;
        track.muted = channel.muted;
        track.solo = channel.solo;
    }

    void syncMixerControlsFromState()
    {
        if (mixerState.size() <= 0)
            return;

        const auto& channel = mixerState.get(0);
        trackGain.setValue(channel.gainDb, juce::dontSendNotification);
        trackPan.setValue(channel.pan, juce::dontSendNotification);
        masterGain.setValue(mixerState.masterGainDb(), juce::dontSendNotification);
        muteTrack.setToggleState(channel.muted, juce::dontSendNotification);
        soloTrack.setToggleState(channel.solo, juce::dontSendNotification);
        audioEngine.refreshMixerSnapshot();
    }

    void timerCallback() override
    {
        audioEngine.refreshMixerSnapshot();
        refresh();
    }
    void captureLoadedInstrumentState()
    {
        if (! instrumentHost.isLoaded() || sessionState.tracks.isEmpty())
            return;

        auto& track = sessionState.tracks.getReference(0);
        const auto& description = instrumentHost.description();
        track.instrument = true;
        track.instrumentFormat = description.pluginFormatName;
        track.instrumentIdentifier = description.fileOrIdentifier;
        track.instrumentName = instrumentHost.pluginName();

        juce::MemoryBlock state;
        if (instrumentHost.saveState(state))
            track.instrumentStateBase64 = state.toBase64Encoding();
    }

    bool restoreInstrumentFromSession()
    {
        if (sessionState.tracks.isEmpty())
            return false;

        const auto& track = sessionState.tracks.getReference(0);
        if (track.instrumentIdentifier.isEmpty())
            return false;

        if (availableInstruments.isEmpty())
            availableInstruments = instrumentHost.scanInstalledInstruments();

        for (const auto& description : availableInstruments)
        {
            if (description.fileOrIdentifier != track.instrumentIdentifier
                || description.pluginFormatName != track.instrumentFormat)
                continue;

            juce::String error;
            if (! instrumentHost.load(description, sessionState.sampleRate, 512, error))
                return false;

            juce::MemoryBlock state;
            if (track.instrumentStateBase64.isNotEmpty()
                && state.fromBase64Encoding(track.instrumentStateBase64))
                instrumentHost.restoreState(state.getData(), static_cast<int>(state.getSize()));
            return true;
        }

        return false;
    }

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
        if (mixerState.size() > 0)
        {
            const auto& channel = mixerState.get(0);
            if (! trackGain.isMouseButtonDown())
                trackGain.setValue(channel.gainDb, juce::dontSendNotification);
            if (! trackPan.isMouseButtonDown())
                trackPan.setValue(channel.pan, juce::dontSendNotification);
            if (! masterGain.isMouseButtonDown())
                masterGain.setValue(mixerState.masterGainDb(), juce::dontSendNotification);
            muteTrack.setToggleState(channel.muted, juce::dontSendNotification);
            soloTrack.setToggleState(channel.solo, juce::dontSendNotification);
        }
        meterSummary.setText("METERS · track pre "
                                 + juce::String(audioEngine.channelPreFaderPeak(), 3)
                                 + " · track post "
                                 + juce::String(audioEngine.channelPostFaderPeak(), 3)
                                 + " · master "
                                 + juce::String(audioEngine.masterPeak(), 3),
                             juce::dontSendNotification);
    }
    Session& sessionState;
    TransportState& transportState;
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    AudioEngine& audioEngine;
    juce::Label title, status, mixerSummary;
    juce::Label trackGainLabel, trackPanLabel, masterGainLabel, meterSummary;
    juce::Label pluginSummary;
    juce::Label audioSummary;
    juce::Label recordingSummary;
    juce::TextButton scanPlugins, loadPlugin, newSession, openSession, saveSession;
    juce::TextButton startAudio, stopAudio;
    juce::TextButton play, pause, stop;
    juce::TextButton recordAudio, stopRecording;
    juce::Slider trackGain, trackPan, masterGain;
    juce::ToggleButton muteTrack, soloTrack;
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
