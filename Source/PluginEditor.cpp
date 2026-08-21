#include "PluginEditor.h"
#include "LicenseVerifier.h"
#include "ClassicPlayerAssets.h"
#include "ChordDetector.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
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

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        auto colour = backgroundColour;
        if (shouldDrawButtonAsDown) colour = colour.darker(0.12f);
        else if (shouldDrawButtonAsHighlighted) colour = colour.brighter(0.08f);
        g.setColour(colour);
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(juce::Colour(line));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
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

struct KnobEditorSpec
{
    const char* label;
    float value;
    float minimum;
    float maximum;
    float interval;
    int decimals;
};

class KnobEditorPanel final : public juce::Component
{
public:
    KnobEditorPanel(std::initializer_list<KnobEditorSpec> specifications, int requestedColumns)
        : columns(juce::jmax(1, requestedColumns))
    {
        for (const auto& specification : specifications)
        {
            auto* label = labels.add(new juce::Label());
            label->setText(specification.label, juce::dontSendNotification);
            label->setJustificationType(juce::Justification::centred);
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            addAndMakeVisible(label);

            auto* knob = knobs.add(new juce::Slider());
            knob->setRange(specification.minimum, specification.maximum, specification.interval);
            knob->setValue(specification.value, juce::dontSendNotification);
            knob->setNumDecimalPlacesToDisplay(specification.decimals);
            knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
            knob->setLookAndFeel(&classicLookAndFeel);
            knob->setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(teal));
            addAndMakeVisible(knob);
        }

        const auto rows = juce::jmax(1, (knobs.size() + columns - 1) / columns);
        setSize(columns * 126, rows * 124);
    }

    void useAnalogHardwareLayout(bool shouldUse) { analogHardwareLayout = shouldUse; resized(); }

    float value(int item) const
    {
        return juce::isPositiveAndBelow(item, knobs.size()) ? (float) knobs[item]->getValue() : 0.0f;
    }

    void setValue(int item, float newValue)
    {
        if (juce::isPositiveAndBelow(item, knobs.size()))
            knobs[item]->setValue(newValue, juce::dontSendNotification);
    }

    void setOnValueChange(std::function<void()> callback)
    {
        onValueChange = std::move(callback);
        for (auto* knob : knobs)
            knob->onValueChange = [this] { if (onValueChange) onValueChange(); };
    }

    void resized() override
    {
        if (analogHardwareLayout && knobs.size() == 19)
        {
            // Keep every label and knob inside a predictable three-row grid.
            // The old hand-written coordinates assumed a 1000px panel and
            // overlapped when the editor was resized by JUCE on smaller Macs.
            constexpr int gridColumns = 5;
            const auto cellWidth = getWidth() / gridColumns;
            const auto rowHeight = juce::jmax(104, getHeight() / 4);
            for (int item = 0; item < knobs.size(); ++item)
            {
                const auto column = item % gridColumns;
                const auto row = item / gridColumns;
                auto cell = juce::Rectangle<int>(column * cellWidth, row * rowHeight,
                                                 cellWidth, rowHeight).reduced(4, 2);
                labels[item]->setBounds(cell.removeFromTop(17));
                knobs[item]->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 18);
                knobs[item]->setBounds(cell.reduced(3, 1));
            }
            return;
        }

        const auto cellWidth = getWidth() / columns;
        for (int item = 0; item < knobs.size(); ++item)
        {
            const auto column = item % columns;
            const auto row = item / columns;
            auto cell = juce::Rectangle<int>(column * cellWidth, row * 124, cellWidth, 124).reduced(5, 2);
            labels[item]->setBounds(cell.removeFromTop(21));
            knobs[item]->setBounds(cell.reduced(2, 0));
        }
    }

private:
    int columns = 1;
    bool analogHardwareLayout = false;
    std::function<void()> onValueChange;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Slider> knobs;
};

class AnalogSynthEditorPanel final : public juce::Component
{
public:
    explicit AnalogSynthEditorPanel(const AnalogSynthEngine::Config& source)
        : initial(source),
          sourceAtOpen(source),
          waves { source.oscillator1Wave, source.oscillator2Wave, source.oscillator3Wave },
          knobs({
              { "OSC 1 LEVEL", source.oscillator1Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 2 LEVEL", source.oscillator2Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 3 LEVEL", source.oscillator3Level * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "OSC 2 TUNE", source.oscillator2Semitones, -24.0f, 24.0f, 1.0f, 0 },
              { "OSC 3 TUNE", source.oscillator3Semitones, -24.0f, 24.0f, 1.0f, 0 },
              { "NOISE", source.noiseLevel * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "CUTOFF", source.cutoff, 0.0f, 100.0f, 1.0f, 0 },
              { "EMPHASIS", source.resonance * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "FILTER CONTOUR", source.filterEnvelopeAmount * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "ATTACK ms", source.ampAttackMs, 1.0f, 2000.0f, 1.0f, 0 },
              { "DECAY ms", source.ampDecayMs, 1.0f, 3000.0f, 1.0f, 0 },
              { "SUSTAIN", source.ampSustain * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "RELEASE ms", source.ampReleaseMs, 1.0f, 5000.0f, 1.0f, 0 },
              { "LFO RATE Hz", source.lfoRateHz, 0.1f, 20.0f, 0.1f, 1 },
              { "LFO PITCH", source.lfoToPitch, 0.0f, 12.0f, 0.1f, 1 },
              { "LFO FILTER", source.lfoToFilter, 0.0f, 100.0f, 1.0f, 0 },
              { "DRIVE", source.mixerDrive * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "KEY TRACK", source.filterKeyboardTracking * 100.0f, 0.0f, 100.0f, 1.0f, 0 },
              { "MOD WHEEL", source.modWheelToFilter * 100.0f, 0.0f, 100.0f, 1.0f, 0 }
          }, 6)
    {
        configureWaveButton(wave1, 0);
        configureWaveButton(wave2, 1);
        configureWaveButton(wave3, 2);
        configureSwitch(oscillator1On, "OSC 1 ON", source.oscillator1Enabled);
        configureSwitch(oscillator2On, "OSC 2 ON", source.oscillator2Enabled);
        configureSwitch(oscillator3On, "OSC 3 ON", source.oscillator3Enabled);
        configureSwitch(pinkNoise, "PINK NOISE", source.pinkNoise);
        configureMonoPoly(source.monophonic);
        presetBox.addItem("INICIAL", 1);
        // The full MIT-licensed Minimoog factory list is represented here.
        // Values are adapted only to controls implemented by Classic Keys Analog.
        presetBox.addItem("[LEAD] SOLO LEAD", 2);
        presetBox.addItem("[PAD] WARM PAD", 3);
        presetBox.addItem("[PAD] ATMOSPHERIC PAD", 4);
        presetBox.addItem("[PAD] SPACE MOD", 5);
        presetBox.addItem("[LEAD] MODERN LEAD", 6);
        presetBox.addItem("[PAD] CRYSTAL PAD", 7);
        presetBox.addItem("[LEAD] EASY LEAD", 8);
        presetBox.addItem("[LEAD] SCREAMING LEAD", 9);
        presetBox.addItem("[PAD] DREAM PAD", 10);
        presetBox.addItem("[LEAD] CLASSIC MINIMOOG LEAD", 11);
        presetBox.addItem("[BASS] TAURUS BASS", 12);
        presetBox.addItem("[LEAD] ANALOG LEAD", 13);
        presetBox.addItem("[LEAD] UNISON LEAD", 14);
        presetBox.addItem("[BASS] THICK BASS", 15);
        presetBox.addItem("[BASS] FUNK BASS", 16);
        presetBox.addItem("[PAD] EASY PAD", 17);
        presetBox.addItem("[BASS] ANALOG BASS", 18);
        presetBox.addItem("[LEAD] VINTAGE LEAD", 19);
        presetBox.addItem("[BASS] PERCUSSIVE BASS", 20);
        presetBox.addItem("[LEAD] LUCKY MAN", 21);
        presetBox.addItem("[LEAD] WATERY LEAD", 22);
        presetBox.addItem("[PAD] VINTAGE MINIMOOG PAD", 23);
        presetBox.addItem("[BASS] AUTHENTIC MINIMOOG BASS", 24);
        presetBox.addItem("[LEAD] PROGRESSIVE ROCK LEAD", 25);
        presetBox.addItem("[PAD] SPACE ECHO", 26);
        presetBox.addItem("[PAD] CATHEDRAL PAD", 27);
        presetBox.addItem("[BASS] DUB BASS", 28);
        presetBox.addItem("[EXPERIMENTAL] ALIEN LANDSCAPE", 29);
        presetBox.addItem("[EXPERIMENTAL] GLASS HARMONICA", 30);
        presetBox.addItem("[EXPERIMENTAL] WIND CHIMES", 31);
        presetBox.addItem("[EXPERIMENTAL] SUBMARINE SONAR", 32);
        presetBox.addItem("[EXPERIMENTAL] CRYSTAL BELLS", 33);
        presetBox.addItem("[EXPERIMENTAL] THUNDER STORM", 34);
        presetBox.addItem("[EXPERIMENTAL] DIGITAL RAIN", 35);
        presetBox.addItem("[EXPERIMENTAL] COSMIC DRONE", 36);
        presetBox.setSelectedId(1, juce::dontSendNotification);
        presetBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
        presetBox.setColour(juce::ComboBox::textColourId, juce::Colour(text));
        presetBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(line));
        presetBox.onChange = [this]
        {
            applyFactoryPreset(presetBox.getSelectedId());
            // Preset selection is an audible action: update the layer now so
            // changing the name never requires a second confirmation click.
            if (onPresetChanged)
                onPresetChanged(config());
            else if (onConfigChanged)
                onConfigChanged(config());
        };
        addAndMakeVisible(presetBox);
        addAndMakeVisible(wave1);
        addAndMakeVisible(wave2);
        addAndMakeVisible(wave3);
        addAndMakeVisible(oscillator1On);
        addAndMakeVisible(oscillator2On);
        addAndMakeVisible(oscillator3On);
        addAndMakeVisible(pinkNoise);
        addAndMakeVisible(monoPoly);
        knobs.useAnalogHardwareLayout(true);
        knobs.setOnValueChange([this] { if (onConfigChanged) onConfigChanged(config()); });
        for (auto* button : { &oscillator1On, &oscillator2On, &oscillator3On, &pinkNoise })
            button->onClick = [this] { if (onConfigChanged) onConfigChanged(config()); };
        addAndMakeVisible(knobs);
        setSize(1000, 640);
    }

    AnalogSynthEngine::Config config() const
    {
        auto result = initial;
        result.oscillator1Wave = waves[0]; result.oscillator2Wave = waves[1]; result.oscillator3Wave = waves[2];
        result.oscillator1Level = knobs.value(0) / 100.0f; result.oscillator2Level = knobs.value(1) / 100.0f;
        result.oscillator3Level = knobs.value(2) / 100.0f; result.oscillator2Semitones = knobs.value(3);
        result.oscillator3Semitones = knobs.value(4); result.noiseLevel = knobs.value(5) / 100.0f;
        result.cutoff = knobs.value(6); result.resonance = knobs.value(7) / 100.0f;
        result.filterEnvelopeAmount = knobs.value(8) / 100.0f; result.ampAttackMs = knobs.value(9);
        result.ampDecayMs = knobs.value(10); result.ampSustain = knobs.value(11) / 100.0f;
        result.ampReleaseMs = knobs.value(12); result.lfoRateHz = knobs.value(13);
        result.lfoToPitch = knobs.value(14); result.lfoToFilter = knobs.value(15);
        result.glideMs = 0.0f;
        result.mixerDrive = knobs.value(16) / 100.0f;
        result.filterKeyboardTracking = knobs.value(17) / 100.0f;
        result.modWheelToFilter = knobs.value(18) / 100.0f;
        result.oscillator1Enabled = oscillator1On.getToggleState();
        result.oscillator2Enabled = oscillator2On.getToggleState();
        result.oscillator3Enabled = oscillator3On.getToggleState();
        result.pinkNoise = pinkNoise.getToggleState();
        result.monophonic = monoPoly.getToggleState();
        result.routing.mono = result.monophonic;
        return result;
    }

    std::function<void(const AnalogSynthEngine::Config&)> onPresetChanged;
    std::function<void(const AnalogSynthEngine::Config&)> onConfigChanged;

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff111214));
        const auto wood = juce::Colour(0xff5e3522), woodLight = juce::Colour(0xffa8663d);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff7f4b2b), 0.0f, 0.0f,
                                               juce::Colour(0xff432319), 0.0f, 48.0f, false));
        g.fillRect(0, 0, getWidth(), 48);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff432319), 0.0f, (float) getHeight() - 42.0f,
                                               juce::Colour(0xff7f4b2b), 0.0f, (float) getHeight(), false));
        g.fillRect(0, getHeight() - 42, getWidth(), 42);
        g.setColour(woodLight); g.drawLine(0.0f, 3.0f, (float) getWidth(), 3.0f, 1.5f);
        g.drawLine(0.0f, (float) getHeight() - 4.0f, (float) getWidth(), (float) getHeight() - 4.0f, 1.5f);

        const auto cabinet = juce::Rectangle<int>(18, 48, getWidth() - 36, getHeight() - 90);
        g.setColour(juce::Colour(0xff222324)); g.fillRect(cabinet);
        g.setColour(juce::Colour(0xff77716a)); g.drawRect(cabinet, 2);

        const auto icon = embeddedImage("classicplayerappicon_png");
        if (icon.isValid()) g.drawImageWithin(icon, 26, 4, 38, 38, juce::RectanglePlacement::centred);
        g.setColour(juce::Colour(teal)); g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText("CLASSIC KEYS ANALOG", 76, 10, 340, 25, juce::Justification::left);
        g.setColour(juce::Colour(text)); g.setFont(juce::FontOptions(10.0f));
        g.drawText("SINTETIZADOR NATIVO DE TRÊS OSCILADORES", 77, 31, 360, 13, juce::Justification::left);

        const int left = 20, top = 78, width = getWidth() - 40, height = getHeight() - 128;
        const std::array<float, 5> edges {{ 0.17f, 0.46f, 0.62f, 0.88f, 1.0f }};
        int x = left;
        for (int group = 0; group < 5; ++group)
        {
            const int right = left + juce::roundToInt(width * edges[(size_t) group]);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff272728), (float) x, (float) top,
                                                   juce::Colour(0xff161718), (float) x, (float) (top + height), false));
            g.fillRect(x, top, right - x, height);
            g.setColour(juce::Colour(0xff514d49)); g.drawRect(x, top, right - x, height, 1);
            x = right;
        }

        // Replace the non-functional static meter with a compact oscilloscope.
        // It is intentionally a visual monitor: audio metering remains in the
        // main layer, while this display makes the Analog editor feel alive.
        const auto scope = juce::Rectangle<float>((float) left + width * 0.895f,
                                                  (float) top + 76.0f,
                                                  width * 0.085f, 118.0f);
        g.setColour(juce::Colour(0xff080909));
        g.fillRoundedRectangle(scope, 3.0f);
        g.setColour(juce::Colour(0xff4d3d20));
        g.drawRoundedRectangle(scope, 3.0f, 1.0f);
        g.setColour(juce::Colour(0xff2b2b22));
        g.drawHorizontalLine(juce::roundToInt(scope.getCentreY()), scope.getX() + 4.0f, scope.getRight() - 4.0f);
        juce::Path waveform;
        for (int sample = 0; sample <= 48; ++sample)
        {
            const auto t = static_cast<float>(sample) / 48.0f;
            const auto y = scope.getCentreY() - std::sin(t * juce::MathConstants<float>::twoPi * 2.0f)
                                           * scope.getHeight() * 0.32f;
            const auto xPos = scope.getX() + 5.0f + t * (scope.getWidth() - 10.0f);
            if (sample == 0) waveform.startNewSubPath(xPos, y);
            else waveform.lineTo(xPos, y);
        }
        g.setColour(juce::Colour(0xffe09a27));
        g.strokePath(waveform, juce::PathStrokeType(1.5f));
        g.setColour(juce::Colour(0xffd6d2cc)); g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("OSCILLOSCOPE", scope.getX() - 8.0f, scope.getBottom() + 9.0f,
                   scope.getWidth() + 16.0f, 14.0f, juce::Justification::centred);
    }

    void resized() override
    {
        const auto w = getWidth();
        presetBox.setBounds(25, 53, juce::jmin(270, w / 3), 24);
        const auto waveY = 90;
        const auto waveWidth = juce::jmax(112, juce::jmin(154, (w - 300) / 5));
        wave1.setBounds(juce::roundToInt(w * 0.19f), waveY, waveWidth, 26);
        wave2.setBounds(juce::roundToInt(w * 0.37f), waveY, waveWidth, 26);
        wave3.setBounds(juce::roundToInt(w * 0.55f), waveY, waveWidth, 26);
        oscillator1On.setBounds(wave1.getX() + 10, 119, waveWidth - 20, 20);
        oscillator2On.setBounds(wave2.getX() + 10, 119, waveWidth - 20, 20);
        oscillator3On.setBounds(wave3.getX() + 10, 119, waveWidth - 20, 20);
        pinkNoise.setBounds(juce::roundToInt(w * 0.73f), 119, juce::jmin(104, w / 8), 20);
        monoPoly.setBounds(juce::roundToInt(w * 0.06f), 108, juce::jmin(112, w / 7), 25);
        // Reserve the right edge for the output meter and keep four complete
        // rows of controls inside the cabinet on compact displays.
        knobs.setBounds(22, 153, juce::jmax(560, getWidth() - 190), juce::jmax(430, getHeight() - 195));
    }

