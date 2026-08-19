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

    void resized() override
    {
        if (analogHardwareLayout && knobs.size() == 17)
        {
            // Positions mirror the five functional hardware sections drawn by
            // AnalogSynthEditorPanel. Every visible control remains a real JUCE knob.
            static constexpr std::array<std::array<float, 4>, 17> layout {{
                {{ 182,  55, 82, 102 }}, {{ 182, 160, 82, 102 }}, {{ 182, 265, 82, 102 }},
                {{ 280, 160, 82, 102 }}, {{ 280, 265, 82, 102 }}, {{ 440,  85, 82, 102 }},
                {{ 590,  80, 82, 102 }}, {{ 682,  80, 82, 102 }}, {{ 774,  80, 82, 102 }},
                {{ 590, 235, 82, 102 }}, {{ 682, 235, 82, 102 }}, {{ 774, 235, 82, 102 }},
                {{ 850, 235, 82, 102 }}, {{  40,  60, 82, 102 }}, {{  40, 175, 82, 102 }},
                {{  40, 290, 82, 102 }}, {{ 440, 270, 82, 102 }}
            }};

            const auto sx = (float) getWidth() / 960.0f;
            const auto sy = (float) getHeight() / 400.0f;
            for (int item = 0; item < knobs.size(); ++item)
            {
                const auto& p = layout[(size_t) item];
                auto cell = juce::Rectangle<int>(juce::roundToInt(p[0] * sx),
                                                 juce::roundToInt(p[1] * sy),
                                                 juce::roundToInt(p[2] * sx),
                                                 juce::roundToInt(p[3] * sy)).reduced(2, 1);
                labels[item]->setBounds(cell.removeFromTop(18));
                knobs[item]->setBounds(cell);
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
              { "GLIDE ms", source.glideMs, 0.0f, 1000.0f, 1.0f, 0 }
          }, 6)
    {
        configureWaveButton(wave1, 0);
        configureWaveButton(wave2, 1);
        configureWaveButton(wave3, 2);
        presetBox.addItem("INICIAL", 1);
        // The full MIT-licensed Minimoog factory list is represented here.
        // Values are adapted only to controls implemented by Classic Keys Analog.
        presetBox.addItem("SOLO LEAD", 2);
        presetBox.addItem("WARM PAD", 3);
        presetBox.addItem("ATMOSPHERIC PAD", 4);
        presetBox.addItem("SPACE MOD", 5);
        presetBox.addItem("MODERN LEAD", 6);
        presetBox.addItem("CRYSTAL PAD", 7);
        presetBox.addItem("EASY LEAD", 8);
        presetBox.addItem("SCREAMING LEAD", 9);
        presetBox.addItem("DREAM PAD", 10);
        presetBox.addItem("CLASSIC MINIMOOG LEAD", 11);
        presetBox.addItem("TAURUS BASS", 12);
        presetBox.addItem("ANALOG LEAD", 13);
        presetBox.addItem("UNISON LEAD", 14);
        presetBox.addItem("THICK BASS", 15);
        presetBox.addItem("FUNK BASS", 16);
        presetBox.addItem("EASY PAD", 17);
        presetBox.addItem("ANALOG BASS", 18);
        presetBox.addItem("VINTAGE LEAD", 19);
        presetBox.addItem("PERCUSSIVE BASS", 20);
        presetBox.addItem("LUCKY MAN", 21);
        presetBox.addItem("WATERY LEAD", 22);
        presetBox.addItem("VINTAGE MINIMOOG PAD", 23);
        presetBox.addItem("AUTHENTIC MINIMOOG BASS", 24);
        presetBox.addItem("PROGRESSIVE ROCK LEAD", 25);
        presetBox.addItem("SPACE ECHO", 26);
        presetBox.addItem("CATHEDRAL PAD", 27);
        presetBox.addItem("DUB BASS", 28);
        presetBox.addItem("ALIEN LANDSCAPE", 29);
        presetBox.addItem("GLASS HARMONICA", 30);
        presetBox.addItem("WIND CHIMES", 31);
        presetBox.addItem("SUBMARINE SONAR", 32);
        presetBox.addItem("CRYSTAL BELLS", 33);
        presetBox.addItem("THUNDER STORM", 34);
        presetBox.addItem("DIGITAL RAIN", 35);
        presetBox.addItem("COSMIC DRONE", 36);
        presetBox.setSelectedId(1, juce::dontSendNotification);
        presetBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(panelLight));
        presetBox.setColour(juce::ComboBox::textColourId, juce::Colour(text));
        presetBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(line));
        presetBox.onChange = [this] { applyFactoryPreset(presetBox.getSelectedId()); };
        addAndMakeVisible(presetBox);
        addAndMakeVisible(wave1);
        addAndMakeVisible(wave2);
        addAndMakeVisible(wave3);
        knobs.useAnalogHardwareLayout(true);
        addAndMakeVisible(knobs);
        setSize(1120, 650);
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
        result.lfoToPitch = knobs.value(14); result.lfoToFilter = knobs.value(15); result.glideMs = knobs.value(16);
        return result;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff171514));
        const auto wood = juce::Colour(0xff70452d), woodLight = juce::Colour(0xffb77a50);
        g.setColour(wood); g.fillRect(0, 0, getWidth(), 46); g.fillRect(0, getHeight() - 38, getWidth(), 38);
        g.setColour(woodLight); g.drawLine(0.0f, 4.0f, (float) getWidth(), 4.0f, 2.0f);
        g.drawLine(0.0f, (float) getHeight() - 5.0f, (float) getWidth(), (float) getHeight() - 5.0f, 2.0f);
        g.setColour(juce::Colour(0xff292727)); g.fillRect(18, 48, getWidth() - 36, getHeight() - 88);
        g.setColour(juce::Colour(0xff6e6b68)); g.drawRect(18, 48, getWidth() - 36, getHeight() - 88, 2);

        const auto icon = embeddedImage("classicplayerappicon_png");
        if (icon.isValid()) g.drawImageWithin(icon, 26, 4, 38, 38, juce::RectanglePlacement::centred);
        g.setColour(juce::Colour(teal)); g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText("CLASSIC KEYS ANALOG", 76, 10, 340, 25, juce::Justification::left);
        g.setColour(juce::Colour(text)); g.setFont(juce::FontOptions(10.0f));
        g.drawText("THREE OSCILLATOR SYNTHESIZER", 77, 31, 340, 13, juce::Justification::left);

        const int left = 20, top = 78, width = getWidth() - 40, height = getHeight() - 128;
        const std::array<juce::String, 5> titles {{ "CONTROLLERS", "OSCILLATOR BANK", "MIXER", "MODIFIERS", "OUTPUT" }};
        const std::array<float, 5> edges {{ 0.15f, 0.42f, 0.57f, 0.82f, 1.0f }};
        int x = left;
        for (int group = 0; group < 5; ++group)
        {
            const int right = left + juce::roundToInt(width * edges[(size_t) group]);
            g.setColour(juce::Colour(0xff202020)); g.fillRect(x, top, right - x, height);
            g.setColour(juce::Colour(0xff55514d)); g.drawRect(x, top, right - x, height, 1);
            g.setColour(juce::Colour(0xffd2d0ca)); g.setFont(juce::FontOptions(14.0f));
            g.drawText(titles[(size_t) group], x + 3, top + height - 30, right - x - 6, 24, juce::Justification::centred);
            x = right;
        }

        g.setColour(juce::Colour(0xffd4d0ca)); g.setFont(juce::FontOptions(10.0f));
        g.drawText("Classic Keys Analog - each control changes this layer in real time.",
                   left + 8, top + 7, width - 16, 16, juce::Justification::centred);
    }

    void resized() override
    {
        presetBox.setBounds(24, 51, 250, 25);
        const auto waveY = 105;
        wave1.setBounds(190, waveY, 148, 26); wave2.setBounds(345, waveY, 148, 26); wave3.setBounds(500, waveY, 148, 26);
        knobs.setBounds(22, 108, getWidth() - 44, getHeight() - 158);
    }

