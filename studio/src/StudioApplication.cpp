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

class MixerView final : public juce::Component
{
public:
    MixerView(MixerState& mixerToUse, AudioEngine& audioToUse)
        : mixerState(mixerToUse), audioEngine(audioToUse)
    {
        setOpaque(true);
    }

    std::function<void(int)> onChannelChanged;

    void refresh() { repaint(); }
    void setSelectedTrack(int index) noexcept
    {
        selectedTrackIndex = mixerState.size() <= 0
            ? 0 : juce::jlimit(0, mixerState.size() - 1, index);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xff171b1e));
        g.setColour(juce::Colour(0xff343d42));
        g.drawHorizontalLine(0, 0.0f, area.getWidth());

        const auto trackCount = mixerState.size();
        const auto titleHeight = 23.0f;
        const auto rightMargin = 12.0f;
        const auto masterWidth = 76.0f;
        const auto trackWidth = trackCount > 0
            ? juce::jlimit(64.0f, 120.0f,
                           (area.getWidth() - masterWidth - rightMargin * 2.0f)
                               / static_cast<float>(trackCount))
            : 80.0f;
        g.setColour(juce::Colour(0xffbac4c8));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText("MIXER", 12, 3, 80, 16, juce::Justification::centredLeft);

        for (int index = 0; index < trackCount; ++index)
        {
            const auto& channel = mixerState.get(index);
            const auto x = rightMargin + trackWidth * static_cast<float>(index);
            auto strip = juce::Rectangle<float>(x + 2.0f, titleHeight, trackWidth - 4.0f,
                                                area.getHeight() - titleHeight - 5.0f);
            const auto selected = index == selectedTrackIndex;
            g.setColour(selected ? juce::Colour(0xff293a42) : juce::Colour(0xff22282c));
            g.fillRoundedRectangle(strip, 4.0f);
            g.setColour(selected ? juce::Colour(0xff168b99) : juce::Colour(0xff39454b));
            g.drawRoundedRectangle(strip, 4.0f, selected ? 1.5f : 1.0f);

            const auto nameBounds = strip.withY(titleHeight + 3.0f).withHeight(16.0f).reduced(3.0f, 0.0f);
            g.setColour(juce::Colour(0xffe0e7e9));
            g.setFont(juce::FontOptions(10.0f, selected ? juce::Font::bold : juce::Font::plain));
            g.drawText(channel.name, nameBounds.toNearestInt(), juce::Justification::centred, true);

            const auto buttonY = titleHeight + 22.0f;
            drawToggle(g, x + trackWidth * 0.5f - 24.0f, buttonY, "M", channel.muted,
                       juce::Colour(0xffaa4c57));
            drawToggle(g, x + trackWidth * 0.5f + 2.0f, buttonY, "S", channel.solo,
                       juce::Colour(0xffbb8a33));

            const auto panY = titleHeight + 56.0f;
            g.setColour(juce::Colour(0xff87979d));
            g.setFont(juce::FontOptions(9.0f));
            g.drawText("PAN " + juce::String(channel.pan, 2),
                       static_cast<int>(x + 2.0f), static_cast<int>(panY - 5.0f),
                       static_cast<int>(trackWidth - 4.0f), 13, juce::Justification::centred);
            g.setColour(juce::Colour(0xff46545b));
            g.fillRoundedRectangle(x + 8.0f, panY + 11.0f, trackWidth - 16.0f, 3.0f, 1.5f);
            const auto panPosition = (channel.pan + 1.0f) * 0.5f;
            g.setColour(juce::Colour(0xff18bdc8));
            g.fillEllipse(x + 7.0f + panPosition * (trackWidth - 18.0f), panY + 7.0f, 10.0f, 10.0f);

            const auto faderTop = titleHeight + 85.0f;
            const auto faderBottom = area.getHeight() - 28.0f;
            const auto faderX = x + trackWidth * 0.64f;
            const auto meterX = x + 8.0f;
            g.setColour(juce::Colour(0xff080c0e));
            g.fillRoundedRectangle(faderX - 2.0f, faderTop, 4.0f,
                                   juce::jmax(12.0f, faderBottom - faderTop), 2.0f);
            g.setColour(juce::Colour(0xff52636a));
            g.drawVerticalLine(static_cast<int>(faderX), faderTop, faderBottom);
            const auto faderFraction = juce::jlimit(0.0f, 1.0f, (12.0f - channel.gainDb) / 72.0f);
            const auto thumbY = faderTop + faderFraction * (faderBottom - faderTop - 10.0f);
            g.setColour(juce::Colour(0xffd1dadd));
            g.fillRoundedRectangle(faderX - trackWidth * 0.19f, thumbY,
                                   trackWidth * 0.38f, 10.0f, 2.0f);
            g.setColour(juce::Colour(0xff536169));
            g.drawRoundedRectangle(faderX - trackWidth * 0.19f, thumbY,
                                   trackWidth * 0.38f, 10.0f, 2.0f, 1.0f);

            const auto meterHeight = juce::jmax(0.0f, faderBottom - faderTop);
            const auto peak = juce::jlimit(0.0f, 1.0f, audioEngine.trackPostFaderPeak(index));
            const auto litHeight = meterHeight * peak;
            g.setColour(juce::Colour(0xff11181b));
            g.fillRoundedRectangle(meterX, faderTop, 6.0f, meterHeight, 2.0f);
            g.setColour(peak > 0.92f ? juce::Colour(0xffe39b37) : juce::Colour(0xff1ac0a0));
            g.fillRoundedRectangle(meterX, faderBottom - litHeight, 6.0f, litHeight, 2.0f);

            g.setColour(juce::Colour(0xffdce5e7));
            g.setFont(juce::FontOptions(9.0f));
            g.drawText(juce::String(channel.gainDb, 1) + " dB",
                       static_cast<int>(x + 3.0f), static_cast<int>(area.getHeight() - 23.0f),
                       static_cast<int>(trackWidth - 6.0f), 15, juce::Justification::centred);
        }

        const auto masterX = area.getWidth() - masterWidth - rightMargin;
        auto masterStrip = juce::Rectangle<float>(masterX, titleHeight, masterWidth,
                                                  area.getHeight() - titleHeight - 5.0f);
        g.setColour(juce::Colour(0xff263035));
        g.fillRoundedRectangle(masterStrip, 4.0f);
        g.setColour(juce::Colour(0xff62727a));
        g.drawRoundedRectangle(masterStrip, 4.0f, 1.0f);
        g.setColour(juce::Colour(0xfff1f5f6));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText("MASTER", masterStrip.withY(titleHeight + 3.0f).withHeight(16.0f).toNearestInt(),
                   juce::Justification::centred);
        g.setColour(juce::Colour(0xff11181b));
        const auto masterFaderTop = titleHeight + 35.0f;
        const auto masterFaderBottom = area.getHeight() - 28.0f;
        const auto masterFaderX = masterStrip.getCentreX() + 7.0f;
        g.fillRoundedRectangle(masterFaderX - 2.0f, masterFaderTop, 4.0f,
                               juce::jmax(12.0f, masterFaderBottom - masterFaderTop), 2.0f);
        const auto masterFraction = juce::jlimit(0.0f, 1.0f,
                (12.0f - mixerState.masterGainDb()) / 72.0f);
        const auto masterThumbY = masterFaderTop
            + masterFraction * (masterFaderBottom - masterFaderTop - 10.0f);
        g.setColour(juce::Colour(0xffd1dadd));
        g.fillRoundedRectangle(masterFaderX - 18.0f, masterThumbY, 36.0f, 10.0f, 2.0f);
        const auto masterPeak = juce::jlimit(0.0f, 1.0f, audioEngine.masterPeak());
        const auto masterMeterHeight = masterFaderBottom - masterFaderTop;
        g.setColour(juce::Colour(0xff101619));
        g.fillRoundedRectangle(masterX + 10.0f, masterFaderTop, 6.0f, masterMeterHeight, 2.0f);
        g.setColour(masterPeak > 0.92f ? juce::Colour(0xffe39b37) : juce::Colour(0xff1ac0a0));
        g.fillRoundedRectangle(masterX + 10.0f, masterFaderBottom - masterMeterHeight * masterPeak,
                               6.0f, masterMeterHeight * masterPeak, 2.0f);
        g.setColour(juce::Colour(0xffdce5e7));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(juce::String(mixerState.masterGainDb(), 1) + " dB",
                   masterStrip.withY(area.getHeight() - 23.0f).withHeight(15.0f).toNearestInt(),
                   juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragTarget = DragTarget::none;
        dragTrackIndex = -1;
        const auto trackCount = mixerState.size();
        const auto area = getLocalBounds().toFloat();
        const auto width = trackCount > 0
            ? juce::jlimit(64.0f, 120.0f, (area.getWidth() - 100.0f)
                                                  / static_cast<float>(trackCount))
            : 80.0f;
        const auto index = event.position.x >= 12.0f
            ? static_cast<int>((event.position.x - 12.0f) / width) : -1;
        if (index >= 0 && index < trackCount)
        {
            selectedTrackIndex = index;
            const auto localX = event.position.x - (12.0f + width * static_cast<float>(index));
            const auto buttonY = 45.0f;
            if (event.position.y >= buttonY && event.position.y <= buttonY + 22.0f)
            {
                if (localX >= width * 0.5f - 24.0f && localX < width * 0.5f - 2.0f)
                    mixerState.get(index).muted = ! mixerState.get(index).muted;
                else if (localX >= width * 0.5f + 2.0f && localX < width * 0.5f + 24.0f)
                    mixerState.get(index).solo = ! mixerState.get(index).solo;
                else
                    dragTarget = DragTarget::none;
                if (onChannelChanged)
                    onChannelChanged(index);
                repaint();
                return;
            }

            const auto panLeft = 8.0f;
            const auto panRight = width - 8.0f;
            const auto faderLeft = width * 0.45f;
            const auto faderRight = width * 0.84f;
            if (event.position.y >= 76.0f && event.position.y <= 101.0f
                && localX >= panLeft && localX <= panRight)
                dragTarget = DragTarget::pan;
            else if (event.position.y >= 105.0f && localX >= faderLeft && localX <= faderRight)
                dragTarget = DragTarget::trackGain;
            else
                dragTarget = DragTarget::none;
            dragTrackIndex = index;
            if (dragTarget != DragTarget::none)
                updateDraggedValue(event.position);
            if (onChannelChanged)
                onChannelChanged(index);
            repaint();
            return;
        }

        const auto masterFaderX = area.getWidth() - 43.0f;
        if (event.position.x >= masterFaderX - 18.0f && event.position.x <= masterFaderX + 18.0f
            && event.position.y >= 55.0f)
        {
            dragTarget = DragTarget::masterGain;
            dragTrackIndex = -1;
            updateDraggedValue(event.position);
            if (onChannelChanged)
                onChannelChanged(-1);
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragTarget == DragTarget::none)
            return;
        updateDraggedValue(event.position);
        if (onChannelChanged)
            onChannelChanged(dragTrackIndex);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override { dragTarget = DragTarget::none; }

private:
    enum class DragTarget { none, trackGain, pan, masterGain };

    static void drawToggle(juce::Graphics& g, float x, float y, const char* text,
                           bool active, juce::Colour activeColour)
    {
        const auto bounds = juce::Rectangle<float>(x, y, 20.0f, 18.0f);
        g.setColour(active ? activeColour : juce::Colour(0xff303a40));
        g.fillRoundedRectangle(bounds, 3.0f);
        g.setColour(active ? juce::Colours::white : juce::Colour(0xffa6b4b9));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText(text, bounds.toNearestInt(), juce::Justification::centred);
    }

    void updateDraggedValue(juce::Point<float> point)
    {
        const auto height = static_cast<float>(getHeight());
        if (dragTarget == DragTarget::pan && dragTrackIndex >= 0
            && juce::isPositiveAndBelow(dragTrackIndex, mixerState.size()))
        {
            const auto trackCount = mixerState.size();
            const auto width = juce::jlimit(64.0f, 120.0f,
                (static_cast<float>(getWidth()) - 100.0f) / static_cast<float>(trackCount));
            const auto stripX = 12.0f + width * static_cast<float>(dragTrackIndex);
            mixerState.get(dragTrackIndex).pan = juce::jlimit(-1.0f, 1.0f,
                ((point.x - (stripX + 8.0f)) / juce::jmax(1.0f, width - 16.0f)) * 2.0f - 1.0f);
            return;
        }

        if (dragTarget == DragTarget::trackGain && dragTrackIndex >= 0
            && juce::isPositiveAndBelow(dragTrackIndex, mixerState.size()))
        {
            const auto top = 108.0f;
            const auto bottom = height - 31.0f;
            const auto fraction = juce::jlimit(0.0f, 1.0f, (point.y - top) / juce::jmax(1.0f, bottom - top));
            mixerState.get(dragTrackIndex).gainDb = 12.0f - fraction * 72.0f;
            return;
        }

        if (dragTarget == DragTarget::masterGain)
        {
            const auto top = 59.0f;
            const auto bottom = height - 31.0f;
            const auto fraction = juce::jlimit(0.0f, 1.0f, (point.y - top) / juce::jmax(1.0f, bottom - top));
            mixerState.setMasterGainDb(12.0f - fraction * 72.0f);
        }
    }

    MixerState& mixerState;
    AudioEngine& audioEngine;
    int selectedTrackIndex { 0 };
    int dragTrackIndex { -1 };
    DragTarget dragTarget { DragTarget::none };
};