private:
    void applyFactoryPreset(int preset)
    {
        if (preset <= 1)
        {
            setFromConfig(sourceAtOpen);
            return;
        }

        // These are the 35 distinct, audible configurations used by the web
        // laboratory.  They intentionally live here rather than being inferred
        // from the name: selecting a preset must change the actual synth voice.
        struct WebPreset
        {
            AnalogSynthEngine::Waveform one, two, three;
            float oneLevel, twoLevel, threeLevel;
            float oneTune, twoTune, threeTune;
            float cutoff, resonance, attack, decay, sustain, release, lfo, glide, modulation, noise;
            bool mono;
        };

        using W = AnalogSynthEngine::Waveform;
        static const std::array<WebPreset, 35> webPresets {{
            { W::saw, W::triangle, W::saw, .66f,.36f,0.f, 0,0,0, 2350,.20f,.034f,3.90f,.76f,.048f,4.0f,.06f,.25f,0.f, true },
            { W::triangle,W::triangle,W::triangle,.55f,.34f,.16f,0,7,12,920,1.5f,.56f,.72f,.80f,2.4f,.28f,0,.31f,0,false },
            { W::triangle,W::saw,W::saw,.36f,.44f,.15f,0,12,24,820,2.1f,.90f,1.10f,.76f,3.1f,.12f,0,.52f,0,false },
            { W::saw,W::triangle,W::triangle,.34f,.52f,.16f,0,12,19,1100,3.0f,.65f,.90f,.70f,2.3f,.15f,0,.60f,0,false },
            { W::square,W::saw,W::saw,.57f,.38f,.28f,0,12,19,6100,2.4f,.006f,.18f,.62f,.09f,5.8f,.05f,.38f,0,true },
            { W::triangle,W::triangle,W::saw,.50f,.33f,.11f,0,12,19,2800,2.0f,.38f,.60f,.72f,1.5f,.33f,0,.38f,0,false },
            { W::square,W::triangle,W::saw,.62f,.26f,.19f,0,7,12,2500,4.0f,.015f,.34f,.70f,.13f,2.2f,.10f,.34f,0,true },
            { W::saw,W::saw,W::square,.70f,.42f,.15f,0,12,24,3800,11.f,.004f,.14f,.57f,.07f,6.5f,.035f,.42f,0,true },
            { W::triangle,W::triangle,W::sine,.62f,.36f,.12f,0,7,12,1250,2.0f,.50f,.70f,.78f,1.9f,.23f,0,.46f,0,false },
            { W::saw,W::saw,W::square,.76f,.41f,.13f,0,0,-12,760,12.f,.013f,.28f,.58f,.10f,4.5f,.08f,.16f,0,true },
            { W::saw,W::square,W::triangle,.80f,.35f,.15f,-12,-12,-24,380,13.f,.008f,.25f,.70f,.18f,2.0f,.04f,.05f,0,true },
            { W::square,W::triangle,W::saw,.48f,.46f,.18f,-12,-24,0,650,5.5f,.010f,.29f,.61f,.17f,1.8f,.025f,.05f,0,true },
            { W::saw,W::saw,W::square,.62f,.48f,.18f,0,12,12,4100,3.2f,.009f,.24f,.64f,.10f,5.1f,.06f,.28f,0,true },
            { W::saw,W::saw,W::square,.70f,.40f,.13f,-12,-24,-12,540,8.f,.006f,.20f,.69f,.13f,2.2f,.03f,.06f,0,true },
            { W::saw,W::square,W::triangle,.62f,.45f,.10f,-12,0,-12,720,6.f,.004f,.13f,.45f,.10f,3.f,0,.08f,0,false },
            { W::triangle,W::triangle,W::sine,.62f,.36f,.12f,0,7,12,1250,2.f,.50f,.70f,.78f,1.9f,.23f,0,.46f,0,false },
            { W::square,W::triangle,W::saw,.48f,.46f,.18f,-12,-24,0,650,5.5f,.010f,.29f,.61f,.17f,1.8f,.025f,.05f,0,true },
            { W::triangle,W::saw,W::triangle,.72f,.27f,.12f,0,0,-12,1120,5.2f,.025f,.48f,.66f,.14f,3.6f,.05f,.11f,0,true },
            { W::square,W::saw,W::triangle,.62f,.37f,.11f,-12,-12,-24,980,8.6f,.004f,.09f,.34f,.055f,3.f,.01f,.04f,0,true },
            { W::triangle,W::saw,W::sine,.55f,.52f,.12f,-12,12,0,1450,4.5f,.022f,.55f,.68f,.12f,3.2f,.12f,.12f,0,true },
            { W::saw,W::sine,W::triangle,.49f,.31f,.28f,0,12,19,2050,2.6f,.030f,.70f,.72f,.20f,1.1f,.09f,.46f,0,true },
            { W::saw,W::triangle,W::square,.31f,.44f,.09f,0,12,19,1030,3.8f,.48f,.80f,.73f,2.1f,.22f,0,.48f,0,false },
            { W::saw,W::saw,W::triangle,.75f,.32f,.09f,-12,-12,-24,460,11.f,.008f,.22f,.55f,.16f,2.5f,.02f,.08f,0,true },
            { W::saw,W::saw,W::square,.68f,.43f,.14f,0,12,24,3000,6.5f,.012f,.30f,.60f,.11f,4.8f,.06f,.30f,0,true },
            { W::saw,W::triangle,W::sine,.37f,.45f,.18f,0,12,19,1450,3.1f,.18f,.85f,.68f,2.6f,.18f,0,.48f,0,false },
            { W::saw,W::triangle,W::square,.33f,.40f,.12f,0,12,19,970,3.8f,.62f,1.10f,.75f,3.0f,.17f,0,.40f,0,false },
            { W::sine,W::square,W::triangle,.52f,.32f,.28f,-24,-12,-12,390,4.2f,.018f,.45f,.74f,.27f,.75f,.05f,.09f,0,true },
            { W::sine,W::triangle,W::square,.40f,.27f,.09f,0,19,24,900,5.f,.70f,1.10f,.65f,2.6f,.12f,0,.72f,.08f,false },
            { W::sine,W::triangle,W::sine,.52f,.24f,.18f,0,12,19,3150,2.8f,.16f,.75f,.62f,2.2f,.42f,0,.34f,0,false },
            { W::triangle,W::sine,W::sine,.38f,.30f,.22f,0,12,24,2600,2.2f,.10f,.85f,.66f,2.8f,.35f,0,.28f,.02f,false },
            { W::sine,W::sine,W::triangle,.80f,.25f,.08f,0,12,-12,650,12.f,.010f,.40f,.10f,2.8f,.18f,.10f,.18f,0,true },
            { W::sine,W::triangle,W::sine,.60f,.28f,.18f,0,12,19,3400,3.4f,.03f,.55f,.48f,2.0f,.65f,0,.36f,0,false },
            { W::square,W::saw,W::sine,.18f,.26f,.33f,-12,0,19,720,8.f,.30f,.90f,.55f,3.2f,.14f,0,.78f,.12f,false },
            { W::square,W::sine,W::triangle,.33f,.40f,.19f,0,12,24,2300,4.f,.010f,.12f,.25f,.45f,7.f,0,.30f,0,false },
            { W::saw,W::sine,W::triangle,.27f,.43f,.25f,-12,0,7,1250,4.5f,.18f,1.20f,.83f,4.0f,.09f,0,.70f,0,false }
        }};

        const auto& presetValue = webPresets[(size_t) juce::jlimit(2, 36, preset) - 2];
        auto browserValue = sourceAtOpen;
        browserValue.oscillator1Wave = presetValue.one; browserValue.oscillator2Wave = presetValue.two; browserValue.oscillator3Wave = presetValue.three;
        browserValue.oscillator1Level = presetValue.oneLevel; browserValue.oscillator2Level = presetValue.twoLevel; browserValue.oscillator3Level = presetValue.threeLevel;
        browserValue.oscillator1Semitones = presetValue.oneTune; browserValue.oscillator2Semitones = presetValue.twoTune; browserValue.oscillator3Semitones = presetValue.threeTune;
        browserValue.oscillator1Enabled = presetValue.oneLevel > 0.0f; browserValue.oscillator2Enabled = presetValue.twoLevel > 0.0f; browserValue.oscillator3Enabled = presetValue.threeLevel > 0.0f;
        browserValue.noiseLevel = presetValue.noise; browserValue.cutoff = juce::jlimit(0.0f, 100.0f, 100.0f * std::log(juce::jmax(25.0f, presetValue.cutoff) / 25.0f) / std::log(700.0f));
        browserValue.resonance = juce::jlimit(0.0f, 1.0f, presetValue.resonance / 20.0f); browserValue.filterEnvelopeAmount = juce::jlimit(0.0f, 1.0f, presetValue.modulation);
        browserValue.ampAttackMs = presetValue.attack * 1000.0f; browserValue.ampDecayMs = presetValue.decay * 1000.0f; browserValue.ampSustain = presetValue.sustain; browserValue.ampReleaseMs = presetValue.release * 1000.0f;
        browserValue.filterAttackMs = browserValue.ampAttackMs; browserValue.filterDecayMs = browserValue.ampDecayMs; browserValue.filterSustain = browserValue.ampSustain; browserValue.filterReleaseMs = browserValue.ampReleaseMs;
        browserValue.lfoRateHz = presetValue.lfo; browserValue.lfoToFilter = presetValue.modulation * 40.0f; browserValue.lfoToPitch = presetValue.modulation * 0.25f;
        // Classic Keys Analog exposes Mono/Legato only; factory glide values
        // from the web presets are intentionally ignored.
        browserValue.glideMs = 0.0f; browserValue.monophonic = presetValue.mono; browserValue.routing.mono = presetValue.mono; browserValue.routing.portamento = false;
        setFromConfig(browserValue);
        return;

    }

    void setFromConfig(const AnalogSynthEngine::Config& value)
    {
        initial = value; waves = { value.oscillator1Wave, value.oscillator2Wave, value.oscillator3Wave };
        wave1.setButtonText("OSC 1 - " + waveformName(waves[0])); wave2.setButtonText("OSC 2 - " + waveformName(waves[1]));
        wave3.setButtonText("OSC 3 - " + waveformName(waves[2]));
        knobs.setValue(0, value.oscillator1Level * 100.0f); knobs.setValue(1, value.oscillator2Level * 100.0f);
        knobs.setValue(2, value.oscillator3Level * 100.0f); knobs.setValue(3, value.oscillator2Semitones);
        knobs.setValue(4, value.oscillator3Semitones); knobs.setValue(5, value.noiseLevel * 100.0f);
        knobs.setValue(6, value.cutoff); knobs.setValue(7, value.resonance * 100.0f);
        knobs.setValue(8, value.filterEnvelopeAmount * 100.0f); knobs.setValue(9, value.ampAttackMs);
        knobs.setValue(10, value.ampDecayMs); knobs.setValue(11, value.ampSustain * 100.0f);
        knobs.setValue(12, value.ampReleaseMs); knobs.setValue(13, value.lfoRateHz);
        knobs.setValue(14, value.lfoToPitch); knobs.setValue(15, value.lfoToFilter);
        knobs.setValue(16, value.mixerDrive * 100.0f);
        knobs.setValue(17, value.filterKeyboardTracking * 100.0f);
        knobs.setValue(18, value.modWheelToFilter * 100.0f);
        oscillator1On.setToggleState(value.oscillator1Enabled, juce::dontSendNotification);
        oscillator2On.setToggleState(value.oscillator2Enabled, juce::dontSendNotification);
        oscillator3On.setToggleState(value.oscillator3Enabled, juce::dontSendNotification);
        pinkNoise.setToggleState(value.pinkNoise, juce::dontSendNotification);
        monoPoly.setToggleState(value.monophonic, juce::dontSendNotification);
        updateMonoPolyText();
    }

    static juce::String waveformName(AnalogSynthEngine::Waveform waveform)
    {
        switch (waveform) { case AnalogSynthEngine::Waveform::triangle: return "TRIANGLE"; case AnalogSynthEngine::Waveform::saw: return "SAW"; case AnalogSynthEngine::Waveform::square: return "SQUARE"; case AnalogSynthEngine::Waveform::pulse: return "PULSE"; case AnalogSynthEngine::Waveform::sine: return "SINE"; }
        return "SAW";
    }

    void configureWaveButton(juce::TextButton& button, int oscillator)
    {
        flatButton(button); button.setClickingTogglesState(false); auto* buttonPtr = &button;
        button.onClick = [this, oscillator, buttonPtr]
        {
            auto value = static_cast<int>(waves[(size_t) oscillator]);
            waves[(size_t) oscillator] = static_cast<AnalogSynthEngine::Waveform>((value + 1) % 5);
            buttonPtr->setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
            if (onConfigChanged) onConfigChanged(config());
        };
        button.setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
    }

    void configureSwitch(juce::TextButton& button, const juce::String& label, bool enabled)
    {
        flatButton(button);
        button.setClickingTogglesState(true);
        button.setButtonText(label);
        button.setToggleState(enabled, juce::dontSendNotification);
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff1f8f89));
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void configureMonoPoly(bool monophonic)
    {
        flatButton(monoPoly);
        monoPoly.setClickingTogglesState(true);
        monoPoly.setToggleState(monophonic, juce::dontSendNotification);
        monoPoly.onClick = [this] { updateMonoPolyText(); if (onConfigChanged) onConfigChanged(config()); };
        updateMonoPolyText();
    }

    void updateMonoPolyText()
    {
        monoPoly.setButtonText(monoPoly.getToggleState() ? "MONO / LEGATO" : "POLI");
    }

    AnalogSynthEngine::Config initial, sourceAtOpen;
    std::array<AnalogSynthEngine::Waveform, 3> waves;
    juce::ComboBox presetBox;
    juce::TextButton wave1, wave2, wave3;
    juce::TextButton oscillator1On, oscillator2On, oscillator3On, pinkNoise, monoPoly;
    KnobEditorPanel knobs;
};