private:
    void applyFactoryPreset(int preset)
    {
        if (preset <= 1) { setFromConfig(sourceAtOpen); return; }

        auto value = sourceAtOpen;
        const auto name = presetBox.getText().toUpperCase();
        const bool isPad = name.contains("PAD") || name.contains("SPACE") || name.contains("COSMIC") || name.contains("RAIN")
                        || name.contains("BELLS") || name.contains("HARMONICA") || name.contains("CHIMES")
                        || name.contains("STORM") || name.contains("WIND");
        const bool isBass = name.contains("BASS") || name.contains("TAURUS") || name.contains("SUBMARINE");
        const bool isLead = name.contains("LEAD") || name.contains("SCREAMING") || name.contains("LUCKY");

        // The reference Minimoog is a one-voice instrument. In Classic Keys
        // the factory library deliberately keeps melodic Lead and Bass patches
        // monophonic, while Pad and Experimental patches remain polyphonic.
        value.monophonic = isLead || isBass;

        value.oscillator1Wave = AnalogSynthEngine::Waveform::saw;
        value.oscillator2Wave = AnalogSynthEngine::Waveform::saw;
        value.oscillator3Wave = AnalogSynthEngine::Waveform::triangle;
        value.oscillator1Level = 0.82f; value.oscillator2Level = 0.60f; value.oscillator3Level = 0.32f;
        value.oscillator2Semitones = 0.0f; value.oscillator3Semitones = -12.0f;
        value.noiseLevel = 0.0f;
        value.lfoRateHz = 1.6f; value.lfoToPitch = 0.10f; value.lfoToFilter = 6.0f;
        value.cutoff = 51.0f; value.resonance = 0.34f; value.filterEnvelopeAmount = 0.62f;
        value.ampAttackMs = 90.0f; value.ampDecayMs = 580.0f; value.ampSustain = 0.80f;
        value.ampReleaseMs = 560.0f; value.filterAttackMs = 90.0f; value.filterDecayMs = 580.0f;
        value.filterSustain = 0.65f; value.filterReleaseMs = 560.0f; value.glideMs = 0.0f;

        // Direct parameter adaptations for the published factory presets. The
        // values map their oscillator, filter and envelope intent into the
        // Classic Keys Analog engine rather than generating variants by name.
        if (name == "SOLO LEAD")
        {
            value.oscillator1Level = 1.0f; value.oscillator2Level = 0.70f; value.oscillator3Level = 0.50f;
            value.cutoff = 12.0f; value.resonance = 0.75f; value.filterEnvelopeAmount = 0.92f;
            value.ampAttackMs = 50.0f; value.ampDecayMs = 550.0f; value.ampSustain = 0.85f;
            value.ampReleaseMs = 420.0f; value.filterAttackMs = 250.0f; value.filterDecayMs = 450.0f;
            value.filterSustain = 0.75f; value.filterReleaseMs = 450.0f;
            value.lfoRateHz = 4.2f; value.lfoToPitch = 0.80f; value.lfoToFilter = 25.0f; value.glideMs = 150.0f;
        }
        else if (name == "MODERN LEAD")
        {
            value.oscillator2Wave = AnalogSynthEngine::Waveform::pulse; value.oscillator3Wave = AnalogSynthEngine::Waveform::saw;
            value.oscillator1Level = 1.0f; value.oscillator2Level = 0.80f; value.oscillator3Level = 0.60f;
            value.oscillator3Semitones = -7.0f; value.noiseLevel = 0.10f;
            value.cutoff = 25.0f; value.resonance = 0.85f; value.filterEnvelopeAmount = 0.98f;
            value.ampAttackMs = 1.0f; value.ampDecayMs = 100.0f; value.ampSustain = 0.50f;
            value.ampReleaseMs = 320.0f; value.lfoRateHz = 3.2f; value.lfoToPitch = 0.80f;
            value.lfoToFilter = 45.0f; value.glideMs = 120.0f;
        }
        else if (name == "WARM PAD")
        {
            value.monophonic = false; value.oscillator1Wave = AnalogSynthEngine::Waveform::pulse;
            value.oscillator2Wave = AnalogSynthEngine::Waveform::pulse; value.oscillator3Wave = AnalogSynthEngine::Waveform::triangle;
            value.oscillator1Level = 0.80f; value.oscillator2Level = 0.60f; value.oscillator3Level = 0.40f;
            value.oscillator2Semitones = -7.0f; value.oscillator3Semitones = -12.0f;
            value.cutoff = 28.0f; value.resonance = 0.25f; value.filterEnvelopeAmount = 0.55f;
            value.ampAttackMs = 350.0f; value.ampDecayMs = 850.0f; value.ampSustain = 0.90f;
            value.ampReleaseMs = 1250.0f; value.lfoRateHz = 0.6f; value.lfoToFilter = 20.0f; value.glideMs = 0.0f;
        }
        else if (name == "ATMOSPHERIC PAD")
        {
            value.monophonic = false; value.oscillator1Level = 0.50f; value.oscillator2Level = 0.50f; value.oscillator3Level = 0.30f;
            value.oscillator2Wave = AnalogSynthEngine::Waveform::triangle; value.oscillator3Wave = AnalogSynthEngine::Waveform::saw;
            value.oscillator2Semitones = 5.0f; value.oscillator3Semitones = -5.0f; value.noiseLevel = 0.20f;
            value.cutoff = 35.0f; value.resonance = 0.40f; value.filterEnvelopeAmount = 0.60f;
            value.ampAttackMs = 400.0f; value.ampDecayMs = 800.0f; value.ampSustain = 0.90f;
            value.ampReleaseMs = 1500.0f; value.lfoRateHz = 0.3f; value.lfoToFilter = 15.0f; value.glideMs = 0.0f;
        }
        else if (isBass)
        {
            value.oscillator1Level = 1.0f; value.oscillator2Level = 0.68f; value.oscillator3Level = 0.20f;
            value.oscillator3Semitones = -12.0f; value.cutoff = 34.0f; value.resonance = 0.42f;
            value.filterEnvelopeAmount = 0.74f; value.ampAttackMs = 8.0f; value.ampDecayMs = 340.0f;
            value.ampSustain = 0.76f; value.ampReleaseMs = 280.0f; value.lfoRateHz = 1.0f; value.glideMs = 45.0f;
        }
        else if (isLead)
        {
            value.oscillator1Level = 0.95f; value.oscillator2Level = 0.70f; value.oscillator3Level = 0.45f;
            value.cutoff = 46.0f; value.resonance = 0.62f; value.filterEnvelopeAmount = 0.82f;
            value.ampAttackMs = 18.0f; value.ampDecayMs = 450.0f; value.ampSustain = 0.78f;
            value.ampReleaseMs = 360.0f; value.lfoRateHz = 3.0f; value.lfoToPitch = 0.35f;
            value.lfoToFilter = 18.0f; value.glideMs = 95.0f;
        }
        else if (isPad)
        {
            value.monophonic = false; value.cutoff = 46.0f; value.resonance = 0.24f; value.filterEnvelopeAmount = 0.52f;
            value.ampAttackMs = 340.0f; value.ampDecayMs = 860.0f; value.ampSustain = 0.88f;
            value.ampReleaseMs = 1200.0f; value.lfoRateHz = 0.6f; value.lfoToFilter = 12.0f; value.glideMs = 0.0f;
        }
        else
        {
            value.monophonic = false;
        }

        setFromConfig(value);
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
        knobs.setValue(14, value.lfoToPitch); knobs.setValue(15, value.lfoToFilter); knobs.setValue(16, value.glideMs);
    }

    static juce::String waveformName(AnalogSynthEngine::Waveform waveform)
    {
        switch (waveform) { case AnalogSynthEngine::Waveform::triangle: return "TRIANGLE"; case AnalogSynthEngine::Waveform::saw: return "SAW"; case AnalogSynthEngine::Waveform::square: return "SQUARE"; case AnalogSynthEngine::Waveform::pulse: return "PULSE"; }
        return "SAW";
    }

    void configureWaveButton(juce::TextButton& button, int oscillator)
    {
        flatButton(button); button.setClickingTogglesState(false); auto* buttonPtr = &button;
        button.onClick = [this, oscillator, buttonPtr]
        {
            auto value = static_cast<int>(waves[(size_t) oscillator]);
            waves[(size_t) oscillator] = static_cast<AnalogSynthEngine::Waveform>((value + 1) % 4);
            buttonPtr->setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
        };
        button.setButtonText("OSC " + juce::String(oscillator + 1) + " - " + waveformName(waves[(size_t) oscillator]));
    }

    AnalogSynthEngine::Config initial, sourceAtOpen;
    std::array<AnalogSynthEngine::Waveform, 3> waves;
    juce::ComboBox presetBox;
    juce::TextButton wave1, wave2, wave3;
    KnobEditorPanel knobs;
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

