#include "PluginEditor.h"
#include "LicenseVerifier.h"
#include "ClassicPlayerAssets.h"
#include "ChordDetector.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include <algorithm>
#include <cmath>
#include <set>

namespace
{
constexpr auto background = 0xff091018;
constexpr auto panel = 0xff151f28;
constexpr auto panelLight = 0xff202c35;
constexpr auto line = 0xff33414c;
constexpr auto teal = 0xff13b8ad;
constexpr auto yellow = 0xffffd84a;
constexpr auto text = 0xffedf4f7;
constexpr auto mutedText = 0xff9eabb5;

void flatButton(juce::Button& button)
{
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(panelLight));
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(teal));
    button.setColour(juce::TextButton::textColourOffId, juce::Colour(text));
    button.setColour(juce::TextButton::textColourOnId, juce::Colour(background));
}

juce::String midiNoteName(int note)
{
    static const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
}

juce::Image embeddedImage(const char* resourceName)
{
    int size = 0;
    if (const auto* data = ClassicPlayerAssets::getNamedResource(resourceName, size))
        return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
    return {};
}

class ClassicLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ClassicLookAndFeel()
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff0b141d));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(line));
        setColour(juce::ComboBox::textColourId, juce::Colour(text));
        setColour(juce::ComboBox::arrowColourId, juce::Colour(teal));
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff111b24));
        setColour(juce::PopupMenu::textColourId, juce::Colour(text));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(teal));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(background));
        setColour(juce::Slider::textBoxTextColourId, juce::Colour(text));
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff0b1117));
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(line));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float position, float startAngle, float endAngle,
                          juce::Slider&) override
    {
        const auto diameter = static_cast<float>(juce::jmin(width, height)) - 8.0f;
        const auto radius = diameter * 0.5f;
        const auto centre = juce::Point<float>(static_cast<float>(x) + static_cast<float>(width) * 0.5f,
                                               static_cast<float>(y) + static_cast<float>(height) * 0.5f);
        const auto bounds = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
        const auto angle = startAngle + position * (endAngle - startAngle);

        g.setColour(juce::Colour(0xff080d11));
        g.fillEllipse(bounds.expanded(3.0f));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff69747c), bounds.getX(), bounds.getY(),
                                               juce::Colour(0xff171e23), bounds.getRight(), bounds.getBottom(), false));
        g.fillEllipse(bounds);
        g.setColour(juce::Colour(0xff88939a));
        g.drawEllipse(bounds, 1.0f);

        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, radius + 3.0f, radius + 3.0f, 0.0f,
                          startAngle, angle, true);
        g.setColour(juce::Colour(teal));
        g.strokePath(arc, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

        const auto pointer = juce::Point<float>(centre.x + std::sin(angle) * radius * 0.66f,
                                                centre.y - std::cos(angle) * radius * 0.66f);
        g.setColour(juce::Colour(teal));
        g.drawLine(centre.x, centre.y, pointer.x, pointer.y, 2.2f);
        g.fillEllipse(pointer.x - 2.0f, pointer.y - 2.0f, 4.0f, 4.0f);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                             minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const auto centreX = static_cast<float>(x) + static_cast<float>(width) * 0.48f;
        g.setColour(juce::Colour(0xff080c0f));
        g.fillRoundedRectangle(centreX - 5.0f, static_cast<float>(y + 5), 10.0f,
                               static_cast<float>(height - 10), 2.0f);
        g.setColour(juce::Colour(0xff515b62));
        g.drawVerticalLine(static_cast<int>(centreX), static_cast<float>(y + 7),
                           static_cast<float>(y + height - 7));

        const auto thumb = juce::Rectangle<float>(centreX - 15.0f, sliderPos - 11.0f, 30.0f, 22.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffeef1f2), thumb.getX(), thumb.getY(),
                                               juce::Colour(0xff929a9f), thumb.getRight(), thumb.getY(), false));
        g.fillRoundedRectangle(thumb, 1.5f);
        g.setColour(juce::Colour(0xff626b70));
        g.drawRoundedRectangle(thumb, 1.5f, 1.0f);
        g.setColour(juce::Colour(0xff555d62));
        for (int offset = -6; offset <= 6; offset += 4)
            g.drawHorizontalLine(static_cast<int>(sliderPos) + offset, thumb.getX() + 3.0f,
                                 thumb.getRight() - 3.0f);
    }
};

ClassicLookAndFeel classicLookAndFeel;

class ColourPicker final : public juce::Component, private juce::ChangeListener
{
public:
    ColourPicker(juce::Colour initial, std::function<void(juce::Colour)> changed)
        : callback(std::move(changed))
    {
        selector.setCurrentColour(initial);
        selector.setColour(juce::ColourSelector::backgroundColourId, juce::Colour(panel));
        selector.addChangeListener(this);
        addAndMakeVisible(selector);
        setSize(300, 300);
    }

    ~ColourPicker() override { selector.removeChangeListener(this); }
    void resized() override { selector.setBounds(getLocalBounds()); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (callback) callback(selector.getCurrentColour());
    }

    juce::ColourSelector selector { juce::ColourSelector::showColourAtTop |
                                    juce::ColourSelector::showSliders |
                                    juce::ColourSelector::showColourspace };
    std::function<void(juce::Colour)> callback;
};
}

void ClassicPlayerAudioProcessorEditor::LevelMeter::setLevel(float newLevel)
{
    newLevel = juce::jlimit(0.0f, 1.0f, newLevel);
    if (std::abs(newLevel - level) > 0.006f)
    {
        level = newLevel;
        repaint();
    }
}