class Sf2EditorPanel final : public juce::Component
{
public:
    Sf2EditorPanel(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        for (auto* label : { &categoryLabel, &libraryLabel, &presetLabel, &modeLabel,
                             &sustainLabel, &channelLabel, &deviceLabel, &octaveLabel,
                             &rangeLabel, &velocityLabel })
        {
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            addAndMakeVisible(*label);
        }
        categoryLabel.setText("CATEGORIA", juce::dontSendNotification);
        libraryLabel.setText("BIBLIOTECA SF2", juce::dontSendNotification);
        presetLabel.setText("PRESET", juce::dontSendNotification);
        modeLabel.setText("MODO", juce::dontSendNotification);
        sustainLabel.setText("SUSTAIN", juce::dontSendNotification);
        channelLabel.setText("CANAL MIDI", juce::dontSendNotification);
        deviceLabel.setText("ENTRADA MIDI", juce::dontSendNotification);
        octaveLabel.setText("OITAVA", juce::dontSendNotification);
        rangeLabel.setText("FAIXA DE NOTAS", juce::dontSendNotification);
        velocityLabel.setText("VELOCIDADE", juce::dontSendNotification);

        for (const auto& category : ClassicPlayerAudioProcessor::soundFontCategories())
            categoryBox.addItem(category, categoryBox.getNumItems() + 1);
        categoryBox.setSelectedId(1, juce::dontSendNotification);
        categoryBox.onChange = [this] { rebuildLibrary(); };
        libraryBox.onChange = [this]
        {
            const auto selected = libraryBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, libraryFiles.size()))
            {
                const auto result = processor.loadSoundFont(index, libraryFiles.getReference(selected));
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                           "Falha ao carregar SF2", result.getErrorMessage());
                rebuildPresets();
            }
        };
        presetBox.onChange = [this]
        {
            const auto selected = presetBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, (int) presets.size()))
                processor.selectLayerPreset(index, presets[(size_t) selected].bank,
                                             presets[(size_t) selected].program);
        };
        importButton.setButtonText("IMPORTAR SF2");
        deleteButton.setButtonText("EXCLUIR SF2");
        flatButton(importButton); flatButton(deleteButton);
        importButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>("Importar SoundFont", juce::File{}, "*.sf2");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& c)
                {
                    const auto source = c.getResult();
                    if (!source.existsAsFile()) return;
                    juce::File imported;
                    const auto result = processor.importSoundFont(source, categoryBox.getText(), imported);
                    if (result.failed())
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Falha ao importar SF2", result.getErrorMessage());
                    else
                    {
                        processor.loadSoundFont(index, imported);
                        rebuildLibrary();
                    }
                });
        };
        deleteButton.onClick = [this]
        {
            const auto selected = libraryBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(selected, libraryFiles.size()))
            {
                processor.deleteLibrarySoundFont(libraryFiles.getReference(selected));
                rebuildLibrary();
            }
        };
        for (auto* box : { &categoryBox, &libraryBox, &presetBox, &modeBox, &sustainBox,
                           &channelBox, &deviceBox, &octaveBox, &lowNoteBox, &highNoteBox,
                           &velocityBox })
        {
            addAndMakeVisible(*box);
            box->setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
            box->setColour(juce::ComboBox::textColourId, juce::Colour(text));
        }
        addAndMakeVisible(importButton); addAndMakeVisible(deleteButton);
        modeBox.addItem("POLI", 1); modeBox.addItem("MONO / LEGATO", 2); modeBox.addItem("PORTAMENTO", 3);
        sustainBox.addItem("SUSTAIN ON", 1); sustainBox.addItem("SUSTAIN OFF", 2);
        channelBox.addItem("MIDI OMNI", 1);
        for (int channel = 1; channel <= 16; ++channel) channelBox.addItem("MIDI CH " + juce::String(channel), channel + 1);
        for (int value = -4; value <= 4; ++value) octaveBox.addItem((value > 0 ? "+" : "") + juce::String(value) + " OIT", value + 5);
        for (int note = 0; note < 128; ++note) { lowNoteBox.addItem(midiNoteName(note), note + 1); highNoteBox.addItem(midiNoteName(note), note + 1); }
        velocityBox.addItem("VEL LINEAR", 1); velocityBox.addItem("VEL SOFT", 2); velocityBox.addItem("VEL HARD", 3);
        for (auto* box : { &modeBox, &sustainBox, &channelBox, &octaveBox, &lowNoteBox, &highNoteBox, &velocityBox })
            box->onChange = [this] { applyRouting(); };
        deviceBox.addItem("TODOS OS CONTROLADORES", 1);
        for (const auto& device : processor.availableMidiDevices())
            deviceBox.addItem(device.name, deviceBox.getNumItems() + 1);
        deviceBox.onChange = [this]
        {
            const auto selected = deviceBox.getSelectedId() - 2;
            const auto devices = processor.availableMidiDevices();
            processor.setLayerMidiDevice(index, juce::isPositiveAndBelow(selected, devices.size())
                                                   ? devices.getReference(selected).identifier : juce::String{});
        };
        refreshFromProcessor();
        setSize(700, 430);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto top = area.removeFromTop(26);
        categoryLabel.setBounds(top.removeFromLeft(105)); categoryBox.setBounds(top.removeFromLeft(250));
        importButton.setBounds(top.removeFromLeft(150).reduced(2)); deleteButton.setBounds(top.reduced(2));
        auto row = area.removeFromTop(25); libraryLabel.setBounds(row.removeFromLeft(105)); libraryBox.setBounds(row);
        row = area.removeFromTop(25); presetLabel.setBounds(row.removeFromLeft(105)); presetBox.setBounds(row);
        area.removeFromTop(10);
        const auto cell = area.getWidth() / 3;
        auto place = [cell](juce::Rectangle<int> r, juce::Label& l, juce::ComboBox& b) { l.setBounds(r.removeFromTop(18)); b.setBounds(r.reduced(2)); };
        for (int rowIndex = 0; rowIndex < 3; ++rowIndex)
        {
            auto line = area.removeFromTop(62);
            if (rowIndex == 0) { place(line.removeFromLeft(cell), modeLabel, modeBox); place(line.removeFromLeft(cell), sustainLabel, sustainBox); place(line, channelLabel, channelBox); }
            if (rowIndex == 1) { place(line.removeFromLeft(cell), deviceLabel, deviceBox); place(line.removeFromLeft(cell), octaveLabel, octaveBox); place(line, velocityLabel, velocityBox); }
            if (rowIndex == 2) { place(line.removeFromLeft(cell), rangeLabel, lowNoteBox); place(line.removeFromLeft(cell), rangeLabel, highNoteBox); }
        }
    }