ClassicPlayerAudioProcessorEditor::LayerStrip::LayerStrip(
    ClassicPlayerAudioProcessor& p, int layerIndex, std::function<void()> mixChanged)
    : processor(p), index(layerIndex), mixStateChanged(std::move(mixChanged))
{
    layerTitle.setText("LAYER " + juce::String(index + 1), juce::dontSendNotification);
    layerTitle.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    layerTitle.setColour(juce::Label::textColourId, juce::Colour(text));
    addAndMakeVisible(layerTitle);

    for (auto* button : { &muteButton, &soloButton, &resetButton, &removeButton, &loadButton,
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
    const auto type = processor.layerType(index);
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
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addLayerButton),
            [safeThis = juce::Component::SafePointer<ClassicPlayerAudioProcessorEditor>(this)](int choice)
            {
                if (safeThis == nullptr || choice == 0) return;
                const auto type = choice == 1 ? ClassicPlayerAudioProcessor::LayerType::sf2
                                : choice == 2 ? ClassicPlayerAudioProcessor::LayerType::dx7
                                              : ClassicPlayerAudioProcessor::LayerType::analog;
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

void ClassicPlayerAudioProcessorEditor::LayerStrip::showAnalogSynthEditor()
{
    if (processor.layerType(index) != ClassicPlayerAudioProcessor::LayerType::analog) return;

    auto* dialog = new juce::AlertWindow(
        "Classic Keys Analog", "Edite a camada Analog sem alterar as outras camadas.",
        juce::MessageBoxIconType::NoIcon);
    dialog->setLookAndFeel(&classicLookAndFeel);
    auto* controls = new AnalogSynthEditorPanel(processor.analogSynthConfig(index));
    dialog->addCustomComponent(controls);
    dialog->addButton("APLICAR", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("CANCELAR", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<LayerStrip> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, controls](int result)
        {
            if (safe != nullptr && result == 1)
                safe->processor.setAnalogSynthConfig(safe->index, controls->config());
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