class StudioCanvas final : public juce::Component
{
public:
    std::function<void(juce::Rectangle<int>)> onLayout;

    void resized() override
    {
        if (onLayout)
            onLayout(getLocalBounds());
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        const auto width = bounds.getWidth();
        const auto height = bounds.getHeight();
        const auto topBarBottom = 104;
        const auto mixerTop = juce::jmax(430, height - 214);

        g.fillAll(juce::Colour(0xff101316));

        // This follows the conventional DAW hierarchy: a compact transport
        // strip, a large arrangement workspace and a permanently visible
        // mixer dock. The high-contrast dividers keep the three work areas
        // legible on small laptop displays as well as full-size monitors.
        g.setColour(juce::Colour(0xff20262a));
        g.fillRect(0, 0, width, topBarBottom);
        g.setColour(juce::Colour(0xff181d21));
        g.fillRect(0, topBarBottom, width, mixerTop - topBarBottom);
        g.setColour(juce::Colour(0xff20262a));
        g.fillRect(0, mixerTop, width, height - mixerTop);

        g.setColour(juce::Colour(0xff0a9bad));
        g.fillRect(0, topBarBottom - 2, width, 2);
        g.setColour(juce::Colour(0xff384248));
        g.drawHorizontalLine(mixerTop, 0.0f, static_cast<float>(width));

        g.setColour(juce::Colour(0xffaebbc1));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText("ARRANGEMENT", 18, topBarBottom + 1, 150, 14, juce::Justification::centredLeft);
        g.drawText("MIXER", 18, mixerTop + 4, 100, 14, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff78929b));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("CLASSIC PLAYER STUDIO", width - 220, 13, 200, 16,
                   juce::Justification::centredRight);
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
          mixerState(mixerToUse), instrumentHost(hostToUse), audioEngine(audioToUse),
          mixerView(mixerToUse, audioToUse)
    {
        setLookAndFeel(&lookAndFeel);
        auto* content = new StudioCanvas();
        content->setSize(1180, 850);
        importFormatManager.registerBasicFormats();

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

        scanPlugins.setButtonText("SCAN");
        scanPlugins.setTooltip("Scan installed VST3 and AU instruments");
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

        loadPlugin.setButtonText("LOAD INST");
        loadPlugin.setTooltip("Load the first available instrument");
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
            {
                selectedTrackIndex = trackSelector.getSelectedId() - 1;
                timelineView.setSelectedTrack(selectedTrackIndex);
                syncMixerControlsFromState();
            }
            refreshTimelineSummary();
        };
        trackSelector.setBounds(120, 214, 260, 36);
        content->addAndMakeVisible(trackSelector);