private:
    void rebuildLibrary()
    {
        libraryFiles = processor.librarySoundFonts(categoryBox.getText());
        libraryBox.clear(juce::dontSendNotification);
        for (int i = 0; i < libraryFiles.size(); ++i) libraryBox.addItem(libraryFiles.getReference(i).getFileNameWithoutExtension(), i + 1);
        libraryBox.setTextWhenNothingSelected("ESCOLHA O SF2");
        rebuildPresets();
    }
    void rebuildPresets()
    {
        presets.clear(); presetBox.clear(juce::dontSendNotification);
        presets = processor.layerPresets(index);
        for (int i = 0; i < (int) presets.size(); ++i) presetBox.addItem(juce::String(presets[(size_t) i].bank) + ": " + presets[(size_t) i].name, i + 1);
    }
    void refreshFromProcessor()
    {
        const auto config = processor.layerConfig(index);
        modeBox.setSelectedId(config.portamento ? 3 : config.mono ? 2 : 1, juce::dontSendNotification);
        sustainBox.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
        channelBox.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
        octaveBox.setSelectedId(config.octave + 5, juce::dontSendNotification);
        lowNoteBox.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
        highNoteBox.setSelectedId(config.highNote + 1, juce::dontSendNotification);
        velocityBox.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
        rebuildLibrary();
    }
    void applyRouting()
    {
        auto config = processor.layerConfig(index);
        config.mono = modeBox.getSelectedId() == 2; config.portamento = modeBox.getSelectedId() == 3;
        config.sustainEnabled = sustainBox.getSelectedId() != 2; config.midiChannel = channelBox.getSelectedId() - 1;
        config.octave = octaveBox.getSelectedId() - 5; config.lowNote = lowNoteBox.getSelectedId() - 1; config.highNote = highNoteBox.getSelectedId() - 1;
        config.velocityCurve = velocityBox.getSelectedId() - 1; processor.setLayerConfig(index, config);
    }
    ClassicPlayerAudioProcessor& processor; int index;
    juce::Label categoryLabel, libraryLabel, presetLabel, modeLabel, sustainLabel, channelLabel, deviceLabel, octaveLabel, rangeLabel, velocityLabel;
    juce::ComboBox categoryBox, libraryBox, presetBox, modeBox, sustainBox, channelBox, deviceBox, octaveBox, lowNoteBox, highNoteBox, velocityBox;
    juce::TextButton importButton, deleteButton; juce::Array<juce::File> libraryFiles; std::vector<Sf2Engine::Preset> presets;
    std::unique_ptr<juce::FileChooser> chooser;
};

class Dx7EditorPanel final : public juce::Component
{
public:
    Dx7EditorPanel(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        bankLabel.setText("BANCO DX7", juce::dontSendNotification);
        patchLabel.setText("TIMBRE DX7", juce::dontSendNotification);
        for (auto* label : { &bankLabel, &patchLabel })
        {
            label->setColour(juce::Label::textColourId, juce::Colour(text));
            label->setFont(juce::FontOptions(12.0f, juce::Font::bold));
            addAndMakeVisible(*label);
        }
        const auto banks = processor.libraryDx7Banks();
        for (int i = 0; i < banks.size(); ++i)
            bankBox.addItem(banks.getReference(i).getFileNameWithoutExtension(), i + 1);
        bankBox.setTextWhenNothingSelected("BIBLIOTECA DX7 VAZIA");
        bankBox.setSelectedItemIndex(processor.dx7Path(index).isNotEmpty()
                                         ? juce::jmax(0, banks.indexOf(juce::File(processor.dx7Path(index)))) : -1,
                                     juce::dontSendNotification);
        bankBox.onChange = [this]
        {
            const auto selected = bankBox.getSelectedItemIndex();
            const auto banksNow = processor.libraryDx7Banks();
            if (juce::isPositiveAndBelow(selected, banksNow.size()))
            {
                processor.loadDx7(index, banksNow.getReference(selected));
                rebuildPatches();
            }
        };
        patchBox.onChange = [this]
        {
            const auto selected = patchBox.getSelectedItemIndex();
            if (selected >= 0) processor.selectDx7Patch(index, selected);
        };
        addAndMakeVisible(bankBox);
        addAndMakeVisible(patchBox);
        rebuildPatches();
        setSize(460, 150);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto row = area.removeFromTop(22);
        bankLabel.setBounds(row.removeFromLeft(100)); bankBox.setBounds(row);
        area.removeFromTop(10);
        row = area.removeFromTop(22);
        patchLabel.setBounds(row.removeFromLeft(100)); patchBox.setBounds(row);
    }

private:
    void rebuildPatches()
    {
        patchBox.clear(juce::dontSendNotification);
        const auto count = processor.dx7PatchCount(index);
        for (int patch = 0; patch < count; ++patch)
            patchBox.addItem(juce::String(patch + 1) + ": " + processor.dx7PatchName(index, patch), patch + 1);
        if (count > 0)
            patchBox.setSelectedId(processor.dx7SelectedPatch(index) + 1, juce::dontSendNotification);
    }

    ClassicPlayerAudioProcessor& processor;
    int index;
    juce::Label bankLabel, patchLabel;
    juce::ComboBox bankBox, patchBox;
};

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

ClassicPlayerAudioProcessorEditor::DrumPadPanel::DrumPadPanel(ClassicPlayerAudioProcessor& p)
    : processor(p)
{
    for (int pad = 0; pad < ClassicPlayerAudioProcessor::drumPadCount; ++pad)
    {
        auto& trigger = pads[(size_t) pad];
        trigger.setButtonText("PAD " + juce::String(pad + 1));
        trigger.setTooltip("Clique para tocar este pad");
        trigger.setColour(juce::TextButton::buttonColourId, juce::Colour(0xfff4f4ee));
        trigger.setColour(juce::TextButton::buttonOnColourId, juce::Colour(yellow));
        trigger.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff15191d));
        trigger.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff15191d));
        trigger.onClick = [this, pad] { processor.triggerDrumPad(pad); };
        addAndMakeVisible(trigger);

        auto& load = loadButtons[(size_t) pad];
        load.setButtonText("LOAD");
        load.setTooltip("Carregar MP3/WAV neste pad");
        flatButton(load);
        load.onClick = [this, pad] { chooseSample(pad); };
        addAndMakeVisible(load);

        auto& learn = learnButtons[(size_t) pad];
        learn.setButtonText("LEARN");
        learn.setTooltip("Aprender um CC MIDI exclusivo para este pad");
        flatButton(learn);
        learn.onClick = [this, pad] { processor.beginDrumPadMidiLearn(pad); refresh(); };
        addAndMakeVisible(learn);
    }
    refresh();
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::chooseSample(int pad)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Carregar áudio do pad", juce::File{}, "*.mp3;*.wav;*.aiff;*.flac");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this, pad](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file.existsAsFile())
            {
                const auto result = processor.loadDrumPad(pad, file);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Drum pad", result.getErrorMessage());
                refresh();
            }
        });
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::refresh()
{
    for (int pad = 0; pad < ClassicPlayerAudioProcessor::drumPadCount; ++pad)
    {
        auto& trigger = pads[(size_t) pad];
        const auto active = processor.isDrumPadPlaying(pad);
        const auto samplePath = processor.drumPadPath(pad);
        trigger.setButtonText(samplePath.isNotEmpty() ? processor.drumPadName(pad)
                                                      : "PAD " + juce::String(pad + 1));
        trigger.setColour(juce::TextButton::buttonColourId,
                          active ? juce::Colour(yellow) : juce::Colour(0xfff4f4ee));
        trigger.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff15191d));
        const auto cc = processor.drumPadMidiCC(pad);
        learnButtons[(size_t) pad].setButtonText(
            processor.isDrumPadMidiLearning(pad) ? "MOVE CC"
            : cc >= 0 ? "CC " + juce::String(cc) : "LEARN");
    }
}

void ClassicPlayerAudioProcessorEditor::DrumPadPanel::resized()
{
    constexpr int columns = 2;
    const auto cellWidth = getWidth() / columns;
    constexpr int rowHeight = 128;
    for (int pad = 0; pad < ClassicPlayerAudioProcessor::drumPadCount; ++pad)
    {
        const auto column = pad % columns;
        const auto row = pad / columns;
        auto cell = juce::Rectangle<int>(column * cellWidth, row * rowHeight,
                                         cellWidth, rowHeight - 4).reduced(5, 3);
        pads[(size_t) pad].setBounds(cell.removeFromTop(cell.getHeight() - 30));
        auto buttons = cell.removeFromBottom(27);
        loadButtons[(size_t) pad].setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(1, 0));
        learnButtons[(size_t) pad].setBounds(buttons.reduced(1, 0));
    }
}