void ClassicPlayerAudioProcessorEditor::LevelMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff080c10));
    g.fillRect(bounds);
    g.setColour(juce::Colour(line));
    g.drawRect(bounds, 1.0f);
    auto fill = bounds.reduced(2.0f);
    fill.removeFromTop(fill.getHeight() * (1.0f - level));
    g.setColour(level > 0.86f ? juce::Colour(0xffe14d45)
                             : level > 0.66f ? juce::Colour(yellow) : juce::Colour(teal));
    g.fillRect(fill);
}

ClassicPlayerAudioProcessorEditor::NamedKeyboard::NamedKeyboard(juce::MidiKeyboardState& state)
    : MidiKeyboardComponent(state, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setAvailableRange(21, 108);
    setLowestVisibleKey(21);
    setScrollButtonsVisible(false);
    setColour(keyDownOverlayColourId, juce::Colour(0xff1976d2));
    setColour(mouseOverKeyOverlayColourId, juce::Colour(0x331976d2));
    setColour(textLabelColourId, juce::Colour(0xff15212a));
}

juce::String ClassicPlayerAudioProcessorEditor::NamedKeyboard::noteLabel(int note)
{
    static const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return names[note % 12];
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::drawWhiteNote(
    int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
    juce::Colour lineColour, juce::Colour)
{
    auto fill = juce::Colour(0xfff4f4ef);
    if (isDown) fill = findColour(keyDownOverlayColourId);
    else if (isOver) fill = fill.interpolatedWith(findColour(mouseOverKeyOverlayColourId), 0.55f);
    g.setColour(fill);
    g.fillRect(area);
    g.setColour(lineColour);
    g.drawRect(area, 1.0f);
    g.setColour(isDown ? juce::Colours::white : juce::Colour(0xff16222c));
    g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    g.drawText(noteLabel(note), area.removeFromBottom(20.0f), juce::Justification::centred);
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::drawBlackNote(
    int, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
    juce::Colour)
{
    auto fill = juce::Colour(0xff10151a);
    if (isDown) fill = findColour(keyDownOverlayColourId);
    else if (isOver) fill = fill.interpolatedWith(findColour(mouseOverKeyOverlayColourId), 0.7f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, 0.0f);
    g.setColour(juce::Colour(0xff56616a));
    g.drawRect(area, 1.0f);
}

void ClassicPlayerAudioProcessorEditor::NamedKeyboard::setActiveColour(juce::Colour colour)
{
    setColour(keyDownOverlayColourId, colour);
    setColour(mouseOverKeyOverlayColourId, colour.withAlpha(0.22f));
    repaint();
}

ClassicPlayerAudioProcessorEditor::LayerStrip::LayerStrip(
    ClassicPlayerAudioProcessor& p, int layerIndex, std::function<void()> mixChanged)
    : processor(p), index(layerIndex), mixStateChanged(std::move(mixChanged))
{
    layerTitle.setText("LAYER " + juce::String(index + 1), juce::dontSendNotification);
    layerTitle.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    layerTitle.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(layerTitle);

    for (auto* button : { &muteButton, &soloButton, &resetButton, &removeButton, &loadButton,
                          &externalInstrumentButton, &refreshExternalInstrumentButton,
                          &openExternalEditorButton, &deleteLibraryButton })
    {
        flatButton(*button);
        addAndMakeVisible(*button);
    }
    muteButton.setClickingTogglesState(true);
    soloButton.setClickingTogglesState(true);
    muteButton.onClick = [this] { muted = muteButton.getToggleState(); mixStateChanged(); };
    soloButton.onClick = [this] { solo = soloButton.getToggleState(); mixStateChanged(); };
    resetButton.onClick = [this] { resetLayer(); };
    removeButton.setTooltip("Excluir esta layer");
    removeButton.onClick = [this]
    {
        // The editor belongs to the hosted instrument and must close before
        // the layer releases or moves that instrument.
        externalEditorWindow.reset();
        if (removeLayerCallback) removeLayerCallback();
    };
    loadButton.onClick = [this] { chooseSoundFont(); };
    externalInstrumentButton.onClick = [this] { chooseExternalInstrument(); };
    refreshExternalInstrumentButton.onClick = [this]
    {
        processor.refreshExternalInstrumentLibrary();
        rebuildExternalInstrumentLibrary();
    };
    openExternalEditorButton.onClick = [this] { openExternalInstrumentEditor(); };
    externalInstrumentButton.setTooltip("Escolher manualmente um instrumento VST3/AU");
    refreshExternalInstrumentButton.setTooltip("Procurar instrumentos VST3/AU instalados");
    openExternalEditorButton.setTooltip("Abrir a janela de configuração do instrumento virtual");
    const auto canHost = processor.supportsExternalInstruments();
    externalInstrumentButton.setVisible(canHost);
    refreshExternalInstrumentButton.setVisible(canHost);
    openExternalEditorButton.setVisible(canHost);
    externalInstrumentBox.setVisible(canHost);
    externalInstrumentBox.setTextWhenNothingSelected("VST INSTALADO");
    externalInstrumentBox.onChange = [this]
    {
        const auto selected = externalInstrumentBox.getSelectedItemIndex();
        if (!juce::isPositiveAndBelow(selected, externalInstrumentFiles.size())) return;
        externalEditorWindow.reset();
        const auto result = processor.loadExternalInstrument(index, externalInstrumentFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar instrumento", result.getErrorMessage());
        refresh();
    };
    addAndMakeVisible(externalInstrumentBox);
    deleteLibraryButton.setTooltip("Excluir o SF2 selecionado da biblioteca");
    deleteLibraryButton.onClick = [this] { deleteSelectedSoundFont(); };
    deleteLibraryButton.setEnabled(false);

    fileLabel.setJustificationType(juce::Justification::centred);
    fileLabel.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(fileLabel);
    for (const auto& category : ClassicPlayerAudioProcessor::soundFontCategories())
        categoryBox.addItem(category, categoryBox.getNumItems() + 1);
    categoryBox.setSelectedId(1, juce::dontSendNotification);
    categoryBox.onChange = [this] { rebuildLibrary(); };
    libraryBox.onChange = [this]
    {
        const auto selected = libraryBox.getSelectedItemIndex();
        deleteLibraryButton.setEnabled(juce::isPositiveAndBelow(selected, libraryFiles.size()));
        if (!juce::isPositiveAndBelow(selected, libraryFiles.size())) return;
        const auto result = processor.loadSoundFont(index, libraryFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar SF2", result.getErrorMessage());
        refresh();
    };
    addAndMakeVisible(categoryBox);
    addAndMakeVisible(libraryBox);
    addAndMakeVisible(presetBox);

    gain.setSliderStyle(juce::Slider::LinearVertical);
    gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 18);
    gain.setColour(juce::Slider::trackColourId, juce::Colour(teal));
    gain.setColour(juce::Slider::thumbColourId, juce::Colour(0xffd8dde0));
    addAndMakeVisible(gain);
    gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "layer" + juce::String(index + 1) + "Gain", gain);

    cutoff.setRange(0.0, 100.0, 1.0);
    cutoff.setValue(100.0);
    cutoff.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    cutoff.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(cutoff);
    reverb.setRange(0.0, 100.0, 1.0);
    reverb.setValue(0.0);
    reverb.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    reverb.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(reverb);
    compressor.setRange(0.0, 100.0, 1.0);
    compressor.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    compressor.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
    addAndMakeVisible(compressor);
    const auto parameterPrefix = "layer" + juce::String(index + 1);
    cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Cutoff", cutoff);
    reverbAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Reverb", reverb);
    compressorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, parameterPrefix + "Comp", compressor);
    addAndMakeVisible(meter);

    for (auto* label : { &cutoffLabel, &reverbLabel, &compressorLabel, &routingLabel })
    {
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, juce::Colour(mutedText));
        label->setFont(juce::FontOptions(9.5f, juce::Font::bold));
        addAndMakeVisible(*label);
    }
    cutoffLabel.setText("CUTOFF", juce::dontSendNotification);
    reverbLabel.setText("REVERB", juce::dontSendNotification);
    compressorLabel.setText("COMP", juce::dontSendNotification);
    routingLabel.setText("ROTEAMENTO DA LAYER", juce::dontSendNotification);

    for (auto* button : { &volumeLearn, &cutoffLearn, &reverbLearn, &compressorLearn })
    {
        flatButton(*button);
        button->setTooltip("Mova um controle MIDI CC depois de ativar o learn");
        addAndMakeVisible(*button);
    }
    volumeLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::volume); };
    cutoffLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::cutoff); };
    reverbLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::reverb); };
    compressorLearn.onClick = [this] { processor.beginMidiLearn(index, ClassicPlayerAudioProcessor::LearnTarget::compressor); };

    initialiseComboBoxes();
    refresh();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::initialiseComboBoxes()
{
    mode.addItem("POLI", 1); mode.addItem("MONO", 2);
    sustain.addItem("SUSTAIN ON", 1); sustain.addItem("SUSTAIN OFF", 2);
    midiChannel.addItem("MIDI OMNI", 1);
    for (int channel = 1; channel <= 16; ++channel)
        midiChannel.addItem("MIDI CH " + juce::String(channel), channel + 1);
    midiDevice.onChange = [this]
    {
        const auto deviceIndex = midiDevice.getSelectedId() - 2;
        processor.setLayerMidiDevice(index,
            juce::isPositiveAndBelow(deviceIndex, midiDevices.size())
                ? midiDevices.getReference(deviceIndex).identifier : juce::String{});
    };
    addAndMakeVisible(midiDevice);
    for (int value = -4; value <= 4; ++value)
        octave.addItem((value > 0 ? "+" : "") + juce::String(value) + " OIT", value + 5);
    for (int note = 0; note < 128; ++note)
    {
        lowNote.addItem(midiNoteName(note), note + 1);
        highNote.addItem(midiNoteName(note), note + 1);
    }
    velocityCurve.addItem("VEL LINEAR", 1);
    velocityCurve.addItem("VEL SOFT", 2);
    velocityCurve.addItem("VEL HARD", 3);
    for (auto* box : { &mode, &sustain, &midiChannel, &octave, &lowNote, &highNote, &velocityCurve })
    {
        box->onChange = [this] { applyConfig(); };
        addAndMakeVisible(*box);
    }
    presetBox.onChange = [this]
    {
        const auto selected = presetBox.getSelectedItemIndex();
        if (juce::isPositiveAndBelow(selected, (int) presets.size()))
            processor.selectLayerPreset(index, presets[(size_t) selected].bank,
                                         presets[(size_t) selected].program);
    };
    refreshMidiDevices();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refreshMidiDevices()
{
    const auto devices = processor.availableMidiDevices();
    juce::String fingerprint;
    for (const auto& device : devices) fingerprint << device.identifier << ";";
    if (fingerprint == midiDeviceFingerprint && midiDevice.getNumItems() > 0) return;
    midiDeviceFingerprint = fingerprint;
    midiDevices = devices;
    const auto selected = processor.layerMidiDevice(index);
    midiDevice.clear(juce::dontSendNotification);
    midiDevice.addItem("TODOS OS CONTROLADORES", 1);
    int selectedId = 1;
    for (int item = 0; item < devices.size(); ++item)
    {
        const auto& device = devices.getReference(item);
        // The item value stores the stable CoreMIDI identifier; its visible
        // label remains the human-readable device name.
        midiDevice.addItem(device.name, item + 2);
        if (device.identifier == selected) selectedId = item + 2;
    }
    midiDevice.setSelectedId(selectedId, juce::dontSendNotification);
    midiDevice.setTextWhenNothingSelected("CONTROLADOR MIDI");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(panel));
    g.fillRoundedRectangle(bounds, 7.0f);
    g.setColour(juce::Colour(line));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 7.0f, 1.0f);
    g.setColour(juce::Colour(mutedText));
    g.setFont(9.0f);
    g.drawText("VOLUME", 12, 151, 86, 15, juce::Justification::centred);

    const auto scaleX = gain.getRight() - 22;
    const auto scaleTop = gain.getY() + 8;
    const auto scaleHeight = juce::jmax(80, gain.getHeight() - 42);
    static const std::array<std::pair<float, const char*>, 8> marks {{
        { 0.0f, "-inf" }, { 0.18f, "-40" }, { 0.36f, "-20" }, { 0.52f, "-10" },
        { 0.65f, "-5" }, { 0.77f, "0" }, { 0.88f, "+3" }, { 1.0f, "+6" }
    }};
    for (size_t markIndex = 0; markIndex < marks.size(); ++markIndex)
    {
        const auto [amount, label] = marks[markIndex];
        const auto y = scaleTop + static_cast<int>((1.0f - amount) * static_cast<float>(scaleHeight));
        g.setColour(juce::Colour(markIndex == 5 ? text : mutedText));
        g.drawHorizontalLine(y, static_cast<float>(scaleX - 5), static_cast<float>(scaleX + 2));
        g.drawText(label, scaleX + 4, y - 7, 27, 14, juce::Justification::left);
    }

    g.setColour(juce::Colour(line));
    g.drawHorizontalLine(routingLabel.getY() - 4, 108.0f, static_cast<float>(getWidth() - 12));
    g.drawHorizontalLine(lowNote.getY() - 9, 108.0f, static_cast<float>(getWidth() - 12));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::resized()
{
    auto area = getLocalBounds().reduced(9);
    auto top = area.removeFromTop(25);
    layerTitle.setBounds(top.removeFromLeft(72));
    muteButton.setBounds(top.removeFromLeft(30).reduced(1));
    soloButton.setBounds(top.removeFromLeft(30).reduced(1));
    resetButton.setBounds(top.removeFromRight(52).reduced(1));
    removeButton.setBounds(top.removeFromRight(24).reduced(1));
    area.removeFromTop(5);
    loadButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(4);
    auto externalLibraryRow = area.removeFromTop(28);
    refreshExternalInstrumentButton.setBounds(externalLibraryRow.removeFromRight(92).reduced(1, 0));
    externalInstrumentBox.setBounds(externalLibraryRow.reduced(1, 0));
    area.removeFromTop(4);
    auto externalRow = area.removeFromTop(28);
    externalInstrumentButton.setBounds(externalRow.removeFromLeft(externalRow.getWidth() / 2).reduced(1, 0));
    openExternalEditorButton.setBounds(externalRow.reduced(1, 0));
    area.removeFromTop(4);
    categoryBox.setBounds(area.removeFromTop(28));
    area.removeFromTop(4);
    auto libraryRow = area.removeFromTop(28);
    deleteLibraryButton.setBounds(libraryRow.removeFromRight(76).reduced(1, 0));
    libraryBox.setBounds(libraryRow.reduced(0, 0));
    area.removeFromTop(4);
    fileLabel.setBounds(area.removeFromTop(27));
    area.removeFromTop(4);
    presetBox.setBounds(area.removeFromTop(28));
    area.removeFromTop(10);

    auto controls = area;
    auto faderColumn = controls.removeFromLeft(96);
    meter.setBounds(faderColumn.removeFromLeft(16).reduced(1, 7));
    volumeLearn.setBounds(faderColumn.removeFromBottom(22).reduced(1, 0));
    gain.setBounds(faderColumn.reduced(1, 0));
    controls.removeFromLeft(4);

    auto knobs = controls.removeFromTop(126);
    const auto knobWidth = knobs.getWidth() / 3;
    auto placeKnob = [knobWidth](juce::Rectangle<int>& row, juce::Label& label,
                                 juce::Slider& slider, juce::TextButton* learn)
    {
        auto cell = row.removeFromLeft(knobWidth).reduced(2, 0);
        label.setBounds(cell.removeFromTop(16));
        if (learn != nullptr) learn->setBounds(cell.removeFromBottom(20).reduced(1));
        slider.setBounds(cell);
    };
    placeKnob(knobs, cutoffLabel, cutoff, &cutoffLearn);
    placeKnob(knobs, reverbLabel, reverb, &reverbLearn);
    placeKnob(knobs, compressorLabel, compressor, &compressorLearn);

    controls.removeFromTop(5);
    routingLabel.setBounds(controls.removeFromTop(22));
    controls.removeFromTop(6);
    auto row = controls.removeFromTop(31);
    mode.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    sustain.setBounds(row.reduced(2, 1));
    controls.removeFromTop(7);
    midiDevice.setBounds(controls.removeFromTop(31).reduced(2, 1));
    controls.removeFromTop(7);
    row = controls.removeFromTop(31);
    midiChannel.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    octave.setBounds(row.reduced(2, 1));
    controls.removeFromTop(18);
    row = controls.removeFromTop(31);
    lowNote.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 1));
    highNote.setBounds(row.reduced(2, 1));
    controls.removeFromTop(7);
    velocityCurve.setBounds(controls.removeFromTop(31).reduced(2, 1));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseSoundFont()
{
    fileChooser = std::make_unique<juce::FileChooser>("Escolha um SoundFont", juce::File{}, "*.sf2;*.SF2");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            externalEditorWindow.reset();
            loadButton.setEnabled(false);
            fileLabel.setText("Carregando...", juce::dontSendNotification);
            juce::File importedFile;
            auto result = processor.importSoundFont(file, categoryBox.getText(), importedFile);
            if (result.wasOk()) result = processor.loadSoundFont(index, importedFile);
            loadButton.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao carregar SF2", result.getErrorMessage());
            refresh();
        });
}