        addAudioTrack.setButtonText("+ TRACK");
        addAudioTrack.setTooltip("Add a new audio track");
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

        removeTrack.setButtonText("REMOVE");
        removeTrack.setTooltip("Remove the selected audio track");
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

        newSession.setButtonText("NEW");
        newSession.setTooltip("Create a new session");
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

        openSession.setButtonText("OPEN");
        openSession.setTooltip("Open a saved session");
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

        saveSession.setButtonText("SAVE");
        saveSession.setTooltip("Save the current session");
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

        startAudio.setButtonText("AUDIO ON");
        startAudio.setTooltip("Start the audio device");
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

        stopAudio.setButtonText("AUDIO OFF");
        stopAudio.setTooltip("Stop audio and finish the current recording");
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

        recordAudio.setButtonText("REC");
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

        stopRecording.setButtonText("STOP REC");
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

        timelineSummary.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        timelineSummary.setBounds(30, 540, 1120, 24);
        content->addAndMakeVisible(timelineSummary);

        importAudio.setButtonText("IMPORT");
        importAudio.setTooltip("Import WAV, AIFF or FLAC audio");
        importAudio.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser>(
                cpText("Importar áudio para a faixa selecionada"), juce::File(),
                "*.wav;*.aif;*.aiff;*.flac");
            juce::Component::SafePointer<MainWindow> safeThis(this);
            chooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectFiles,
                                 [safeThis, chooser](const juce::FileChooser& completedChooser)
            {
                if (safeThis == nullptr)
                    return;

                const auto result = completedChooser.getResult();
                if (result != juce::File())
                    safeThis->importAudioFile(result);
            });
        };
        importAudio.setBounds(30, 570, 150, 30);
        content->addAndMakeVisible(importAudio);

        deleteTimelineClip.setButtonText("DELETE CLIP");
        deleteTimelineClip.onClick = [this]
        {
            if (timelineView.deleteSelectedClip())
            {
                pluginSummary.setText(cpText("Clip removido da timeline"), juce::dontSendNotification);
            }
            else
                pluginSummary.setText(cpText("Selecione um clip para remover"), juce::dontSendNotification);
        };
        deleteTimelineClip.setBounds(190, 570, 145, 30);
        content->addAndMakeVisible(deleteTimelineClip);

        timelineZoomOut.setButtonText("- ZOOM");
        timelineZoomOut.onClick = [this] { timelineView.zoomOut(); };
        timelineZoomOut.setBounds(345, 570, 95, 30);
        content->addAndMakeVisible(timelineZoomOut);

        timelineZoomIn.setButtonText("+ ZOOM");
        timelineZoomIn.onClick = [this] { timelineView.zoomIn(); };
        timelineZoomIn.setBounds(450, 570, 95, 30);
        content->addAndMakeVisible(timelineZoomIn);

        timelineFit.setButtonText("SHOW ALL");
        timelineFit.onClick = [this] { timelineView.showAll(); };
        timelineFit.setBounds(555, 570, 110, 30);
        content->addAndMakeVisible(timelineFit);

        timelineView.onTrackSelected = [this](int trackIndex)
        {
            selectedTrackIndex = trackIndex;
            trackSelector.setSelectedId(trackIndex + 1, juce::dontSendNotification);
            syncMixerControlsFromState();
            refreshTimelineSummary();
        };
        timelineView.onTrackStateChanged = [this](int trackIndex)
        {
            selectedTrackIndex = trackIndex;
            trackSelector.setSelectedId(trackIndex + 1, juce::dontSendNotification);
            mixerState.syncFromSession(sessionState.tracks);
            syncMixerControlsFromState();
            audioEngine.refreshMixerSnapshot();
        };
        timelineView.onClipSelected = [this](const juce::String& clipName)
        {
            pluginSummary.setText(cpText("Clip selecionado: ") + clipName,
                                  juce::dontSendNotification);
        };
        timelineView.onSessionEdited = [this]
        {
            reloadSessionAudio();
            refreshTimelineSummary();
            pluginSummary.setText(cpText("Posição do clip atualizada"), juce::dontSendNotification);
        };
        timelineView.setBounds(30, 610, 1120, 185);
        content->addAndMakeVisible(timelineView);

        mixerView.onChannelChanged = [this](int trackIndex)
        {
            if (trackIndex >= 0 && trackIndex < sessionState.tracks.size())
            {
                selectedTrackIndex = trackIndex;
                trackSelector.setSelectedId(trackIndex + 1, juce::dontSendNotification);
                timelineView.setSelectedTrack(trackIndex);
                syncSessionFromMixer();
            }
            audioEngine.refreshMixerSnapshot();
            mixerView.refresh();
        };
        content->addAndMakeVisible(mixerView);

        for (auto* label : { &status, &mixerSummary, &pluginSummary, &audioSummary,
                             &recordingSummary, &timelineSummary })
        {
            label->setFont(juce::FontOptions(14.0f));
        }

        content->onLayout = [this](juce::Rectangle<int> bounds) { layoutWorkspace(bounds); };
        startTimerHz(20);
        setContentOwned(content, true);
        refreshTrackSelector();
        syncMixerControlsFromState();
        refreshTimelineSummary();
        centreWithSize(1280, 800);
        setResizeLimits(980, 640, 2560, 1600);
        setResizable(true, true);
        setUsingNativeTitleBar(true);
    }