ClassicPlayerAudioProcessorEditor::LayerStrip::LayerStrip(
    ClassicPlayerAudioProcessor& p, int layerIndex, std::function<void()> mixChanged)
    : processor(p), index(layerIndex), mixStateChanged(std::move(mixChanged)), drumPadPanel(p)
{
    layerTitle.setText("LAYER " + juce::String(index + 1), juce::dontSendNotification);
    layerTitle.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    layerTitle.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(layerTitle);
    addAndMakeVisible(drumPadPanel);
    drumPadPanel.setVisible(false);

    for (auto* button : { &muteButton, &soloButton, &resetButton, &removeButton, &editButton, &loadButton,
                          &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton, &openExternalEditorButton, &deleteLibraryButton })
    {
        flatButton(*button);
        addAndMakeVisible(*button);
    }
    muteButton.setClickingTogglesState(true);
    soloButton.setClickingTogglesState(true);
    muteButton.onClick = [this] { muted = muteButton.getToggleState(); mixStateChanged(); };
    soloButton.onClick = [this] { solo = soloButton.getToggleState(); mixStateChanged(); };
    resetButton.onClick = [this] { resetLayer(); };
    editButton.setTooltip("Mostrar ou ocultar os controles desta layer");
    editButton.onClick = [this]
    {
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::analog)
        {
            // Analog already has a dedicated, independent editor window.
            showAnalogSynthEditor();
            return;
        }
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::dx7)
        {
            showDx7Editor();
            return;
        }
        showLayerEditor();
    };
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
    dx7Button.onClick = [this]
    {
        if (processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::analog)
            showAnalogSynthEditor();
        else
            chooseDx7();
    };
    openExternalEditorButton.onClick = [this] { openExternalInstrumentEditor(); };
    externalInstrumentButton.setTooltip("Escolher manualmente um instrumento VST3/AU");
    openExternalEditorButton.setTooltip("Abrir a janela de configuração do instrumento virtual");
    dx7Button.setTooltip("Importar banco ou voz DX7 em formato SysEx (.syx)");
    deleteDx7LibraryButton.setTooltip("Excluir o banco DX7 selecionado da biblioteca");
    deleteDx7LibraryButton.onClick = [this] { deleteSelectedDx7Bank(); };
    dx7LibraryBox.setTextWhenNothingSelected("BIBLIOTECA DX7 VAZIA");
    dx7LibraryBox.onChange = [this]
    {
        const auto selected = dx7LibraryBox.getSelectedItemIndex();
        deleteDx7LibraryButton.setEnabled(juce::isPositiveAndBelow(selected, dx7LibraryFiles.size()));
        if (!juce::isPositiveAndBelow(selected, dx7LibraryFiles.size())) return;
        const auto result = processor.loadDx7(index, dx7LibraryFiles.getReference(selected));
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Falha ao carregar DX7", result.getErrorMessage());
        refresh();
    };
    dx7PatchBox.setTextWhenNothingSelected("SELECIONE O TIMBRE DX7");
    dx7PatchBox.onChange = [this]
    {
        const auto patch = dx7PatchBox.getSelectedItemIndex();
        if (processor.selectDx7Patch(index, patch)) refresh();
    };
    addAndMakeVisible(dx7LibraryBox);
    addAndMakeVisible(dx7PatchBox);
    addAndMakeVisible(deleteDx7LibraryButton);
    const auto canHost = processor.supportsExternalInstruments();
    externalInstrumentButton.setVisible(canHost);
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
    sourceSummary.setJustificationType(juce::Justification::centredLeft);
    sourceSummary.setColour(juce::Label::textColourId, juce::Colour(text));
    sourceSummary.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    addAndMakeVisible(sourceSummary);
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
    for (auto* button : { &reverbEditButton, &compressorEditButton })
    {
        flatButton(*button);
        button->setTooltip("Ajustar com precisão a intensidade do efeito");
        addAndMakeVisible(*button);
    }
    reverbEditButton.onClick = [this] { showReverbEditor(); };
    compressorEditButton.onClick = [this] { showCompressorEditor(); };
    flatButton(resetMidiLearnButton);
    resetMidiLearnButton.setTooltip("Apagar todos os endereçamentos MIDI Learn desta layer");
    resetMidiLearnButton.onClick = [this]
    {
        processor.resetMidiLearn(index);
        refresh();
    };
    addAndMakeVisible(resetMidiLearnButton);

    initialiseComboBoxes();
    refresh();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::initialiseComboBoxes()
{
    mode.addItem("POLI", 1);
    mode.addItem("MONO LEGATO", 2);
    mode.addItem("PORTAMENTO", 3);
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::showLayerEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new juce::AlertWindow(
        "EDITAR LAYER", "Ajuste os controles desta layer sem expandir o canal.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto valueOf = [this](const juce::String& id, float fallback)
    {
        if (auto* value = processor.parameters.getRawParameterValue(id)) return value->load();
        return fallback;
    };
    auto* knobs = new KnobEditorPanel({
        { "VOLUME", valueOf(prefix + "Gain", 80.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "CUTOFF", valueOf(prefix + "Cutoff", 100.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "REVERB", valueOf(prefix + "Reverb", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "COMP", valueOf(prefix + "Comp", 0.0f), 0.0f, 100.0f, 1.0f, 0 }
    }, 4);
    dialog->addCustomComponent(new Sf2EditorPanel(processor, index));
    dialog->addCustomComponent(knobs);
    dialog->addButton("FECHAR", 1, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, dialog, knobs, prefix](int)
        {
            if (safe == nullptr) return;
            const auto set = [safe, prefix](const juce::String& id, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("Gain", knobs->value(0));
            set("Cutoff", knobs->value(1));
            set("Reverb", knobs->value(2));
            set("Comp", knobs->value(3));
            safe->refresh();
        }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showReverbEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new juce::AlertWindow(
        "REVERB DA LAYER", "O knob REVERB controla a quantidade. Ajuste o carater da sala abaixo.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "TAMANHO", processor.parameters.getRawParameterValue(prefix + "ReverbSize")->load(), 0.0f, 100.0f, 1.0f, 0 },
        { "DAMPING", processor.parameters.getRawParameterValue(prefix + "ReverbDamping")->load(), 0.0f, 100.0f, 1.0f, 0 },
        { "LARGURA ESTEREO", processor.parameters.getRawParameterValue(prefix + "ReverbWidth")->load(), 0.0f, 100.0f, 1.0f, 0 }
    }, 3);
    dialog->addCustomComponent(knobs);
    dialog->addButton("APLICAR", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("CANCELAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, dialog, knobs, prefix](int result)
        {
            if (safe == nullptr || result != 1) return;
            const auto set = [safe, prefix](const juce::String& id, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("ReverbSize", juce::jlimit(0.0f, 100.0f, knobs->value(0)));
            set("ReverbDamping", juce::jlimit(0.0f, 100.0f, knobs->value(1)));
            set("ReverbWidth", juce::jlimit(0.0f, 100.0f, knobs->value(2)));
        }), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showCompressorEditor()
{
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new juce::AlertWindow(
        "COMPRESSOR DA LAYER", "O knob COMP controla a mistura. Ajuste a dinamica abaixo.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "THRESHOLD dB", processor.parameters.getRawParameterValue(prefix + "CompThreshold")->load(), -60.0f, 0.0f, 0.1f, 1 },
        { "RATIO", processor.parameters.getRawParameterValue(prefix + "CompRatio")->load(), 1.0f, 20.0f, 0.1f, 1 },
        { "ATTACK ms", processor.parameters.getRawParameterValue(prefix + "CompAttack")->load(), 0.1f, 100.0f, 0.1f, 1 },
        { "RELEASE ms", processor.parameters.getRawParameterValue(prefix + "CompRelease")->load(), 5.0f, 1000.0f, 1.0f, 0 },
        { "MAKEUP dB", processor.parameters.getRawParameterValue(prefix + "CompMakeup")->load(), 0.0f, 24.0f, 0.1f, 1 }
    }, 3);
    dialog->addCustomComponent(knobs);
    dialog->addButton("APLICAR", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("CANCELAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, dialog, knobs, prefix](int result)
        {
            if (safe == nullptr || result != 1) return;
            const auto set = [safe, prefix](const juce::String& id, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + id))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("CompThreshold", juce::jlimit(-60.0f, 0.0f, knobs->value(0)));
            set("CompRatio", juce::jlimit(1.0f, 20.0f, knobs->value(1)));
            set("CompAttack", juce::jlimit(0.1f, 100.0f, knobs->value(2)));
            set("CompRelease", juce::jlimit(5.0f, 1000.0f, knobs->value(3)));
            set("CompMakeup", juce::jlimit(0.0f, 24.0f, knobs->value(4)));
        }), true);
}
void ClassicPlayerAudioProcessorEditor::LayerStrip::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const auto drumLayer = processor.layerType(index) == ClassicPlayerAudioProcessor::LayerType::drumPads;
    g.setColour(juce::Colour(panel));
    if (drumLayer)
    {
        g.fillRoundedRectangle(bounds, 7.0f);
        g.setColour(juce::Colour(line));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 7.0f, 1.0f);
        return;
    }
    g.fillRoundedRectangle(bounds, 7.0f);
    g.setColour(juce::Colour(line));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 7.0f, 1.0f);
    g.setColour(juce::Colour(mutedText));
    g.setFont(9.0f);
    const auto volumeY = expanded ? 151 : juce::jmax(48, meter.getY() - 18);
    g.drawText("VOLUME", 8, volumeY, getWidth() - 16, 15, juce::Justification::centred);

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
    const auto type = processor.layerType(index);
    if (type == ClassicPlayerAudioProcessor::LayerType::drumPads)
    {
        drumPadPanel.setBounds(area.reduced(0, 2));
        return;
    }
    if (! expanded)
    {
        auto summaryRow = area.removeFromTop(28);
        sourceSummary.setBounds(summaryRow.removeFromLeft(summaryRow.getWidth() - 70).reduced(3, 1));
        editButton.setBounds(summaryRow.reduced(1, 1));
        area.removeFromTop(4);
        auto faderArea = area.reduced(8, 4);
        volumeLearn.setBounds(faderArea.removeFromBottom(24).withWidth(juce::jmin(86, faderArea.getWidth())));
        faderArea.removeFromBottom(6);
        meter.setBounds(faderArea.removeFromLeft(18).reduced(1, 2));
        const auto faderWidth = juce::jmin(82, juce::jmax(54, faderArea.getWidth() / 2));
        gain.setBounds(faderArea.removeFromLeft(faderWidth).reduced(4, 2));
        return;
    }
    editButton.setBounds(area.removeFromTop(28).removeFromRight(70).reduced(1, 1));
    area.removeFromTop(4);
    if (type == ClassicPlayerAudioProcessor::LayerType::sf2)
    {
        loadButton.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        categoryBox.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto libraryRow = area.removeFromTop(28);
        deleteLibraryButton.setBounds(libraryRow.removeFromRight(76).reduced(1, 0));
        libraryBox.setBounds(libraryRow);
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
        area.removeFromTop(4);
        presetBox.setBounds(area.removeFromTop(28));
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::vst)
    {
        externalInstrumentBox.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto externalRow = area.removeFromTop(28);
        externalInstrumentButton.setBounds(externalRow.removeFromLeft(externalRow.getWidth() / 2).reduced(1, 0));
        openExternalEditorButton.setBounds(externalRow.reduced(1, 0));
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
    {
        dx7Button.setBounds(area.removeFromTop(28));
        area.removeFromTop(4);
        auto bankRow = area.removeFromTop(28);
        deleteDx7LibraryButton.setBounds(bankRow.removeFromRight(86).reduced(1, 0));
        dx7LibraryBox.setBounds(bankRow);
        area.removeFromTop(4);
        fileLabel.setBounds(area.removeFromTop(27));
        area.removeFromTop(4);
        dx7PatchBox.setBounds(area.removeFromTop(28));
    }
    else
    {
        dx7Button.setBounds(area.removeFromTop(30));
        area.removeFromTop(5);
        fileLabel.setBounds(area.removeFromTop(28));
    }
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
                                 juce::Slider& slider, juce::TextButton* learn,
                                 juce::TextButton* edit)
    {
        auto cell = row.removeFromLeft(knobWidth).reduced(2, 0);
        auto titleRow = cell.removeFromTop(16);
        if (edit != nullptr)
        {
            edit->setBounds(titleRow.removeFromRight(28).reduced(1, 0));
            label.setBounds(titleRow);
        }
        else
        {
            label.setBounds(titleRow);
        }
        if (learn != nullptr) learn->setBounds(cell.removeFromBottom(20).reduced(1));
        slider.setBounds(cell);
    };
    placeKnob(knobs, cutoffLabel, cutoff, &cutoffLearn, nullptr);
    placeKnob(knobs, reverbLabel, reverb, &reverbLearn, &reverbEditButton);
    placeKnob(knobs, compressorLabel, compressor, &compressorLearn, &compressorEditButton);

    controls.removeFromTop(5);
    auto routingRow = controls.removeFromTop(22);
    resetMidiLearnButton.setBounds(routingRow.removeFromRight(76).reduced(1, 1));
    routingLabel.setBounds(routingRow);
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::chooseDx7()
{
    fileChooser = std::make_unique<juce::FileChooser>("Escolha um arquivo DX7 SysEx",
                                                      juce::File{}, "*.syx;*.SYX");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{}) return;
            dx7Button.setEnabled(false);
            fileLabel.setText("Importando DX7...", juce::dontSendNotification);
            juce::File importedFile;
            auto result = processor.importDx7Bank(file, importedFile);
            if (result.wasOk()) result = processor.loadDx7(index, importedFile);
            dx7Button.setEnabled(true);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Falha ao carregar DX7", result.getErrorMessage());
            refresh();
        });
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::updateSourceTypeVisibility()
{
    const auto type = processor.layerType(index);
    const auto isSf2 = type == ClassicPlayerAudioProcessor::LayerType::sf2;
    const auto isVst = type == ClassicPlayerAudioProcessor::LayerType::vst;
    const auto isDx7 = type == ClassicPlayerAudioProcessor::LayerType::dx7;
    const auto isAnalog = type == ClassicPlayerAudioProcessor::LayerType::analog;
    const auto isDrumPads = type == ClassicPlayerAudioProcessor::LayerType::drumPads;
    drumPadPanel.setVisible(isDrumPads);

    // Restore the shared layer controls on every non-drum refresh. Without
    // this explicit reset, switching from Drum Pads left the meter, knobs and
    // routing controls hidden in the next SF2/DX7/Analog layer.
    const std::initializer_list<juce::Component*> sharedControls {
        &fileLabel, &gain, &cutoff, &reverb, &compressor, &mode, &sustain,
        &midiChannel, &octave, &lowNote, &highNote, &velocityCurve, &midiDevice,
        &volumeLearn, &resetMidiLearnButton, &cutoffLearn, &reverbLearn,
        &compressorLearn, &reverbEditButton, &compressorEditButton, &meter,
        &cutoffLabel, &reverbLabel, &compressorLabel, &routingLabel
    };
    for (auto* control : sharedControls)
        control->setVisible(!isDrumPads);

    loadButton.setVisible(isSf2);
    categoryBox.setVisible(isSf2);
    libraryBox.setVisible(isSf2);
    deleteLibraryButton.setVisible(isSf2);
    presetBox.setVisible(isSf2);
    externalInstrumentBox.setVisible(isVst && processor.supportsExternalInstruments());
    externalInstrumentButton.setVisible(isVst && processor.supportsExternalInstruments());
    openExternalEditorButton.setVisible(isVst && processor.supportsExternalInstruments());
    dx7Button.setVisible(isDx7 || isAnalog);
    dx7LibraryBox.setVisible(isDx7);
    dx7PatchBox.setVisible(isDx7);
    deleteDx7LibraryButton.setVisible(isDx7);
    editButton.setVisible(! isDrumPads);
    sourceSummary.setText(fileLabel.getText().isNotEmpty()
                              ? fileLabel.getText()
                              : (isSf2 ? "SF2" : isDx7 ? "DX7" : isAnalog ? "CLASSIC KEYS ANALOG"
                                                   : isVst ? "VST" : "DRUM PADS"),
                          juce::dontSendNotification);
    if (isDrumPads)
    {
        const std::initializer_list<juce::Component*> controls {
            &loadButton, &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton,
            &openExternalEditorButton, &deleteLibraryButton, &categoryBox, &libraryBox,
            &presetBox, &externalInstrumentBox, &dx7LibraryBox, &dx7PatchBox, &fileLabel,
            &gain, &cutoff, &reverb, &compressor, &mode, &sustain, &midiChannel,
            &octave, &lowNote, &highNote, &velocityCurve, &midiDevice,
            &volumeLearn, &resetMidiLearnButton, &cutoffLearn, &reverbLearn,
            &compressorLearn, &reverbEditButton, &compressorEditButton, &meter
        };
        for (auto* control : controls)
            control->setVisible(false);
        resized();
        return;
    }
    if (! expanded)
    {
        const std::initializer_list<juce::Component*> detailedControls {
            &loadButton, &externalInstrumentButton, &dx7Button, &deleteDx7LibraryButton,
            &openExternalEditorButton, &deleteLibraryButton, &categoryBox, &libraryBox,
            &presetBox, &externalInstrumentBox, &dx7LibraryBox, &dx7PatchBox, &fileLabel,
            &cutoff, &reverb, &compressor, &mode, &sustain, &midiChannel, &octave,
            &lowNote, &highNote, &velocityCurve, &midiDevice, &volumeLearn,
            &resetMidiLearnButton, &cutoffLearn, &reverbLearn, &compressorLearn,
            &reverbEditButton, &compressorEditButton, &cutoffLabel, &reverbLabel,
            &compressorLabel, &routingLabel
        };
        for (auto* control : detailedControls)
            control->setVisible(false);
        sourceSummary.setVisible(true);
        gain.setVisible(true);
        meter.setVisible(true);
        resized();
        return;
    }
    sourceSummary.setVisible(false);
    fileLabel.setVisible(true);
    if (isAnalog)
        fileLabel.setText("CLASSIC KEYS ANALOG", juce::dontSendNotification);
    resized();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::closeExternalInstrumentEditor()
{
    // The plug-in owns this native editor. It must be gone before a saved
    // program can unload, replace, or restore the hosted instrument.
    externalEditorWindow.reset();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::openExternalInstrumentEditor()
{
    // Closing a hosted editor only hides its DocumentWindow. Reuse that same
    // window/editor on the next click: asking JUCE for another editor before
    // destroying the hidden window can hand out the existing editor pointer.
    if (externalEditorWindow != nullptr)
    {
        externalEditorWindow->setVisible(true);
        externalEditorWindow->toFront(true);
        return;
    }

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
    processor.unloadDx7(index);
    processor.setLayerType(index, ClassicPlayerAudioProcessor::LayerType::sf2);
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::refreshExternalInstrumentLibrary()
{
    rebuildExternalInstrumentLibrary();
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildDx7Library()
{
    const auto currentPath = processor.dx7Path(index);
    dx7LibraryFiles = processor.libraryDx7Banks();
    dx7LibraryBox.clear(juce::dontSendNotification);
    int selectedId = 0;
    for (int item = 0; item < dx7LibraryFiles.size(); ++item)
    {
        const auto& file = dx7LibraryFiles.getReference(item);
        dx7LibraryBox.addItem(file.getFileNameWithoutExtension(), item + 1);
        if (file.getFullPathName() == currentPath) selectedId = item + 1;
    }
    dx7LibraryBox.setTextWhenNothingSelected(dx7LibraryFiles.isEmpty()
        ? "BIBLIOTECA DX7 VAZIA" : "ESCOLHA O BANCO DX7");
    if (selectedId > 0) dx7LibraryBox.setSelectedId(selectedId, juce::dontSendNotification);
    deleteDx7LibraryButton.setEnabled(dx7LibraryBox.getSelectedItemIndex() >= 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::rebuildDx7Patches()
{
    dx7PatchBox.clear(juce::dontSendNotification);
    const auto count = processor.dx7PatchCount(index);
    for (int patch = 0; patch < count; ++patch)
        dx7PatchBox.addItem(juce::String(patch + 1) + ": " + processor.dx7PatchName(index, patch), patch + 1);
    if (count > 0)
        dx7PatchBox.setSelectedId(processor.dx7SelectedPatch(index) + 1, juce::dontSendNotification);
    dx7PatchBox.setEnabled(count > 0);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::deleteSelectedDx7Bank()
{
    const auto selected = dx7LibraryBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, dx7LibraryFiles.size())) return;
    const auto file = dx7LibraryFiles.getReference(selected);
    const juce::Component::SafePointer<LayerStrip> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
        "Excluir DX7", "Excluir '" + file.getFileName() + "' da biblioteca?",
        "Excluir", "Cancelar", this, juce::ModalCallbackFunction::create(
        [safe, file](int answer)
        {
            if (safe == nullptr || answer == 0) return;
            const auto result = safe->processor.deleteLibraryDx7Bank(file);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Falha ao excluir DX7", result.getErrorMessage());
            safe->refresh();
        }));
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::refresh()
{
    const auto type = processor.layerType(index);
    if (type == ClassicPlayerAudioProcessor::LayerType::drumPads)
    {
        updateSourceTypeVisibility();
        drumPadPanel.refresh();
        return;
    }
    const auto path = processor.soundFontPath(index);
    const auto externalName = processor.externalInstrumentName(index);
    const auto dx7Name = processor.dx7PatchName(index);
    dx7Button.setButtonText(type == ClassicPlayerAudioProcessor::LayerType::analog
                                 ? "ABRIR CLASSIC KEYS ANALOG" : "IMPORTAR DX7");
    if (type == ClassicPlayerAudioProcessor::LayerType::sf2)
    {
        if (path.isNotEmpty())
        {
            const auto parentCategory = juce::File(path).getParentDirectory().getFileName();
            const auto categories = ClassicPlayerAudioProcessor::soundFontCategories();
            const auto categoryIndex = categories.indexOf(parentCategory);
            if (categoryIndex >= 0) categoryBox.setSelectedId(categoryIndex + 1, juce::dontSendNotification);
        }
        rebuildLibrary();
        rebuildPresets();
    }
    else if (type == ClassicPlayerAudioProcessor::LayerType::vst)
        rebuildExternalInstrumentLibrary();
    else if (type == ClassicPlayerAudioProcessor::LayerType::dx7)
    {
        rebuildDx7Library();
        rebuildDx7Patches();
    }

    const auto hasSource = type == ClassicPlayerAudioProcessor::LayerType::sf2 ? path.isNotEmpty()
        : type == ClassicPlayerAudioProcessor::LayerType::vst ? externalName.isNotEmpty()
        : type == ClassicPlayerAudioProcessor::LayerType::dx7 ? processor.hasDx7(index)
        : processor.hasAnalogSynth(index);
    fileLabel.setText(type == ClassicPlayerAudioProcessor::LayerType::sf2
                        ? (path.isNotEmpty() ? juce::File(path).getFileName() : "Sem SoundFont")
                        : type == ClassicPlayerAudioProcessor::LayerType::vst
                            ? (externalName.isNotEmpty() ? externalName : "Sem VST")
                            : type == ClassicPlayerAudioProcessor::LayerType::dx7
                                ? (dx7Name.isNotEmpty() ? dx7Name : "Sem DX7")
                                : "CLASSIC KEYS ANALOG",
                      juce::dontSendNotification);
    fileLabel.setColour(juce::Label::backgroundColourId,
                        hasSource ? juce::Colour(yellow) : juce::Colour(0xff0b1218));
    fileLabel.setColour(juce::Label::textColourId,
                        hasSource ? juce::Colours::black : juce::Colour(mutedText));
    openExternalEditorButton.setEnabled(processor.supportsExternalInstruments()
                                        && processor.hasExternalInstrument(index));
    const auto config = processor.layerConfig(index);
    mode.setSelectedId(config.portamento ? 3 : (config.mono ? 2 : 1),
                       juce::dontSendNotification);
    sustain.setSelectedId(config.sustainEnabled ? 1 : 2, juce::dontSendNotification);
    midiChannel.setSelectedId(config.midiChannel + 1, juce::dontSendNotification);
    octave.setSelectedId(config.octave + 5, juce::dontSendNotification);
    lowNote.setSelectedId(config.lowNote + 1, juce::dontSendNotification);
    highNote.setSelectedId(config.highNote + 1, juce::dontSendNotification);
    velocityCurve.setSelectedId(config.velocityCurve + 1, juce::dontSendNotification);
    updateSourceTypeVisibility();
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::applyConfig()
{
    auto config = processor.layerConfig(index);
    config.mono = mode.getSelectedId() >= 2;
    config.portamento = mode.getSelectedId() == 3;
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
        const auto channel = processor.midiLearnChannel(index, target);
        const auto mappingText = cc < 0 ? juce::String("LEARN")
            : "CC " + juce::String(cc) + (channel > 0 ? " C" + juce::String(channel) : juce::String{});
        button->setButtonText(learning ? "MOVA O CC" : mappingText);
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
    chordLabel.setFont(juce::FontOptions(36.0f, juce::Font::bold));
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


    accidentalStyleBox.addItem("MISTO", 1);
    accidentalStyleBox.addItem("SUSTENIDO", 2);
    accidentalStyleBox.addItem("BEMOL", 3);
    accidentalStyleBox.setSelectedId(1, juce::dontSendNotification);
    accidentalStyleBox.setTooltip("Formato dos acidentes exibidos no visor de acordes");
    accidentalStyleBox.onChange = [this]
    {
        switch (accidentalStyleBox.getSelectedId())
        {
            case 2: accidentalStyle = ClassicChordDetector::AccidentalStyle::sharp; break;
            case 3: accidentalStyle = ClassicChordDetector::AccidentalStyle::flat; break;
            default: accidentalStyle = ClassicChordDetector::AccidentalStyle::mixed; break;
        }
        triggerAsyncUpdate();
    };
    addAndMakeVisible(accidentalStyleBox);

    programBox.setEditableText(true);
    programBox.setTextWhenNothingSelected("NOVO PROGRAMA");
    addAndMakeVisible(programBox);
    flatButton(saveProgramButton);
    flatButton(deleteProgramButton);
    flatButton(loadProgramButton);
    saveProgramButton.onClick = [this] { saveProgram(); };
    deleteProgramButton.onClick = [this] { deleteSelectedProgram(); };
    loadProgramButton.onClick = [this] { loadSelectedProgram(); };
    addAndMakeVisible(saveProgramButton);
    addAndMakeVisible(deleteProgramButton);
    addAndMakeVisible(loadProgramButton);
    refreshProgramLibrary();

    flatButton(addLayerButton);
    addLayerButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Layer SF2");
        menu.addItem(2, "Layer DX7 (.syx)");
        menu.addItem(3, "Classic Keys Analog");
        menu.addItem(4, "Layer Drum Pads (8)");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addLayerButton),
            [safeThis = juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor>(this)](int choice)
            {
                if (safeThis == nullptr || choice == 0) return;
                const auto type = choice == 1 ? ClassicPlayerAudioProcessor::LayerType::sf2
                                : choice == 2 ? ClassicPlayerAudioProcessor::LayerType::dx7
                                : choice == 3 ? ClassicPlayerAudioProcessor::LayerType::analog
                                              : ClassicPlayerAudioProcessor::LayerType::drumPads;
                safeThis->addLayer(type);
            });
    };
    addAndMakeVisible(addLayerButton);

    flatButton(recordingButton);
    recordingButton.setTooltip("Gravar a saída completa do Classic Player em WAV");
    recordingButton.onClick = [this]
    {
        if (classicProcessor.isAudioRecording())
        {
            classicProcessor.stopAudioRecording();
            recordingStatus.setText("WAV salvo na Area de Trabalho", juce::dontSendNotification);
            recordingButton.setButtonText("GRAVAR WAV");
            return;
        }

        const auto result = classicProcessor.startAudioRecording();
        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Não foi possível gravar", result.getErrorMessage());
            return;
        }
        recordingStartedAtMs = juce::Time::currentTimeMillis();
        recordingStatus.setText("GRAVANDO 00:00", juce::dontSendNotification);
        recordingButton.setButtonText("PARAR");
    };
    addAndMakeVisible(recordingButton);
    recordingStatus.setJustificationType(juce::Justification::centredLeft);
    recordingStatus.setColour(juce::Label::textColourId, juce::Colour(mutedText));
    recordingStatus.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    recordingStatus.setText("WAV: Area de Trabalho", juce::dontSendNotification);
    addAndMakeVisible(recordingStatus);

    flatButton(liveSetButton);
    liveSetButton.onClick = [this] { showLiveSet(!showingLiveSet); };
    addAndMakeVisible(liveSetButton);

    flatButton(editLiveSetButton);
    editLiveSetButton.onClick = [this]
    {
        editingLiveSet = !editingLiveSet;
        editLiveSetButton.setButtonText(editingLiveSet ? "CONCLUIR EDICAO" : "EDITAR LIVE SET");
        refreshLiveSet();
    };
    addAndMakeVisible(editLiveSetButton);

    for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
    {
        auto& button = liveSetBankButtons[(size_t) bank];
        flatButton(button);
        button.setButtonText("BANCO " + juce::String(bank + 1));
        button.onClick = [this, bank]
        {
            activeLiveSetBank = bank;
            activeLiveSetSlot = -1;
            refreshLiveSet();
        };
        addAndMakeVisible(button);
    }
    for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
    {
        auto& button = liveSetSlotButtons[(size_t) slot];
        flatButton(button);
        button.onClick = [this, slot]
        {
            if (editingLiveSet) chooseLiveSetSlot(slot);
            else loadLiveSetSlot(slot);
        };
        addAndMakeVisible(button);

        auto& learnButton = liveSetSlotLearnButtons[(size_t) slot];
        flatButton(learnButton);
        learnButton.setButtonText("LEARN CC");
        learnButton.setTooltip("Clique e mova um controle MIDI para carregar esta performance");
        learnButton.onClick = [this, slot]
        {
            classicProcessor.beginLiveSetSlotMidiLearn(activeLiveSetBank, slot);
            refreshLiveSet();
        };
        addAndMakeVisible(learnButton);
    }
    showLiveSet(false);

    flatButton(masterEqButton);
    masterEqButton.setTooltip("Abrir o equalizador paramétrico da saída master");
    masterEqButton.onClick = [this] { showMasterEqEditor(); };
    addAndMakeVisible(masterEqButton);

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
                                                          [this] { applyMixerStates(); layoutLayerStrips(); });
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::showAnalogSynthEditor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::analog) return;

    auto* dialog = new juce::AlertWindow(
        "Classic Keys Analog", "Edite a camada Analog sem alterar as outras camadas.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* controls = new AnalogSynthEditorPanel(processor.analogSynthConfig(index));
    dialog->addCustomComponent(controls);
    dialog->addButton("FECHAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    controls->onConfigChanged = [safe](const AnalogSynthEngine::Config& config)
    {
        if (safe != nullptr)
            safe->processor.setAnalogSynthConfig(safe->index, config);
    };
    controls->onPresetChanged = [safe](const AnalogSynthEngine::Config& config)
    {
        if (safe != nullptr)
        {
            safe->processor.resetAnalogSynthVoices(safe->index);
            safe->processor.setAnalogSynthConfig(safe->index, config);
        }
    };
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [](int) {}), true);
}

void ClassicPlayerAudioProcessorEditor::LayerStrip::showDx7Editor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::dx7) return;
    const auto prefix = "layer" + juce::String(index + 1);
    auto* dialog = new juce::AlertWindow(
        "DX7", "Selecione o banco e o timbre desta camada.", juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    dialog->addCustomComponent(new Dx7EditorPanel(processor, index));
    auto valueOf = [this, prefix](const juce::String& suffix, float fallback)
    {
        if (auto* value = processor.parameters.getRawParameterValue(prefix + suffix)) return value->load();
        return fallback;
    };
    auto* common = new KnobEditorPanel({
        { "VOLUME", valueOf("Gain", 80.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "CUTOFF", valueOf("Cutoff", 100.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "REVERB", valueOf("Reverb", 0.0f), 0.0f, 100.0f, 1.0f, 0 },
        { "COMP", valueOf("Comp", 0.0f), 0.0f, 100.0f, 1.0f, 0 }
    }, 4);
    dialog->addCustomComponent(common);
    dialog->addButton("FECHAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, common, prefix](int)
        {
            if (safe == nullptr) return;
            const auto set = [safe, prefix](const juce::String& suffix, float value)
            {
                if (auto* parameter = safe->processor.parameters.getParameter(prefix + suffix))
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            };
            set("Gain", common->value(0));
            set("Cutoff", common->value(1));
            set("Reverb", common->value(2));
            set("Comp", common->value(3));
            safe->refresh();
        }), true);
}

void ClassicPlayerAudioProcessorEditor::showMasterEqEditor()
{
    auto* dialog = new juce::AlertWindow(
        "EQ MASTER", "EQ de cinco estagios: corte baixo, tres bandas e corte alto.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* knobs = new KnobEditorPanel({
        { "LOW CUT Hz", classicProcessor.masterEqValue("masterEqLowCut"), 20.0f, 250.0f, 1.0f, 0 },
        { "LOW GAIN dB", classicProcessor.masterEqValue("masterEqLow"), -12.0f, 12.0f, 0.1f, 1 },
        { "LOW FREQ Hz", classicProcessor.masterEqValue("masterEqLowFrequency"), 40.0f, 400.0f, 1.0f, 0 },
        { "MID GAIN dB", classicProcessor.masterEqValue("masterEqMid"), -12.0f, 12.0f, 0.1f, 1 },
        { "MID FREQ Hz", classicProcessor.masterEqValue("masterEqFrequency"), 200.0f, 6000.0f, 1.0f, 0 },
        { "HIGH GAIN dB", classicProcessor.masterEqValue("masterEqHigh"), -12.0f, 12.0f, 0.1f, 1 },
        { "HIGH FREQ Hz", classicProcessor.masterEqValue("masterEqHighFrequency"), 2000.0f, 16000.0f, 1.0f, 0 },
        { "HIGH CUT Hz", classicProcessor.masterEqValue("masterEqHighCut"), 2000.0f, 20000.0f, 1.0f, 0 }
    }, 4);
    dialog->addCustomComponent(knobs);
    dialog->addButton("APLICAR", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("CANCELAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, dialog, knobs](int result)
        {
            if (safe == nullptr || result != 1) return;
            safe->classicProcessor.setMasterEqValue("masterEqLowCut", juce::jlimit(20.0f, 250.0f, knobs->value(0)));
            safe->classicProcessor.setMasterEqValue("masterEqLow", juce::jlimit(-12.0f, 12.0f, knobs->value(1)));
            safe->classicProcessor.setMasterEqValue("masterEqLowFrequency", juce::jlimit(40.0f, 400.0f, knobs->value(2)));
            safe->classicProcessor.setMasterEqValue("masterEqMid", juce::jlimit(-12.0f, 12.0f, knobs->value(3)));
            safe->classicProcessor.setMasterEqValue("masterEqFrequency", juce::jlimit(200.0f, 6000.0f, knobs->value(4)));
            safe->classicProcessor.setMasterEqValue("masterEqHigh", juce::jlimit(-12.0f, 12.0f, knobs->value(5)));
            safe->classicProcessor.setMasterEqValue("masterEqHighFrequency", juce::jlimit(2000.0f, 16000.0f, knobs->value(6)));
            safe->classicProcessor.setMasterEqValue("masterEqHighCut", juce::jlimit(2000.0f, 20000.0f, knobs->value(7)));
        }), true);
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
    const auto brandWidth = juce::jlimit(230, 330, getWidth() / 4);
    auto brand = header.removeFromLeft(brandWidth);
    brand.removeFromTop(16);
    title.setBounds(brand.removeFromTop(38));
    subtitle.setBounds(brand.removeFromTop(25));

    auto masterArea = header.removeFromRight(116);
    masterMeter.setBounds(masterArea.removeFromRight(13).reduced(0, 6));
    masterLabel.setBounds(masterArea.removeFromTop(17));
    auto masterKnobArea = masterArea.removeFromTop(57);
    master.setBounds(masterKnobArea.reduced(3, 0));
    masterEqButton.setBounds(masterArea.removeFromTop(20).reduced(1, 0));
    header.removeFromRight(12);

    auto addLayerArea = header.removeFromRight(100);
    addLayerButton.setBounds(addLayerArea.withSizeKeepingCentre(88, 32));
    header.removeFromRight(4);

    auto liveSetArea = header.removeFromRight(100);
    liveSetButton.setBounds(liveSetArea.withSizeKeepingCentre(88, 32));
    header.removeFromRight(4);

    auto accidentalArea = header.removeFromRight(126);
    accidentalStyleBox.setBounds(accidentalArea.withSizeKeepingCentre(118, 32));
    header.removeFromRight(4);

    auto chordArea = header.reduced(4, 1);
    auto colourControls = chordArea.removeFromRight(106).reduced(5, 2);
    chordColourButton.setBounds(colourControls.removeFromTop(30));
    colourControls.removeFromTop(4);
    keyColourButton.setBounds(colourControls.removeFromTop(30));

    auto programRow = chordArea.removeFromBottom(28);
    loadProgramButton.setBounds(programRow.removeFromRight(76).reduced(1, 0));
    deleteProgramButton.setBounds(programRow.removeFromRight(70).reduced(1, 0));
    saveProgramButton.setBounds(programRow.removeFromRight(62).reduced(1, 0));
    programBox.setBounds(programRow.reduced(1, 0));

    auto chordBox = chordArea.reduced(2, 0);
    chordCaption.setBounds({});
    chordLabel.setBounds(chordBox);

    area.removeFromTop(12);
    auto footer = area.removeFromBottom(54);
    auto recordingArea = footer.removeFromTop(27);
    recordingButton.setBounds(recordingArea.removeFromLeft(128).reduced(1, 0));
    recordingStatus.setBounds(recordingArea.removeFromLeft(210).reduced(6, 0));
    recordingButton.setVisible(!showingLiveSet);
    recordingStatus.setVisible(!showingLiveSet);

    if (showingLiveSet)
    {
        auto liveArea = area.reduced(4, 2);
        auto banks = liveArea.removeFromTop(38);
        const auto bankWidth = banks.getWidth() / ClassicPlayerAudioProcessor::liveSetBankCount;
        for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
            liveSetBankButtons[(size_t) bank].setBounds(
                banks.removeFromLeft(bank == ClassicPlayerAudioProcessor::liveSetBankCount - 1
                                     ? banks.getWidth() : bankWidth).reduced(2, 2));

        auto editArea = liveArea.removeFromBottom(46);
        editLiveSetButton.setBounds(editArea.removeFromRight(168).reduced(2, 4));
        const auto tileHeight = liveArea.getHeight() / 2;
        const auto tileWidth = liveArea.getWidth() / 4;
        for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
        {
            const auto column = slot % 4;
            const auto row = slot / 4;
            auto tile = juce::Rectangle<int>(
                liveArea.getX() + column * tileWidth + 3,
                liveArea.getY() + row * tileHeight + 3,
                tileWidth - 6, tileHeight - 6);
            auto learnArea = tile.removeFromBottom(25);
            liveSetSlotButtons[(size_t) slot].setBounds(tile);
            liveSetSlotLearnButtons[(size_t) slot].setBounds(learnArea.reduced(0, 2));
        }
    }
    else
    {
        auto keyboardArea = area.removeFromBottom(112);
        keyboard.setBounds(keyboardArea.reduced(0, 4));
        area.removeFromBottom(8);
        layerViewport.setBounds(area);
        layoutLayerStrips();
    }

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
    if (classicProcessor.consumeLiveSetSlotMidiLearnChanged())
    {
        classicProcessor.saveLiveSetSlotMidiLearnState();
        refreshLiveSet();
    }
    if (const auto requested = classicProcessor.consumeRequestedLiveSetSlot();
        juce::isPositiveAndBelow(requested,
                                 ClassicPlayerAudioProcessor::liveSetBankCount
                                 * ClassicPlayerAudioProcessor::liveSetSlotsPerBank))
    {
        activeLiveSetBank = requested / ClassicPlayerAudioProcessor::liveSetSlotsPerBank;
        activeLiveSetSlot = -1;
        if (!showingLiveSet) showLiveSet(true);
        loadLiveSetSlot(requested % ClassicPlayerAudioProcessor::liveSetSlotsPerBank);
    }
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

    if (classicProcessor.isAudioRecording())
    {
        const auto elapsed = juce::jmax<juce::int64>(0, juce::Time::currentTimeMillis() - recordingStartedAtMs) / 1000;
        recordingStatus.setText("GRAVANDO " + juce::String(elapsed / 60).paddedLeft('0', 2)
                                + ":" + juce::String(elapsed % 60).paddedLeft('0', 2),
                                juce::dontSendNotification);
        recordingButton.setButtonText("PARAR");
    }
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
    const auto chord = ClassicChordDetector::formatAccidentals(detectedChord(), accidentalStyle);
    const auto fontSize = chord.length() <= 5 ? 56.0f
                         : chord.length() <= 9 ? 46.0f
                         : chord.length() <= 13 ? 36.0f
                         : chord.length() <= 18 ? 28.0f : 21.0f;
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

void ClassicPlayerAudioProcessorEditor::refreshExternalInstrumentLibrary()
{
    if (!classicProcessor.supportsExternalInstruments()) return;

    classicProcessor.refreshExternalInstrumentLibrary();
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->refreshExternalInstrumentLibrary();
}

void ClassicPlayerAudioProcessorEditor::refreshProgramLibrary()
{
    programFiles = classicProcessor.savedPrograms();
    const auto currentName = programBox.getText();
    programBox.clear(juce::dontSendNotification);
    int selectedId = 0;
    for (int item = 0; item < programFiles.size(); ++item)
    {
        const auto name = programFiles.getReference(item).getFileNameWithoutExtension();
        programBox.addItem(name, item + 1);
        if (name == currentName) selectedId = item + 1;
    }
    programBox.setTextWhenNothingSelected("NOVO PROGRAMA");
    if (selectedId > 0)
        programBox.setSelectedId(selectedId, juce::dontSendNotification);
    else if (currentName.isNotEmpty())
        programBox.setText(currentName, juce::dontSendNotification);
}

void ClassicPlayerAudioProcessorEditor::saveProgram()
{
    juce::File savedFile;
    const auto result = classicProcessor.saveProgram(programBox.getText(), savedFile);
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Falha ao salvar programação", result.getErrorMessage());
        return;
    }

    programBox.setText(savedFile.getFileNameWithoutExtension(), juce::dontSendNotification);
    refreshProgramLibrary();
}

