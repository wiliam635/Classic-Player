#include "StudioApplication.h"
#include "TimelineView.h"

namespace classicplayer
{
namespace
{
// Keep user-facing Portuguese text explicitly UTF-8 on every platform.
juce::String cpText(const char* value)
{
    return juce::String::fromUTF8(value);
}

class StudioCanvas final : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xff08151e));

        g.setColour(juce::Colour(0xff0c202d));
        g.fillRoundedRectangle(bounds.reduced(14.0f).withHeight(84.0f), 10.0f);
        g.setColour(juce::Colour(0xff12c8cf));
        g.fillRoundedRectangle(bounds.reduced(14.0f).withHeight(3.0f).translated(0.0f, 95.0f), 1.5f);

        const auto panel = juce::Colour(0xff102632);
        const auto panelAlt = juce::Colour(0xff0d202c);
        g.setColour(panel);
        g.fillRoundedRectangle(14.0f, 112.0f, bounds.getWidth() - 28.0f, 104.0f, 10.0f);
        g.fillRoundedRectangle(14.0f, 228.0f, bounds.getWidth() - 28.0f, 142.0f, 10.0f);
        g.fillRoundedRectangle(14.0f, 382.0f, bounds.getWidth() - 28.0f, 120.0f, 10.0f);
        g.setColour(panelAlt);
        g.fillRoundedRectangle(14.0f, 514.0f, bounds.getWidth() - 28.0f,
                               juce::jmax(140.0f, bounds.getHeight() - 528.0f), 10.0f);

        g.setColour(juce::Colour(0xff1c4051));
        g.drawHorizontalLine(112, 28.0f, bounds.getWidth() - 28.0f);
        g.drawHorizontalLine(228, 28.0f, bounds.getWidth() - 28.0f);
        g.drawHorizontalLine(382, 28.0f, bounds.getWidth() - 28.0f);
        g.drawHorizontalLine(514, 28.0f, bounds.getWidth() - 28.0f);

        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xff52dbe0));
        g.drawText("STUDIO / INSTRUMENT", 30, 119, 220, 18, juce::Justification::left);
        g.drawText("SESSION / TRANSPORT", 30, 235, 220, 18, juce::Justification::left);
        g.drawText("MIXER / MONITORING", 30, 389, 220, 18, juce::Justification::left);
        g.drawText("ARRANGEMENT / TIMELINE", 30, 521, 260, 18, juce::Justification::left);
        g.setColour(juce::Colour(0xff8fb5bf));
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("AUDIO WORKSTATION", bounds.getWidth() - 190.0f, 39.0f, 150.0f, 20.0f,
                   juce::Justification::right);
    }
};

class StudioLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff08151e));
        setColour(juce::DocumentWindow::backgroundColourId, juce::Colour(0xff08151e));
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff183544));
        setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff0e9ea9));
        setColour(juce::TextButton::textColourOffId, juce::Colour(0xffedf6f8));
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff10232e));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff2a5263));
        setColour(juce::ComboBox::textColourId, juce::Colour(0xffedf6f8));
        setColour(juce::Slider::thumbColourId, juce::Colour(0xff16c5d0));
        setColour(juce::Slider::trackColourId, juce::Colour(0xff1d5264));
        setColour(juce::Slider::backgroundColourId, juce::Colour(0xff07131b));
        setColour(juce::Label::textColourId, juce::Colour(0xffd7e5e8));
        setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd7e5e8));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto colour = backgroundColour;
        if (shouldDrawButtonAsDown)
            colour = colour.brighter(0.18f);
        else if (shouldDrawButtonAsHighlighted)
            colour = colour.brighter(0.10f);

        g.setColour(colour);
        g.fillRoundedRectangle(button.getLocalBounds().toFloat().reduced(0.5f), 6.0f);
        g.setColour(juce::Colour(0xff2e6072));
        g.drawRoundedRectangle(button.getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
    }
};
}