private:
    int selectedMixerChannel() const noexcept
    {
        return mixerState.size() <= 0
            ? 0
            : juce::jlimit(0, mixerState.size() - 1, selectedTrackIndex);
    }

    void layoutWorkspace(juce::Rectangle<int> bounds)
    {
        const auto width = bounds.getWidth();
        const auto height = bounds.getHeight();
        const auto margin = 16;
        const auto mixerTop = juce::jmax(430, height - 214);

        // Compact, always-visible DAW toolbar.
        title.setBounds(margin, 10, 255, 28);
        status.setBounds(margin, 40, 290, 20);
        mixerSummary.setBounds(margin, 62, 300, 18);

        const auto compact = width < 1240;
        auto toolbarX = compact ? 280 : 320;
        const auto sessionButtonWidth = compact ? 58 : 76;
        const auto utilityButtonWidth = compact ? 68 : 110;
        const auto pluginButtonWidth = compact ? 82 : 128;
        newSession.setBounds(toolbarX, 16, sessionButtonWidth, 28);
        toolbarX += sessionButtonWidth + 5;
        openSession.setBounds(toolbarX, 16, sessionButtonWidth, 28);
        toolbarX += sessionButtonWidth + 5;
        saveSession.setBounds(toolbarX, 16, sessionButtonWidth, 28);
        toolbarX += sessionButtonWidth + 6;
        scanPlugins.setBounds(toolbarX, 16, utilityButtonWidth, 28);
        toolbarX += utilityButtonWidth + 5;
        loadPlugin.setBounds(toolbarX, 16, pluginButtonWidth, 28);

        const auto compactTransport = compact;
        const auto transportButton = compactTransport ? 44 : 64;
        const auto transportGap = compactTransport ? 4 : 6;
        const auto recordButton = compactTransport ? 50 : 64;
        const auto stopRecordButton = compactTransport ? 68 : 100;
        const auto transportWidth = transportButton * 3 + recordButton + stopRecordButton
                                    + transportGap * 4;
        auto transportX = juce::jmax(toolbarX + pluginButtonWidth + 12,
                                     width - margin - transportWidth);
        play.setBounds(transportX, 16, transportButton, 28);
        transportX += transportButton + transportGap;
        pause.setBounds(transportX, 16, transportButton, 28);
        transportX += transportButton + transportGap;
        stop.setBounds(transportX, 16, transportButton, 28);
        transportX += transportButton + transportGap;
        recordAudio.setBounds(transportX, 16, recordButton, 28);
        transportX += recordButton + transportGap;
        stopRecording.setBounds(transportX, 16, stopRecordButton, 28);
        recordAudio.setButtonText(compact ? "REC" : "RECORD");
        stopRecording.setButtonText(compact ? "STOP REC" : "STOP RECORDING");

        startAudio.setBounds(width - margin - 176, 56, 84, 26);
        stopAudio.setBounds(width - margin - 86, 56, 70, 26);
        audioSummary.setBounds(toolbarX, 58, juce::jmax(100, width - toolbarX - 205), 22);
        pluginSummary.setBounds(toolbarX, 80, juce::jmax(140, width - toolbarX - 280), 18);
        recordingSummary.setBounds(width - 270, 80, 254, 18);

        // Arrangement toolbar: track focus, editing and navigation tools.
        trackLabel.setBounds(margin, 122, 50, 22);
        trackSelector.setBounds(margin + 52, 118, 176, 28);
        addAudioTrack.setBounds(margin + 234, 118, 84, 28);
        removeTrack.setBounds(margin + 324, 118, 92, 28);
        importAudio.setBounds(margin + 428, 118, 100, 28);
        deleteTimelineClip.setBounds(margin + 534, 118, 94, 28);
        timelineZoomOut.setBounds(margin + 640, 118, 62, 28);
        timelineZoomIn.setBounds(margin + 708, 118, 62, 28);
        timelineFit.setBounds(margin + 776, 118, 78, 28);
        timelineSummary.setBounds(margin + 870, 122,
                                  juce::jmax(72, width - (margin + 886)), 20);

        timelineView.setBounds(margin, 154, width - margin * 2,
                               juce::jmax(250, mixerTop - 168));

        // Docked mixer. The selected channel has direct controls while every
        // track remains selectable/muteable/soloable from the arrangement.
        mixerView.setBounds(margin, mixerTop + 23, width - margin * 2,
                            juce::jmax(150, height - mixerTop - 23));
    }

    void syncSessionFromMixer()
    {
        if (sessionState.tracks.isEmpty() || mixerState.size() <= 0)
            return;

        const auto channelIndex = selectedMixerChannel();
        const auto& channel = mixerState.get(channelIndex);
        auto& track = sessionState.tracks.getReference(channelIndex);
        track.volume = channel.linearGain();
        track.pan = channel.pan;
        track.muted = channel.muted;
        track.solo = channel.solo;
    }

    void syncMixerControlsFromState()
    {
        mixerView.setSelectedTrack(selectedTrackIndex);
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
            timelineView.showAll();
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

    void importAudioFile(const juce::File& file)
    {
        if (sessionState.tracks.isEmpty())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader(importFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        {
            pluginSummary.setText(cpText("Não foi possível importar este arquivo de áudio"),
                                  juce::dontSendNotification);
            return;
        }

        SessionClip clip;
        clip.name = file.getFileNameWithoutExtension();
        clip.filePath = file.getFullPathName();
        clip.startSeconds = transportState.position();
        clip.lengthSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        clip.sampleRate = reader->sampleRate;
        clip.numChannels = juce::jlimit(1, 64, static_cast<int>(reader->numChannels));

        const auto target = juce::jlimit(0, sessionState.tracks.size() - 1, selectedTrackIndex);
        sessionState.tracks.getReference(target).clips.add(std::move(clip));
        reloadSessionAudio();
        timelineView.setSelectedTrack(target);
        timelineView.showAll();
        refreshTimelineSummary();
        pluginSummary.setText(cpText("Áudio importado para ") + sessionState.tracks[target].name,
                              juce::dontSendNotification);
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
        {
            trackSelector.setSelectedId(selectedTrackIndex + 1, juce::dontSendNotification);
            timelineView.setSelectedTrack(selectedTrackIndex);
            mixerView.setSelectedTrack(selectedTrackIndex);
        }
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
            const auto& channel = mixerState.get(selectedMixerChannel());
            status.setText(status.getText() + " · " + channel.name,
                           juce::dontSendNotification);
        }
        mixerView.refresh();
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
    MixerView mixerView;
    juce::Label title, status, mixerSummary;
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
    juce::TextButton importAudio, deleteTimelineClip, timelineZoomOut, timelineZoomIn, timelineFit;
    juce::ComboBox trackSelector;
    juce::AudioFormatManager importFormatManager;
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