void ClassicPlayerAudioProcessorEditor::deleteSelectedProgram()
{
    const auto selected = programBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, programFiles.size()))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Excluir programacao",
                                               "Seleciona uma programacao salva na lista.");
        return;
    }

    const auto result = classicProcessor.deleteProgram(programFiles.getReference(selected));
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Falha ao excluir programacao", result.getErrorMessage());
        return;
    }

    programBox.clear(juce::dontSendNotification);
    refreshProgramLibrary();
    refreshLiveSet();
}

void ClassicPlayerAudioProcessorEditor::showLiveSet(bool show)
{
    showingLiveSet = show;
    liveSetButton.setButtonText(show ? "VOLTAR" : "LIVE SET");
    layerViewport.setVisible(!show);
    keyboard.setVisible(!show);
    programBox.setVisible(!show);
    saveProgramButton.setVisible(!show);
    deleteProgramButton.setVisible(!show);
    loadProgramButton.setVisible(!show);
    addLayerButton.setVisible(!show);

    editLiveSetButton.setVisible(show);
    for (auto& button : liveSetBankButtons) button.setVisible(show);
    for (auto& button : liveSetSlotButtons) button.setVisible(show);
    for (auto& button : liveSetSlotLearnButtons) button.setVisible(show);
    if (show) refreshLiveSet();
    resized();
}