class StudioApplication::MainWindow final : public juce::DocumentWindow,
                                             private juce::Timer
{
public:
    MainWindow(Session& sessionToUse, TransportState& transportToUse,
               MixerState& mixerToUse, InstrumentHost& hostToUse,
               AudioEngine& audioToUse)
        : DocumentWindow("Classic Player Studio", juce::Colour(0xff08151e), DocumentWindow::allButtons),
          sessionState(sessionToUse), transportState(transportToUse), timelineView(sessionToUse, transportToUse),
          mixerState(mixerToUse), instrumentHost(hostToUse), audioEngine(audioToUse)
    {
        setLookAndFeel(&lookAndFeel);
        auto* content = new StudioCanvas();
        content->setSize(1180, 760);

        title.setText("CLASSIC PLAYER STUDIO", juce::dontSendNotification);
        title.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colour(0xfff4fbfc));
        title.setBounds(30, 24, 400, 36);
        content->addAndMakeVisible(title);

        status.setText(cpText("Sessão vazia · fundação da DAW"), juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colour(0xff8fb5bf));
        status.setBounds(30, 72, 620, 28);
        content->addAndMakeVisible(status);

        mixerSummary.setColour(juce::Label::textColourId, juce::Colour(0xff8fb5bf));
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
                pluginSummary.setText(cpText("Instrumentos: faça uma varredura antes de carregar"),
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

        pluginSummary.setText(cpText("Instrumentos: varredura não executada"), juce::dontSendNotification);
        pluginSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        pluginSummary.setBounds(435, 172, 700, 28);
        content->addAndMakeVisible(pluginSummary);

        trackLabel.setText("RECORD TO", juce::dontSendNotification);
        trackLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        trackLabel.setBounds(30, 218, 90, 28);
        content->addAndMakeVisible(trackLabel);

        trackSelector.setTextWhenNoChoicesAvailable("No tracks");
        trackSelector.onChange = [this]
        {
            if (trackSelector.getSelectedId() > 0)
                selectedTrackIndex = trackSelector.getSelectedId() - 1;
            refreshTimelineSummary();
        };
        trackSelector.setBounds(120, 214, 260, 36);
        content->addAndMakeVisible(trackSelector);

        addAudioTrack.setButtonText("ADD AUDIO TRACK");
        addAudioTrack.onClick = [this]
        {
            SessionTrack track;
            track.name = "Audio " + juce::String(sessionState.tracks.size());
            track.instrument = false;
            sessionState.tracks.add(std::move(track));
            selectedTrackIndex = sessionState.tracks.size() - 1;
            mixerState.syncFromSession(sessionState.tracks);
            refreshTrackSelector();
            reloadSessionAudio();
            refreshTimelineSummary();
        };
        addAudioTrack.setBounds(390, 214, 155, 36);
        content->addAndMakeVisible(addAudioTrack);

        removeTrack.setButtonText("REMOVE AUDIO TRACK");
        removeTrack.onClick = [this]
        {
            if (selectedTrackIndex <= 0 || selectedTrackIndex >= sessionState.tracks.size())
            {
                pluginSummary.setText(cpText("A primeira pista é reservada ao instrumento"),
                                      juce::dontSendNotification);
                return;
            }

            sessionState.tracks.remove(selectedTrackIndex);
            selectedTrackIndex = juce::jmin(selectedTrackIndex, sessionState.tracks.size() - 1);
            mixerState.syncFromSession(sessionState.tracks);
            refreshTrackSelector();
            syncMixerControlsFromState();
            reloadSessionAudio();
            refreshTimelineSummary();
        };
        removeTrack.setBounds(555, 214, 175, 36);
        content->addAndMakeVisible(removeTrack);

        newSession.setButtonText("NEW SESSION");
        newSession.onClick = [this]
        {
            finishActiveRecording();
            audioEngine.stop();
            sessionState.clear();
            transportState.stop();
            transportState.setSampleRate(sessionState.sampleRate);
            mixerState.syncFromSession(sessionState.tracks);
            syncMixerControlsFromState();
            instrumentHost.unload();
            selectedTrackIndex = 0;
            refreshTrackSelector();
            reloadSessionAudio();
            refreshTimelineSummary();
            pluginSummary.setText(cpText("Sessão nova"), juce::dontSendNotification);
        };
        newSession.setBounds(30, 268, 120, 36);
        content->addAndMakeVisible(newSession);

        openSession.setButtonText("OPEN SESSION");
        openSession.onClick = [this]
        {
            finishActiveRecording();
            audioEngine.stop();
            auto chooser = std::make_shared<juce::FileChooser>(
                cpText("Abrir sessão do Classic Player Studio"), juce::File(), "*.cpsession");
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
                    owner.selectedTrackIndex = 0;
                    owner.refreshTrackSelector();
                    owner.reloadSessionAudio();
                    owner.refreshTimelineSummary();
                    const auto restored = owner.restoreInstrumentFromSession();
                    owner.pluginSummary.setText(
                        restored ? cpText("Sessão aberta e instrumento restaurado: ") + result.getFileName()
                                 : cpText("Sessão aberta: ") + result.getFileName(),
                        juce::dontSendNotification);
                }
                else
                    owner.pluginSummary.setText(cpText("Não foi possível abrir a sessão"),
                                                juce::dontSendNotification);
            });
        };
        openSession.setBounds(160, 268, 130, 36);
        content->addAndMakeVisible(openSession);

        saveSession.setButtonText("SAVE SESSION");
        saveSession.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser>(
                cpText("Salvar sessão do Classic Player Studio"), juce::File(), "*.cpsession");
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
                    owner.pluginSummary.setText(cpText("Sessão salva: ") + result.getFileName(),
                                                juce::dontSendNotification);
                else
                    owner.pluginSummary.setText(cpText("Não foi possível salvar a sessão"),
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
            {
                reloadSessionAudio();
                audioSummary.setText(cpText("Áudio: ativo · ") + juce::String(audioEngine.sampleRate(), 0)
                                         + " Hz / " + juce::String(audioEngine.bufferSize()) + " samples",
                                     juce::dontSendNotification);
            }
            else
                audioSummary.setText(cpText("Áudio: falha · ") + error, juce::dontSendNotification);
        };
        startAudio.setBounds(440, 268, 130, 36);
        content->addAndMakeVisible(startAudio);

        stopAudio.setButtonText("STOP AUDIO");
        stopAudio.onClick = [this]
        {
            finishActiveRecording();
            audioEngine.stop();
            audioSummary.setText(cpText("Áudio: parado"), juce::dontSendNotification);
            recordingSummary.setText(cpText("Gravação: parada"), juce::dontSendNotification);
        };
        stopAudio.setBounds(580, 268, 130, 36);
        content->addAndMakeVisible(stopAudio);

        audioSummary.setText(cpText("Áudio: parado"), juce::dontSendNotification);
        audioSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        audioSummary.setBounds(440, 308, 650, 24);
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
                cpText("Gravar entrada de áudio"), juce::File(), "*.wav");
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
                        owner.recordingSummary.setText(cpText("Gravação: falha ao iniciar áudio · ") + startError,
                                                       juce::dontSendNotification);
                        return;
                    }
                }

                juce::String error;
                if (owner.audioEngine.startRecording(result, error))
                {
                    owner.activeRecording = true;
                    owner.recordStartPosition = owner.transportState.position();
                    owner.recordingSummary.setText("Gravando: "
                                                       + owner.audioEngine.recordingFile().getFileName(),
                                                   juce::dontSendNotification);
                }
                else
                    owner.recordingSummary.setText(cpText("Gravação: falha · ") + error,
                                                   juce::dontSendNotification);
            });
        };
        recordAudio.setBounds(30, 360, 130, 36);
        content->addAndMakeVisible(recordAudio);

        stopRecording.setButtonText("STOP RECORDING");
        stopRecording.onClick = [this]
        {
            finishActiveRecording();
        };
        stopRecording.setBounds(170, 360, 145, 36);
        content->addAndMakeVisible(stopRecording);

        recordingSummary.setText(cpText("Gravação: pronta"), juce::dontSendNotification);
        recordingSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        recordingSummary.setBounds(330, 364, 800, 28);
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
        meterSummary.setBounds(30, 474, 1120, 24);
        content->addAndMakeVisible(meterSummary);

        timelineSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        timelineSummary.setBounds(30, 510, 1120, 32);
        content->addAndMakeVisible(timelineSummary);
        timelineView.setBounds(30, 545, 1120, 150);
        content->addAndMakeVisible(timelineView);

        for (auto* label : { &status, &mixerSummary, &pluginSummary, &audioSummary,
                             &recordingSummary, &meterSummary, &timelineSummary })
        {
            label->setFont(juce::FontOptions(14.0f));
        }

        startTimerHz(20);
        setContentOwned(content, true);
        refreshTrackSelector();
        refreshTimelineSummary();
        centreWithSize(1180, 760);
        // The first Studio build was freely resizable while most controls had
        // fixed coordinates, which produced a large empty grey area.  Keep a
        // deliberate, balanced workspace until the responsive layout pass is
        // implemented.
        setResizable(false, false);
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

    void finishActiveRecording()
    {
        if (! activeRecording && ! audioEngine.isRecording())
            return;

        const auto file = audioEngine.recordingFile();
        const auto samples = audioEngine.recordedSamples();
        audioEngine.stopRecording();
        const auto sampleRate = audioEngine.sampleRate();

        if (activeRecording && ! sessionState.tracks.isEmpty() && samples > 0 && sampleRate > 0.0)
        {
            SessionClip clip;
            clip.name = file.getFileNameWithoutExtension();
            clip.filePath = file.getFullPathName();
            clip.startSeconds = recordStartPosition;
            clip.lengthSeconds = static_cast<double>(samples) / sampleRate;
            clip.sampleRate = sampleRate;
            clip.numChannels = 2;
            const auto target = juce::jlimit(0, sessionState.tracks.size() - 1, selectedTrackIndex);
            sessionState.tracks.getReference(target).clips.add(std::move(clip));
            reloadSessionAudio();
            recordingSummary.setText(cpText("Gravação adicionada à ") + sessionState.tracks[target].name
                                         + ": " + file.getFileName(),
                                     juce::dontSendNotification);
            refreshTimelineSummary();
        }
        else
        {
            recordingSummary.setText(cpText("Gravação: parada"), juce::dontSendNotification);
        }

        activeRecording = false;
    }

    void reloadSessionAudio()
    {
        juce::String clipStatus;
        if (! audioEngine.reloadClipSources(clipStatus) && clipStatus.isNotEmpty())
            pluginSummary.setText(clipStatus, juce::dontSendNotification);
    }

    void refreshTimelineSummary()
    {
        int clipCount = 0;
        double totalSeconds = 0.0;
        juce::String lastClip;
        for (const auto& track : sessionState.tracks)
            for (const auto& clip : track.clips)
            {
                ++clipCount;
                totalSeconds = juce::jmax(totalSeconds, clip.startSeconds + clip.lengthSeconds);
                lastClip = clip.name;
            }

        timelineSummary.setText(cpText("TIMELINE · ") + juce::String(clipCount) + cpText(" clipe(s) · duração ")
                                   + juce::String(totalSeconds, 2) + cpText(" s · áudio carregado: ")
                                   + juce::String(audioEngine.loadedClipCount())
                                   + (lastClip.isNotEmpty() ? cpText(" · último: ") + lastClip : juce::String()),
                               juce::dontSendNotification);
        timelineView.refresh();
    }

    void refreshTrackSelector()
    {
        selectedTrackIndex = sessionState.tracks.isEmpty()
                                 ? 0
                                 : juce::jlimit(0, sessionState.tracks.size() - 1, selectedTrackIndex);
        trackSelector.clear(juce::dontSendNotification);
        for (int i = 0; i < sessionState.tracks.size(); ++i)
            trackSelector.addItem(sessionState.tracks[i].name, i + 1);
        if (! sessionState.tracks.isEmpty())
            trackSelector.setSelectedId(selectedTrackIndex + 1, juce::dontSendNotification);
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
                           + cpText(" · posição ") + juce::String(transportState.position(), 2)
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
public:
    ~MainWindow() override
    {
        setLookAndFeel(nullptr);
    }

    StudioLookAndFeel lookAndFeel;
    Session& sessionState;
    TransportState& transportState;
    TimelineView timelineView;
    MixerState& mixerState;
    InstrumentHost& instrumentHost;
    AudioEngine& audioEngine;
    juce::Label title, status, mixerSummary;
    juce::Label trackGainLabel, trackPanLabel, masterGainLabel, meterSummary;
    juce::Label pluginSummary;
    juce::Label audioSummary;
    juce::Label recordingSummary;
    juce::Label timelineSummary;
    juce::Label trackLabel;
    juce::TextButton scanPlugins, loadPlugin, newSession, openSession, saveSession;
    juce::TextButton startAudio, stopAudio;
    juce::TextButton play, pause, stop;
    juce::TextButton recordAudio, stopRecording;
    juce::TextButton addAudioTrack, removeTrack;
    juce::ComboBox trackSelector;
    juce::Slider trackGain, trackPan, masterGain;
    juce::ToggleButton muteTrack, soloTrack;
    juce::Array<juce::PluginDescription> availableInstruments;
    bool activeRecording { false };
    double recordStartPosition { 0.0 };
    int selectedTrackIndex { 0 };
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