namespace
{
class HostedInstrumentEditorWindow final : public juce::DocumentWindow
{
public:
    HostedInstrumentEditorWindow(const juce::String& title, juce::AudioProcessorEditor* editor)
        : DocumentWindow(title, juce::Colour(0xff111820), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        centreWithSize(juce::jmax(420, getWidth()), juce::jmax(300, getHeight()));
        setVisible(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseExternalInstrument()
{
    if (!processor.supportsExternalInstruments())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Instrumento externo",
                                               "O carregamento de VST/AU está disponível apenas no aplicativo standalone.");
        return;
    }

   #if JUCE_MAC
    const auto filters = "*.vst3;*.component";
   #else
    const auto filters = "*.vst3";
   #endif
    fileChooser = std::make_unique<juce::FileChooser>("Escolha um instrumento virtual",
                                                      juce::File{}, filters);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            externalEditorWindow.reset();
            externalInstrumentButton.setEnabled(false);
            fileLabel.setText("Carregando instrumento...", juce::dontSendNotification);
            const auto result = processor.loadExternalInstrument(index, file);
            externalInstrumentButton.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao carregar instrumento", result.getErrorMessage());
            refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::openExternalInstrumentEditor()
{
    if (auto* editor = processor.createExternalInstrumentEditor(index))
    {
        externalEditorWindow = std::make_unique<HostedInstrumentEditorWindow>(
            processor.externalInstrumentName(index), editor);
        return;
    }

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           "Editor indisponível",
                                           "Este instrumento virtual não possui uma janela de edição.");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::deleteSelectedSoundFont()
{
    const auto selected = libraryBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, libraryFiles.size())) return;
    const auto file = libraryFiles.getReference(selected);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
                                       "Excluir SF2",
                                       "Excluir '" + file.getFileName() + "' da biblioteca?",
                                       "Excluir", "Cancelar", this,
                                       juce::ModalCallbackFunction::create(
                                           [safe, file](int answer)
                                           {
                                               if (safe == nullptr || answer == 0) return;
                                               const auto result = safe->processor.deleteLibrarySoundFont(file);
                                               if (result.failed())
                                                   juce::AlertWindow::showMessageBoxAsync(
                                                       juce::MessageBoxIconType::WarningIcon,
                                                       "Falha ao excluir SF2", result.getErrorMessage());
                                               safe->refresh();
                                           }));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::resetLayer()
{
    externalEditorWindow.reset();
    processor.unloadSoundFont(index);
    processor.unloadExternalInstrument(index);
    muted = solo = false;
    muteButton.setToggleState(false, juce::dontSendNotification);
    soloButton.setToggleState(false, juce::dontSendNotification);
    cutoff.setValue(100.0);
    reverb.setValue(0.0);
    compressor.setValue(0.0);
    gain.setValue(80.0);
    mode.setSelectedId(1);
    sustain.setSelectedId(1);
    midiChannel.setSelectedId(1);
    octave.setSelectedId(5);
    lowNote.setSelectedId(1);
    highNote.setSelectedId(128);
    velocityCurve.setSelectedId(1);
    refresh();
    mixStateChanged();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildPresets()
{
    presets = processor.layerPresets(index);
    presetBox.clear(juce::dontSendNotification);
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const auto& preset = presets[(size_t) i];
        presetBox.addItem(juce::String(preset.bank) + ":" + juce::String(preset.program)
                          + "  " + preset.name, i + 1);
    }
    if (!presets.empty()) presetBox.setSelectedItemIndex(0, juce::sendNotification);
    presetBox.setEnabled(!presets.empty());
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildExternalInstrumentLibrary()
{
    externalInstrumentFiles = processor.availableExternalInstruments();
    externalInstrumentBox.clear(juce::dontSendNotification);
    for (int item = 0; item < externalInstrumentFiles.size(); ++item)
        externalInstrumentBox.addItem(externalInstrumentFiles.getReference(item).getFileNameWithoutExtension(), item + 1);
    externalInstrumentBox.setTextWhenNothingSelected(
        externalInstrumentFiles.isEmpty() ? "NENHUM VST ENCONTRADO" : "VST INSTALADO");
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildLibrary()
{
    const auto currentPath = processor.soundFontPath(index);
    libraryFiles = processor.librarySoundFonts(categoryBox.getText());
    libraryBox.clear(juce::dontSendNotification);
    auto selectedId = 0;
    for (int item = 0; item < libraryFiles.size(); ++item)
    {
        const auto& file = libraryFiles.getReference(item);
        libraryBox.addItem(file.getFileNameWithoutExtension(), item + 1);
        if (file.getFullPathName() == currentPath) selectedId = item + 1;
    }
    libraryBox.setTextWhenNothingSelected(libraryFiles.isEmpty() ? "CATEGORIA VAZIA" : "ESCOLHA O SF2");
    if (selectedId > 0) libraryBox.setSelectedId(selectedId, juce::dontSendNotification);
    deleteLibraryButton.setEnabled(libraryBox.getSelectedItemIndex() >= 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refresh()
{
    const auto path = processor.soundFontPath(index);
    const auto externalName = processor.externalInstrumentName(index);
    if (path.isNotEmpty())
    {
        const auto parentCategory = juce::File(path).getParentDirectory().getFileName();
        const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
        const auto categoryIndex = categories.indexOf(parentCategory);
        if (categoryIndex >= 0) categoryBox.setSelectedId(categoryIndex + 1, juce::dontSendNotification);
    }
    rebuildLibrary();
    rebuildExternalInstrumentLibrary();
    const auto hasSource = path.isNotEmpty() || externalName.isNotEmpty();
    fileLabel.setText(path.isNotEmpty() ? juce::File(path).getFileName()
                                        : (externalName.isNotEmpty() ? externalName : "Sem SoundFont"),
                      juce::dontSendNotification);
    fileLabel.setColour(juce::Label::backgroundColourId,
                        hasSource ? juce::Colour(yellow) : juce::Colour(0xff0b1218));
    fileLabel.setColour(juce::Label::textColourId,
                        hasSource ? juce::Colours::black : juce::Colour(mutedText));
    openExternalEditorButton.setEnabled(processor.supportsExternalInstruments()
                                        && processor.hasExternalInstrument(index));
    const auto config = processor.layerConfig(index);
    mode.setSelectedId(config.mono ? 2 : 1, juce::dontSendNotification);
    sustain.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
    midiChannel.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
    octave.setSelectedId(config.octave + 5, juce::dontSendNotification);
    lowNote.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
    highNote.setSelectedId(config.highNote + 1, juce::dontSendNotification);
    velocityCurve.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
    rebuildPresets();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::applyConfig()
{
    auto config = processor.layerConfig(index);
    config.mono = mode.getSelectedId() == 2;
    config.sustainEnabled = sustain.getSelectedId() != 2;
    config.midiChannel = juce::jlimit(0, 16, midiChannel.getSelectedId() - 1);
    config.octave = octave.getSelectedId() - 5;
    config.lowNote = juce::jlimit(0, 127, lowNote.getSelectedId() - 1);
    config.highNote = juce::jlimit(0, 127, highNote.getSelectedId() - 1);
    config.velocityCurve = juce::jlimit(0, 2, velocityCurve.getSelectedId() - 1);
    if (config.lowNote > config.highNote)
    {
        config.highNote = config.lowNote;
        highNote.setSelectedId(config.highNote + 1, juce::dontSendNotification);
    }
    processor.setLayerConfig(index, config);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::setEngineEnabled(bool enabled)
{
    auto config = processor.layerConfig(index);
    config.enabled = enabled;
    processor.setLayerConfig(index, config);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateMeter()
{
    meter.setLevel(std::sqrt(juce::jlimit(0.0f, 1.0f, processor.layerPeak(index))));
    updateMidiLearnState();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateMidiLearnState()
{
    using Target = ClassicPlayerAudioProcessor::LearnTarget;
    const std::array<std::pair<Target, juce::TextButton*>, 4> controls {{
        { Target::volume, &volumeLearn }, { Target::cutoff, &cutoffLearn },
        { Target::reverb, &reverbLearn }, { Target::compressor, &compressorLearn }
    }};
    for (const auto& [target, button] : controls)
    {
        const auto learning = processor.isMidiLearning(index, target);
        const auto cc = processor.midiLearnCC(index, target);
        button->setButtonText(learning ? "MOVA O CC" : cc >= 0 ? "CC " + juce::String(cc) : "LEARN");
        button->setColour(juce::TextButton::buttonColourId,
                          learning ? juce::Colour(yellow)
                                   : cc >= 0 ? juce::Colour(0xff1b554e) : juce::Colour(panelLight));
        button->setColour(juce::TextButton::textColourOffId,
                          learning ? juce::Colour(background) : juce::Colour(text));
    }
}

ClassicPlayerAudioProcessorEditor::ClassicPlayerAudioProcessorEditor(ClassicPlayerAudioProcessor& p)
    : AudioProcessorEditor(&p), classicProcessor(p), keyboard(p.keyboardState)
{
    juce::Logger::writeToLog("Editor Classic Player inicializado");
    setLookAndFeel(&classicLookAndFeel);
    setOpaque(true);
#if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        classicProcessor.attachStandaloneMidiRouting(holder->deviceManager, holder->player);
#endif
    // Scan the standard VST3/AU locations once when the standalone editor opens.
    if (classicProcessor.supportsExternalInstruments())
        classicProcessor.refreshExternalInstrumentLibrary();

    appIcon.setImage(embeddedImage("classicplayerappicon_png"), juce::RectanglePlacement::centred);
    appIcon.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(appIcon);


    title.setText("CLASSIC PLAYER", juce::dontSendNotification);
    title.setFont(juce::FontOptions(25.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colour(teal));
    addAndMakeVisible(title);
    subtitle.setText("CLASSIC KEYS SF2 WORKSTATION", juce::dontSendNotification);
    subtitle.setColour(juce::Label::textColourId, juce::Colour(mutedText));
    addAndMakeVisible(subtitle);
    chordLabel.setText("-", juce::dontSendNotification);
    chordLabel.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    chordLabel.setJustificationType(juce::Justification::centred);
    chordLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black);
    chordLabel.setColour(juce::Label::textColourId, chordColour);
    addAndMakeVisible(chordLabel);
    // O visor já identifica visualmente a cifra; não exibir um rótulo adicional
    // acima do nome do acorde.
    chordCaption.setVisible(false);
    flatButton(chordColourButton);
    chordColourButton.onClick = [this]
    {
        const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
        auto picker = std::make_unique<ColourPicker>(chordColour, [safe](juce::Colour colour)
        {
            if (safe == nullptr) return;
            safe->chordColour = colour;
            safe->chordLabel.setColour(juce::Label::textColourId, colour);
            safe->chordLabel.repaint();
        });
        juce::CallOutBox::launchAsynchronously(std::move(picker), chordColourButton.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(chordColourButton);

    flatButton(keyColourButton);
    keyColourButton.onClick = [this]
    {
        const auto current = keyboard.findColour(juce::MidiKeyboardComponent::keyDownOverlayColourId);
        const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
        auto picker = std::make_unique<ColourPicker>(current, [safe](juce::Colour colour)
        {
            if (safe != nullptr) safe->keyboard.setActiveColour(colour);
        });
        juce::CallOutBox::launchAsynchronously(std::move(picker), keyColourButton.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(keyColourButton);

    flatButton(addLayerButton);
    addLayerButton.onClick = [this] { addLayer(); };
    addAndMakeVisible(addLayerButton);

    master.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    master.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
    master.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(teal));
    addAndMakeVisible(master);
    masterLabel.setText("MASTER", juce::dontSendNotification);
    masterLabel.setJustificationType(juce::Justification::centred);
    masterLabel.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(masterLabel);
    addAndMakeVisible(masterMeter);
    masterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        classicProcessor.parameters, "master", master);

    layerViewport.setViewedComponent(&layerContent, false);
    // Keep all layer controls accessible on compact notebook displays.
    layerViewport.setScrollBarsShown(true, true);
    layerViewport.setScrollBarThickness(9);
    layerViewport.setWantsKeyboardFocus(false);
    addAndMakeVisible(layerViewport);

    const auto visibleLayerCount = classicProcessor.activeLayerCount();
    displayedLayerCount = visibleLayerCount;
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        strips[(size_t) i] = std::make_unique<LayerStrip>(classicProcessor, i,
                                                          [this] { applyMixerStates(); });
        strips[(size_t) i]->setRemoveCallback([this, i] { removeLayer(i); });
        layerContent.addAndMakeVisible(*strips[(size_t) i]);
        strips[(size_t) i]->setVisible(i < visibleLayerCount);
    }
    addLayerButton.setEnabled(visibleLayerCount < Sf2Engine::layerCount);
    addAndMakeVisible(keyboard);
    classicProcessor.keyboardState.addListener(this);

    addAndMakeVisible(activationPanel);
    activationTitle.setText("ATIVAÇÃO DO CLASSIC PLAYER", juce::dontSendNotification);
    activationTitle.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    activationTitle.setJustificationType(juce::Justification::centred);
    activationPanel.addAndMakeVisible(activationTitle);
    activationHelp.setText("Digite o código de ativação fornecido com sua licença.", juce::dontSendNotification);
    activationHelp.setJustificationType(juce::Justification::centred);
    activationPanel.addAndMakeVisible(activationHelp);
    activationCode.setMultiLine(false);
    activationCode.setTextToShowWhenEmpty("CK26-....código assinado", juce::Colours::grey);
    activationPanel.addAndMakeVisible(activationCode);
    activationButton.onClick = [this] { activate(); };
    flatButton(activationButton);
    activationPanel.addAndMakeVisible(activationButton);
    activationStatus.setJustificationType(juce::Justification::centred);
    activationStatus.setColour(juce::Label::textColourId, juce::Colours::salmon);
    activationPanel.addAndMakeVisible(activationStatus);
    activationPanel.setVisible(!classicProcessor.isActivated());

    // setSize() invokes resized() immediately. All layer strips must exist
    // before that callback can lay them out.
    setResizable(true, true);
    // Keep the editor usable on 1366x768 notebooks while preserving the
    // existing Full HD layout when a larger display is available.
    setResizeLimits(960, 540, 1920, 1080);
    const auto display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto available = display != nullptr ? display->userArea
                                               : juce::Rectangle<int>(0, 0, 1366, 768);
    setSize(juce::jmin(1600, juce::jmax(960, available.getWidth() - 40)),
            juce::jmin(900, juce::jmax(540, available.getHeight() - 80)));
    startTimerHz(20);
}

ClassicPlayerAudioProcessorEditor::~ClassicPlayerAudioProcessorEditor()
{
    stopTimer();
    cancelPendingUpdate();
    classicProcessor.keyboardState.removeListener(this);
    setLookAndFeel(nullptr);
}

void ClassicPlayerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(background));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff122633), 0.0f, 0.0f,
                                           juce::Colour(background), (float) getWidth(), 220.0f, false));
    g.fillRect(0, 0, getWidth(), 118);
    g.setColour(juce::Colour(line));
    g.drawHorizontalLine(117, 18.0f, (float) getWidth() - 18.0f);
    g.drawHorizontalLine(getHeight() - 66, 18.0f, (float) getWidth() - 18.0f);
    g.setColour(juce::Colour(mutedText));
    g.setFont(10.5f);
    g.drawText("Copyright 2026 Willam Silva & Classic Keys. Todos os direitos reservados.",
               305, getHeight() - 49, getWidth() - 610, 28, juce::Justification::centred);
}

void ClassicPlayerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    auto header = area.removeFromTop(96);
    appIcon.setBounds(header.removeFromLeft(78).reduced(4));
    header.removeFromLeft(8);
    auto brand = header.removeFromLeft(330);
    brand.removeFromTop(16);
    title.setBounds(brand.removeFromTop(38));
    subtitle.setBounds(brand.removeFromTop(25));

    auto masterArea = header.removeFromRight(116);
    masterMeter.setBounds(masterArea.removeFromRight(13).reduced(0, 6));
    masterLabel.setBounds(masterArea.removeFromTop(17));
    master.setBounds(masterArea.reduced(3, 0));
    header.removeFromRight(12);

    auto addLayerArea = header.removeFromRight(100);
    addLayerButton.setBounds(addLayerArea.withSizeKeepingCentre(88, 32));
    header.removeFromRight(4);

    auto chordArea = header.reduced(4, 1);
    auto colourControls = chordArea.removeFromRight(106).reduced(5, 2);
    chordColourButton.setBounds(colourControls.removeFromTop(30));
    colourControls.removeFromTop(5);
    keyColourButton.setBounds(colourControls.removeFromTop(30));
    auto chordBox = chordArea.reduced(2, 0);
        chordCaption.setBounds({});
        chordLabel.setBounds(chordBox);

    area.removeFromTop(12);
    auto footer = area.removeFromBottom(54);

    auto keyboardArea = area.removeFromBottom(112);
    keyboard.setBounds(keyboardArea.reduced(0, 4));
    area.removeFromBottom(8);
    layerViewport.setBounds(area);
    layoutLayerStrips();

    activationPanel.setBounds(getLocalBounds());
    auto activation = activationPanel.getLocalBounds().withSizeKeepingCentre(
        juce::jmin(650, getWidth() - 60), 300);
    activationTitle.setBounds(activation.removeFromTop(58));
    activationHelp.setBounds(activation.removeFromTop(38));
    activation.removeFromTop(12);
    activationCode.setBounds(activation.removeFromTop(44));
    activation.removeFromTop(14);
    activationButton.setBounds(activation.removeFromTop(44).withSizeKeepingCentre(180, 44));
    activationStatus.setBounds(activation.removeFromTop(38));

    keyboard.setKeyWidth(juce::jmax(11.0f, static_cast<float>(keyboard.getWidth()) / 52.0f));
}

void ClassicPlayerAudioProcessorEditor::timerCallback()
{
    classicProcessor.consumeMidiControlUpdates();
    float masterLevel = 0.0f;
    for (int i = 0; i < classicProcessor.activeLayerCount(); ++i)
    {
        if (strips[(size_t) i] != nullptr)
            strips[(size_t) i]->updateMeter();
        masterLevel = juce::jmax(masterLevel, classicProcessor.layerPeak(i));
    }
    if (++timerTicks >= 20)
    {
        timerTicks = 0;
        const auto activeCount = classicProcessor.activeLayerCount();
        if (displayedLayerCount != activeCount)
        {
            displayedLayerCount = activeCount;
            addLayerButton.setEnabled(activeCount < Sf2Engine::layerCount);
            layoutLayerStrips();
        }
        classicProcessor.refreshStandaloneMidiInputs();
        for (auto& strip : strips)
            if (strip != nullptr) strip->refreshMidiDevices();
    }
    masterMeter.setLevel(std::sqrt(juce::jlimit(0.0f, 1.0f, masterLevel)));
}

void ClassicPlayerAudioProcessorEditor::handleNoteOn(juce::MidiKeyboardState*, int, int note, float)
{
    if (juce::isPositiveAndBelow(note, 128)) heldNotes[(size_t) note].store(true);
    triggerAsyncUpdate();
}

void ClassicPlayerAudioProcessorEditor::handleNoteOff(juce::MidiKeyboardState*, int, int note, float)
{
    if (juce::isPositiveAndBelow(note, 128)) heldNotes[(size_t) note].store(false);
    triggerAsyncUpdate();
}

void ClassicPlayerAudioProcessorEditor::handleAsyncUpdate()
{
    const auto chord = detectedChord();
    const auto fontSize = chord.length() <= 5 ? 48.0f
                         : chord.length() <= 9 ? 39.0f
                         : chord.length() <= 13 ? 30.0f
                         : chord.length() <= 18 ? 23.0f : 18.0f;
    chordLabel.setFont(juce::FontOptions(fontSize, juce::Font::bold));
    chordLabel.setText(chord, juce::dontSendNotification);
}

juce::String ClassicPlayerAudioProcessorEditor::detectedChord() const
{
    std::vector<int> notes;
    for (int note = 0; note < 128; ++note)
        if (heldNotes[(size_t) note].load()) notes.push_back(note);
    return ClassicChordDetector::detect(notes);
}

void ClassicPlayerAudioProcessorEditor::applyMixerStates()
{
    bool anySolo = false;
    const auto count = classicProcessor.activeLayerCount();
    for (int i = 0; i < count; ++i)
        anySolo = anySolo || strips[(size_t) i]->isSolo();
    for (int i = 0; i < count; ++i)
    {
        auto& strip = strips[(size_t) i];
        strip->setEngineEnabled(anySolo ? strip->isSolo() : !strip->isMuted());
    }
}

void ClassicPlayerAudioProcessorEditor::addLayer()
{
    const auto newLayerIndex = classicProcessor.activeLayerCount();
    if (!classicProcessor.addLayer()) return;
    displayedLayerCount = classicProcessor.activeLayerCount();
    if (strips[(size_t) newLayerIndex] != nullptr)
    {
        strips[(size_t) newLayerIndex]->setVisible(true);
        strips[(size_t) newLayerIndex]->refresh();
    }
    addLayerButton.setEnabled(classicProcessor.activeLayerCount() < Sf2Engine::layerCount);
    layoutLayerStrips();
    layerViewport.setViewPosition(juce::jmax(0, layerContent.getWidth() - layerViewport.getWidth()), 0);
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::removeLayer(int layer)
{
    if (!classicProcessor.removeLayer(layer)) return;
    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
        if (strips[(size_t) i] != nullptr)
            strips[(size_t) i]->setVisible(i < displayedLayerCount);
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::layoutLayerStrips()
{
    const auto count = classicProcessor.activeLayerCount();
    if (count <= 0 || layerViewport.getWidth() <= 0) return;
    constexpr int gap = 8;
    constexpr int initiallyVisible = Sf2Engine::defaultLayerCount;
    const auto stripWidth = juce::jmax(
        220, (layerViewport.getWidth() - gap * (initiallyVisible - 1)) / initiallyVisible);
    const auto contentWidth = juce::jmax(
        layerViewport.getWidth(), count * stripWidth + juce::jmax(0, count - 1) * gap);
    // Full height required by LayerStrip::resized(); small displays use the
    // viewport scrollbar instead of clipping routing and velocity controls.
    constexpr int minimumStripHeight = 590;
    const auto contentHeight = juce::jmax(minimumStripHeight,
        layerViewport.getHeight() - layerViewport.getScrollBarThickness());
    layerContent.setSize(contentWidth, contentHeight);
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < count);
        strips[(size_t) i]->setBounds(i * (stripWidth + gap), 0, stripWidth, contentHeight);
    }
}

void ClassicPlayerAudioProcessorEditor::activate()
{
    if (LicenseVerifier::activateAndStore(activationCode.getText()))
    {
        classicProcessor.refreshActivation();
        activationPanel.setVisible(false);
    }
    else
        activationStatus.setText("Código de ativação inválido.", juce::dontSendNotification);
}