void ClassicPlayerAudioProcessorEditor::refreshLiveSet()
{
    for (int bank = 0; bank < ClassicPlayerAudioProcessor::liveSetBankCount; ++bank)
    {
        auto& button = liveSetBankButtons[(size_t) bank];
        button.setToggleState(bank == activeLiveSetBank, juce::dontSendNotification);
        button.setColour(juce::TextButton::buttonColourId,
                         bank == activeLiveSetBank ? juce::Colour(teal) : juce::Colour(panelLight));
        button.setColour(juce::TextButton::buttonOnColourId,
                         bank == activeLiveSetBank ? juce::Colour(teal) : juce::Colour(teal));
        button.setColour(juce::TextButton::textColourOffId,
                         bank == activeLiveSetBank ? juce::Colour(background) : juce::Colour(text));
    }

    for (int slot = 0; slot < ClassicPlayerAudioProcessor::liveSetSlotsPerBank; ++slot)
    {
        auto& button = liveSetSlotButtons[(size_t) slot];
        const auto name = classicProcessor.liveSetSlotName(activeLiveSetBank, slot);
        const auto layers = classicProcessor.liveSetSlotLayerSummary(activeLiveSetBank, slot);
        button.setButtonText(juce::String(slot + 1).paddedLeft('0', 2)
                             + "\n" + (name.isNotEmpty() ? name : "SEM PERFORMANCE")
                             + (layers.isNotEmpty() ? "\n" + layers : juce::String{}));
        const auto active = slot == activeLiveSetSlot;
        button.setColour(juce::TextButton::buttonColourId,
                         active ? juce::Colour(yellow) : juce::Colour(panel));
        button.setColour(juce::TextButton::buttonOnColourId,
                         active ? juce::Colour(yellow) : juce::Colour(teal));
        button.setColour(juce::TextButton::textColourOffId,
                         active ? juce::Colour(background) : juce::Colour(text));
        button.setTooltip(editingLiveSet ? "Clique para atribuir uma programação salva"
                                         : (name.isEmpty() ? "Posição vazia" : "Carregar " + name));

        auto& learnButton = liveSetSlotLearnButtons[(size_t) slot];
        const auto cc = classicProcessor.liveSetSlotMidiLearnCC(activeLiveSetBank, slot);
        const auto learning = classicProcessor.isLiveSetSlotMidiLearning(activeLiveSetBank, slot);
        learnButton.setButtonText(learning ? "AGUARDANDO..." : (cc >= 0 ? "CC " + juce::String(cc) : "LEARN CC"));
        learnButton.setColour(juce::TextButton::buttonColourId,
                              learning ? juce::Colour(teal) : juce::Colour(panelLight));
        learnButton.setColour(juce::TextButton::textColourOffId,
                              learning ? juce::Colour(background) : juce::Colour(text));
        learnButton.setTooltip(learning ? "Mova agora um controle MIDI CC"
                                        : (cc >= 0 ? "CC " + juce::String(cc)
                                                   + " no canal "
                                                   + juce::String(classicProcessor.liveSetSlotMidiLearnChannel(activeLiveSetBank, slot))
                                                   + ". Clique para reaprender."
                                                   : "Clique e mova um controle MIDI para carregar esta performance"));
    }
}

void ClassicPlayerAudioProcessorEditor::chooseLiveSetSlot(int slot)
{
    refreshProgramLibrary();
    juce::PopupMenu menu;
    menu.addItem(1, "Limpar posição");
    menu.addSeparator();
    for (int item = 0; item < programFiles.size(); ++item)
        menu.addItem(item + 2, programFiles.getReference(item).getFileNameWithoutExtension());

    const juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor> safe(this);
    const auto bank = activeLiveSetBank;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(liveSetSlotButtons[(size_t) slot]),
        [safe, bank, slot](int selected)
        {
            if (safe == nullptr || selected == 0) return;
            if (selected == 1)
                safe->classicProcessor.clearLiveSetSlot(bank, slot);
            else
            {
                const auto item = selected - 2;
                if (juce::isPositiveAndBelow(item, safe->programFiles.size()))
                {
                    const auto result = safe->classicProcessor.assignLiveSetSlot(
                        bank, slot, safe->programFiles.getReference(item));
                    if (result.failed())
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::WarningIcon,
                            "Live Set", result.getErrorMessage());
                }
            }
            safe->refreshLiveSet();
        });
}

void ClassicPlayerAudioProcessorEditor::refreshAfterProgramLoad()
{
    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < displayedLayerCount);
        strips[(size_t) i]->refresh();
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::loadLiveSetSlot(int slot)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->closeExternalInstrumentEditor();

    const auto result = classicProcessor.loadLiveSetSlot(activeLiveSetBank, slot);
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Live Set", result.getErrorMessage());
        return;
    }
    activeLiveSetSlot = slot;
    refreshAfterProgramLoad();
    refreshLiveSet();
}

void ClassicPlayerAudioProcessorEditor::loadSelectedProgram()
{
    const auto selected = programBox.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(selected, programFiles.size()))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Carregar programação",
                                               "Selecione uma programação salva na lista.");
        return;
    }

    // Close every hosted native editor before the processor restores the
    // program. This prevents a VST editor from retaining pointers to an
    // instrument instance that may be replaced by the selected program.
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->closeExternalInstrumentEditor();

    const auto result = classicProcessor.loadProgram(programFiles.getReference(selected));
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Falha ao carregar programação", result.getErrorMessage());
        return;
    }

    displayedLayerCount = classicProcessor.activeLayerCount();
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < displayedLayerCount);
        strips[(size_t) i]->refresh();
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
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

void ClassicPlayerAudioProcessorEditor::addLayer(ClassicPlayerAudioProcessor::LayerType type)
{
    const auto newLayerIndex = classicProcessor.activeLayerCount();
    if (!classicProcessor.addLayer(type)) return;
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
    {
        if (strips[(size_t) i] != nullptr)
        {
            strips[(size_t) i]->setVisible(i < displayedLayerCount);
            if (i < displayedLayerCount)
                strips[(size_t) i]->refresh();
        }
    }
    addLayerButton.setEnabled(displayedLayerCount < Sf2Engine::layerCount);
    layoutLayerStrips();
    applyMixerStates();
}

void ClassicPlayerAudioProcessorEditor::layoutLayerStrips()
{
    const auto count = classicProcessor.activeLayerCount();
    if (count <= 0 || layerViewport.getWidth() <= 0) return;
    constexpr int gap = 8;
    // Keep each layer as a vertical strip.  Use four columns on a normal
    // desktop window, reducing responsively only when the window is narrow.
    const auto availableWidth = juce::jmax(1,
        layerViewport.getWidth() - layerViewport.getScrollBarThickness());
    const int columns = availableWidth >= 1000 ? 8
                       : availableWidth >= 720  ? 4
                       : availableWidth >= 450  ? 2
                                                  : 1;
    constexpr int expandedHeight = 590;
    const auto viewportHeight = layerViewport.getHeight()
        - layerViewport.getScrollBarThickness();
    // Mixer-style channels fill the complete area above the keyboard instead
    // of collapsing into short horizontal cards at the top.
    const int compactHeight = juce::jmax(148, viewportHeight - gap * 2);
    const auto stripWidth = juce::jmax(150,
        (layerViewport.getWidth() - gap * (columns - 1)) / columns);
    const auto contentWidth = juce::jmax(availableWidth,
        columns * stripWidth + gap * (columns - 1));
    const auto rows = (count + columns - 1) / columns;
    std::vector<int> rowHeights((size_t) rows, compactHeight);
    for (int i = 0; i < count; ++i)
        if (strips[(size_t) i] != nullptr && strips[(size_t) i]->isExpanded())
            rowHeights[(size_t) (i / columns)] = expandedHeight;
    int contentHeight = gap;
    for (const auto height : rowHeights) contentHeight += height + gap;
    contentHeight = juce::jmax(contentHeight,
        layerViewport.getHeight() - layerViewport.getScrollBarThickness());
    layerContent.setSize(contentWidth, contentHeight);
    int y = gap;
    for (int i = 0; i < Sf2Engine::layerCount; ++i)
    {
        if (strips[(size_t) i] == nullptr) continue;
        strips[(size_t) i]->setVisible(i < count);
        if (i >= count) continue;
        const auto row = i / columns;
        const auto column = i % columns;
        int rowY = gap;
        for (int r = 0; r < row; ++r) rowY += rowHeights[(size_t) r] + gap;
        strips[(size_t) i]->setBounds(column * (stripWidth + gap), rowY,
                                      stripWidth, rowHeights[(size_t) row]);
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
