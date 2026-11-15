#pragma once
#include "PluginProcessor.h"
#include <array>
#include <functional>
#include <vector>

// Six-operator voice editor for the DX7 layer. It edits the unpacked 156-byte
// voice of the selected patch (operators are stored OP6 first, 21 bytes each),
// so every control maps to a real field of the voice the engine plays.
namespace dx7ui
{
inline juce::Colour operatorColour(int opNumber)
{
    static const juce::uint32 colours[] { 0xff4ae05a, 0xff3aa0e0, 0xff6a78f0, 0xff9a62e8, 0xffe8884a, 0xffe0584a };
    return juce::Colour(colours[juce::jlimit(1, 6, opNumber) - 1]);
}
constexpr juce::uint32 teal = 0xff2fd0a8, ink = 0xff12171a, card = 0xff1b2226, line = 0xff2c383d, light = 0xffcfd8dc;

// Teal ring knob.
class KnobLook final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider&) override
    {
        const auto b = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height);
        const auto c = b.getCentre();
        const float r = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f - 3.0f;
        g.setColour(juce::Colour(0xff0b0f11));
        g.fillEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
        juce::Path track, value;
        track.addCentredArc(c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(line));
        g.strokePath(track, juce::PathStrokeType(3.0f));
        const float angle = startAngle + pos * (endAngle - startAngle);
        value.addCentredArc(c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, startAngle, angle, true);
        g.setColour(juce::Colour(teal));
        g.strokePath(value, juce::PathStrokeType(3.0f));
        const juce::Point<float> d(std::sin(angle), -std::cos(angle));
        g.setColour(juce::Colour(light));
        g.drawLine({ c + d * (r * 0.2f), c + d * (r * 0.62f) }, 2.0f);
    }
};

// Rate/level envelope with draggable points (used for operators and pitch EG).
class EnvelopeGraph final : public juce::Component
{
public:
    std::function<int(int)> get;       // 0..3 rates, 4..7 levels
    std::function<void(int, int)> set;
    juce::Colour colour { teal };

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff0b1013)); g.fillRoundedRectangle(b, 4.0f);
        g.setColour(juce::Colour(line)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
        if (!get) return;
        g.setColour(juce::Colour(line).withAlpha(0.6f));
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine(juce::roundToInt(b.getY() + b.getHeight() * (float) i / 4.0f), b.getX() + 2.0f, b.getRight() - 2.0f);
        juce::Path p;
        const auto pts = points();
        p.startNewSubPath(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i)
        {
            // Sustain: after the third point the line holds until key-off.
            if (i == 4) { p.lineTo(pts[3] + juce::Point<float>(holdWidth(), 0.0f)); p.lineTo(pts[4]); }
            else p.lineTo(pts[i]);
        }
        g.setColour(colour.withAlpha(0.18f));
        juce::Path fill(p); fill.lineTo(pts[4].x, b.getBottom() - pad); fill.lineTo(pts[0].x, b.getBottom() - pad); fill.closeSubPath();
        g.fillPath(fill);
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.8f));
        for (size_t i = 0; i < pts.size(); ++i)
        {
            g.setColour(i == (size_t) dragged ? juce::Colours::white : colour);
            g.fillEllipse(pts[i].x - 4.5f, pts[i].y - 4.5f, 9.0f, 9.0f);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!get) return;
        const auto pts = points();
        float best = 14.0f; dragged = -1;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const float d = pts[i].getDistanceFrom(e.position);
            if (d < best) { best = d; dragged = (int) i; }
        }
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!set || dragged < 0) return;
        const float plotH = (float) getHeight() - 2.0f * pad;
        const int level = juce::jlimit(0, 99, juce::roundToInt((((float) getHeight() - pad) - e.position.y) / plotH * 99.0f));
        // Point 0 and point 4 both show level 4; points 1..3 show levels 1..3.
        const int levelIndex = (dragged == 0 || dragged == 4) ? 7 : 3 + dragged;
        set(levelIndex, level);
        if (dragged >= 1)
        {
            // Time axis: each segment width is derived from its rate.
            float startVirtual = 0.0f;
            for (int i = 1; i < dragged; ++i) startVirtual += segmentWidth(get(i - 1));
            const float xVirtual = (e.position.x - pad) / scale();
            const float width = juce::jlimit(8.0f, 8.0f + 99.0f * 0.9f, xVirtual - startVirtual);
            set(dragged - 1, juce::jlimit(0, 99, juce::roundToInt(99.0f - (width - 8.0f) / 0.9f)));
        }
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override { dragged = -1; repaint(); }

private:
    static constexpr float pad = 10.0f;
    static float segmentWidth(int rate) { return 8.0f + (99.0f - (float) rate) * 0.9f; }
    static float holdWidth() { return 30.0f; }
    float scale() const { return ((float) getWidth() - 2.0f * pad) / (4.0f * (8.0f + 99.0f * 0.9f) + 30.0f); }
    std::array<juce::Point<float>, 5> points() const
    {
        std::array<juce::Point<float>, 5> pts;
        const float plotH = (float) getHeight() - 2.0f * pad, s = scale();
        const auto yOf = [&](int level) { return ((float) getHeight() - pad) - (float) level / 99.0f * plotH; };
        float x = 0.0f;
        pts[0] = { pad, yOf(get(7)) };
        for (int i = 1; i <= 3; ++i)
        {
            x += segmentWidth(get(i - 1));
            pts[(size_t) i] = { pad + x * s, yOf(get(3 + i)) };
        }
        x += 30.0f + segmentWidth(get(3));
        pts[4] = { pad + x * s, yOf(get(7)) };
        return pts;
    }
    int dragged = -1;
};

// Algorithm routing, drawn from the same table the FM core renders with.
class AlgorithmView final : public juce::Component
{
public:
    void setAlgorithm(int newAlgorithm)
    {
        newAlgorithm = juce::jlimit(0, 31, newAlgorithm);
        if (newAlgorithm != algorithm) { algorithm = newAlgorithm; repaint(); }
    }
    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff0b1013)); g.fillRoundedRectangle(b, 4.0f);
        g.setColour(juce::Colour(line)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
        std::array<std::vector<int>, 6> mods;
        std::array<bool, 6> carrier {}, feedback {};
        std::array<std::vector<int>, 3> writers;
        for (int idx = 0; idx < 6; ++idx)
        {
            const int flags = FmCore::opFlags(algorithm, idx);
            const int inbus = (flags >> 4) & 3, outbus = flags & 3;
            const bool add = (flags & 4) != 0;
            if (inbus != 0) mods[(size_t) idx] = writers[(size_t) inbus];
            if (outbus == 0) carrier[(size_t) idx] = true;
            else
            {
                if (!add) writers[(size_t) outbus].assign(1, idx);
                else writers[(size_t) outbus].push_back(idx);
            }
            if ((flags & 0xc0) == 0xc0) feedback[(size_t) idx] = true;
        }
        std::array<int, 6> level {};
        for (int idx = 5; idx >= 0; --idx)
        {
            int lv = 0;
            for (int target = idx + 1; target < 6; ++target)
                for (int m : mods[(size_t) target]) if (m == idx) lv = juce::jmax(lv, level[(size_t) target] + 1);
            level[(size_t) idx] = lv;
        }
        int maxLevel = 0;
        for (int lv : level) maxLevel = juce::jmax(maxLevel, lv);
        std::array<juce::Point<float>, 6> centre;
        const float boxW = 24.0f, boxH = 18.0f;
        const float top = b.getY() + 10.0f + boxH * 0.5f, bottom = b.getBottom() - 24.0f - boxH * 0.5f;
        for (int lv = 0; lv <= maxLevel; ++lv)
        {
            std::vector<int> row;
            for (int idx = 5; idx >= 0; --idx) if (level[(size_t) idx] == lv) row.push_back(idx);
            const float y = maxLevel == 0 ? bottom : bottom - (bottom - top) * (float) lv / (float) maxLevel;
            for (size_t k = 0; k < row.size(); ++k)
                centre[(size_t) row[k]] = { b.getX() + b.getWidth() * ((float) k + 1.0f) / ((float) row.size() + 1.0f), y };
        }
        g.setColour(juce::Colour(teal).withAlpha(0.8f));
        for (int idx = 0; idx < 6; ++idx)
            for (int m : mods[(size_t) idx])
                g.drawLine({ centre[(size_t) m].x, centre[(size_t) m].y + boxH * 0.5f, centre[(size_t) idx].x, centre[(size_t) idx].y - boxH * 0.5f }, 1.4f);
        g.setColour(juce::Colour(light).withAlpha(0.8f));
        const float outY = b.getBottom() - 14.0f;
        g.drawHorizontalLine(juce::roundToInt(outY), b.getX() + 8.0f, b.getRight() - 8.0f);
        g.setFont(juce::FontOptions(8.5f, juce::Font::bold));
        g.drawText("OUT", juce::Rectangle<float>(b.getX(), outY + 1.0f, b.getWidth(), 11.0f), juce::Justification::centred);
        for (int idx = 0; idx < 6; ++idx)
        {
            const int opNumber = 6 - idx;
            if (carrier[(size_t) idx])
                g.drawLine({ centre[(size_t) idx].x, centre[(size_t) idx].y + boxH * 0.5f, centre[(size_t) idx].x, outY }, 1.4f);
            const auto box = juce::Rectangle<float>(boxW, boxH).withCentre(centre[(size_t) idx]);
            g.setColour(juce::Colour(ink)); g.fillRoundedRectangle(box, 3.0f);
            g.setColour(operatorColour(opNumber)); g.drawRoundedRectangle(box, 3.0f, 1.6f);
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText(juce::String(opNumber), box, juce::Justification::centred);
            if (feedback[(size_t) idx])
            {
                juce::Path loop;
                loop.addCentredArc(box.getRight() + 1.0f, box.getY() - 1.0f, 6.0f, 6.0f, 0.0f, 0.0f, juce::MathConstants<float>::pi * 1.5f, true);
                g.setColour(juce::Colour(light).withAlpha(0.8f));
                g.strokePath(loop, juce::PathStrokeType(1.2f));
            }
        }
    }
private:
    int algorithm = 0;
};
} // namespace dx7ui

class Dx7VoiceEditorPanel final : public juce::Component, private juce::Timer
{
public:
    Dx7VoiceEditorPanel(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        using namespace dx7ui;
        for (int i = 0; i < 2; ++i)
        {
            pageButtons[(size_t) i].setButtonText(i == 0 ? "OVERVIEW" : "MODS");
            pageButtons[(size_t) i].setClickingTogglesState(true);
            pageButtons[(size_t) i].setRadioGroupId(8401);
            pageButtons[(size_t) i].setConnectedEdges(i == 0 ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
            pageButtons[(size_t) i].onClick = [this, i] { page = i; updateVisibility(); resized(); repaint(); };
            addAndMakeVisible(pageButtons[(size_t) i]);
        }
        pageButtons[0].setToggleState(true, juce::dontSendNotification);

        for (int i = 0; i < 6; ++i)
        {
            const int opNumber = 6 - i;
            opButtons[(size_t) i].setButtonText("OP" + juce::String(opNumber));
            opButtons[(size_t) i].setClickingTogglesState(true);
            opButtons[(size_t) i].setRadioGroupId(8402);
            opButtons[(size_t) i].setColour(juce::TextButton::buttonColourId, juce::Colour(card));
            opButtons[(size_t) i].setColour(juce::TextButton::buttonOnColourId, operatorColour(opNumber));
            opButtons[(size_t) i].setColour(juce::TextButton::textColourOffId, operatorColour(opNumber));
            opButtons[(size_t) i].setColour(juce::TextButton::textColourOnId, juce::Colour(ink));
            opButtons[(size_t) i].onClick = [this, opNumber] { selectedOp = opNumber; updateWidgets(); repaint(); };
            addAndMakeVisible(opButtons[(size_t) i]);
            ratioLabels[(size_t) i].setJustificationType(juce::Justification::centred);
            ratioLabels[(size_t) i].setFont(juce::FontOptions(10.0f));
            ratioLabels[(size_t) i].setColour(juce::Label::textColourId, juce::Colour(light));
            ratioLabels[(size_t) i].setInterceptsMouseClicks(false, false);
            addAndMakeVisible(ratioLabels[(size_t) i]);
            auto& s = levelSliders[(size_t) i];
            setupSlider(s, 0, 99, juce::Slider::LinearHorizontal);
            s.setColour(juce::Slider::trackColourId, operatorColour(opNumber));
            s.setColour(juce::Slider::thumbColourId, operatorColour(opNumber));
            s.onValueChange = [this, i, &s] { setByte(i * 21 + 16, (int) s.getValue()); updateRatioLabels(); };
            addAndMakeVisible(s);
        }
        opButtons[5].setToggleState(true, juce::dontSendNotification);

        // Oscillator.
        for (int i = 0; i < 2; ++i)
        {
            modeButtons[(size_t) i].setButtonText(i == 0 ? "RATIO" : "FIXED");
            modeButtons[(size_t) i].setClickingTogglesState(true);
            modeButtons[(size_t) i].setRadioGroupId(8403);
            modeButtons[(size_t) i].setConnectedEdges(i == 0 ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
            modeButtons[(size_t) i].onClick = [this, i] { setByte(opBase() + 17, i); updateWidgets(); };
            addAndMakeVisible(modeButtons[(size_t) i]);
        }
        const auto field = [this](LabeledSlider& ls, const char* name, int minV, int maxV, int relOffset)
        {
            setupSlider(ls.slider, minV, maxV, juce::Slider::LinearHorizontal);
            ls.label.setText(name, juce::dontSendNotification);
            ls.label.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            ls.label.setColour(juce::Label::textColourId, juce::Colour(0xff9fb0b6));
            ls.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 16);
            ls.slider.onValueChange = [this, &ls, relOffset] { setByte(opBase() + relOffset, (int) ls.slider.getValue()); updateRatioLabels(); };
            addAndMakeVisible(ls.slider); addAndMakeVisible(ls.label);
        };
        field(coarse, "COARSE", 0, 31, 18);
        field(fine, "FINE", 0, 99, 19);
        field(detune, "DETUNE", 0, 14, 20);
        detune.slider.textFromValueFunction = [](double v) { return juce::String((int) v - 7); };
        field(breakpoint, "BREAKPOINT", 0, 99, 8);
        field(leftDepth, "LEFT DEPTH", 0, 99, 9);
        field(rightDepth, "RIGHT DEPTH", 0, 99, 10);
        field(rateScaling, "RATE SCALING", 0, 7, 13);
        field(velocitySens, "VEL SENS", 0, 7, 15);
        field(ampModSens, "AMP MOD SENS", 0, 3, 14);
        for (auto* c : { &leftCurve, &rightCurve })
        {
            c->addItem("-LIN", 1); c->addItem("-EXP", 2); c->addItem("+EXP", 3); c->addItem("+LIN", 4);
            styleCombo(*c);
            addAndMakeVisible(*c);
        }
        leftCurve.onChange = [this] { setByte(opBase() + 11, leftCurve.getSelectedId() - 1); };
        rightCurve.onChange = [this] { setByte(opBase() + 12, rightCurve.getSelectedId() - 1); };
        for (auto* l : { &leftCurveLabel, &rightCurveLabel })
        {
            l->setFont(juce::FontOptions(9.5f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, juce::Colour(0xff9fb0b6));
            addAndMakeVisible(*l);
        }
        leftCurveLabel.setText("LEFT CURVE", juce::dontSendNotification);
        rightCurveLabel.setText("RIGHT CURVE", juce::dontSendNotification);

        for (int i = 0; i < 2; ++i)
        {
            subButtons[(size_t) i].setButtonText(i == 0 ? "ENVELOPE" : "LEVEL SCALING");
            subButtons[(size_t) i].setClickingTogglesState(true);
            subButtons[(size_t) i].setRadioGroupId(8404);
            subButtons[(size_t) i].setConnectedEdges(i == 0 ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
            subButtons[(size_t) i].onClick = [this, i] { subPage = i; updateVisibility(); resized(); };
            addAndMakeVisible(subButtons[(size_t) i]);
        }
        subButtons[0].setToggleState(true, juce::dontSendNotification);
        envelope.get = [this](int k) { return (int) raw[(size_t) (opBase() + k)]; };
        envelope.set = [this](int k, int v) { setByte(opBase() + k, v); };
        addAndMakeVisible(envelope);

        // Global.
        addAndMakeVisible(algorithmView);
        for (int i = 0; i < 2; ++i)
        {
            algStep[(size_t) i].setButtonText(i == 0 ? "<" : ">");
            algStep[(size_t) i].onClick = [this, i] { setByte(134, juce::jlimit(0, 31, (int) raw[134] + (i == 0 ? -1 : 1))); updateWidgets(); };
            addAndMakeVisible(algStep[(size_t) i]);
        }
        algLabel.setJustificationType(juce::Justification::centred);
        algLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        algLabel.setColour(juce::Label::textColourId, juce::Colour(light));
        addAndMakeVisible(algLabel);
        setupSlider(feedback, 0, 7, juce::Slider::RotaryHorizontalVerticalDrag);
        feedback.setLookAndFeel(&knobLook);
        feedback.setRotaryParameters(juce::degreesToRadians(-135.0f), juce::degreesToRadians(135.0f), true);
        feedback.onValueChange = [this] { setByte(135, (int) feedback.getValue()); };
        addAndMakeVisible(feedback);
        feedbackLabel.setText("FEEDBACK", juce::dontSendNotification);
        oscSync.setButtonText("OSC SYNC");
        oscSync.setClickingTogglesState(true);
        oscSync.onClick = [this] { setByte(136, oscSync.getToggleState() ? 1 : 0); };
        addAndMakeVisible(oscSync);
        transpose.label.setText("TRANSPOSE", juce::dontSendNotification);
        setupSlider(transpose.slider, 0, 48, juce::Slider::LinearHorizontal);
        transpose.slider.textFromValueFunction = [](double v) { return juce::String((int) v - 24); };
        transpose.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 16);
        transpose.slider.onValueChange = [this] { setByte(144, (int) transpose.slider.getValue()); };
        addAndMakeVisible(transpose.slider); addAndMakeVisible(transpose.label);

        // MODS: LFO and pitch EG.
        const auto lfoField = [this](LabeledSlider& ls, const char* name, int minV, int maxV, int byte)
        {
            setupSlider(ls.slider, minV, maxV, juce::Slider::LinearHorizontal);
            ls.label.setText(name, juce::dontSendNotification);
            ls.label.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            ls.label.setColour(juce::Label::textColourId, juce::Colour(0xff9fb0b6));
            ls.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 16);
            ls.slider.onValueChange = [this, &ls, byte] { setByte(byte, (int) ls.slider.getValue()); };
            addAndMakeVisible(ls.slider); addAndMakeVisible(ls.label);
        };
        lfoField(lfoSpeed, "LFO SPEED", 0, 99, 137);
        lfoField(lfoDelay, "LFO DELAY", 0, 99, 138);
        lfoField(pitchModDepth, "PITCH MOD DEPTH", 0, 99, 139);
        lfoField(ampModDepth, "AMP MOD DEPTH", 0, 99, 140);
        lfoField(pitchModSens, "PITCH MOD SENS", 0, 7, 143);
        for (const char* name : { "TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "SAMPLE+HOLD" })
            lfoWave.addItem(name, lfoWave.getNumItems() + 1);
        styleCombo(lfoWave);
        lfoWave.onChange = [this] { setByte(142, lfoWave.getSelectedId() - 1); };
        addAndMakeVisible(lfoWave);
        lfoWaveLabel.setText("LFO WAVE", juce::dontSendNotification);
        lfoSync.setButtonText("LFO KEY SYNC");
        lfoSync.setClickingTogglesState(true);
        lfoSync.onClick = [this] { setByte(141, lfoSync.getToggleState() ? 1 : 0); };
        addAndMakeVisible(lfoSync);
        pitchEnvelope.colour = juce::Colour(0xffe8c24a);
        pitchEnvelope.get = [this](int k) { return (int) raw[(size_t) (k < 4 ? 126 + k : 130 + (k - 4))]; };
        pitchEnvelope.set = [this](int k, int v) { setByte(k < 4 ? 126 + k : 130 + (k - 4), v); };
        addAndMakeVisible(pitchEnvelope);
        pitchEgLabel.setText("PITCH EG", juce::dontSendNotification);

        for (auto* l : { &feedbackLabel, &lfoWaveLabel, &pitchEgLabel })
        {
            l->setFont(juce::FontOptions(9.5f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, juce::Colour(0xff9fb0b6));
            addAndMakeVisible(*l);
        }
        for (auto* b : std::initializer_list<juce::TextButton*>{ &pageButtons[0], &pageButtons[1], &modeButtons[0], &modeButtons[1],
                                                                  &subButtons[0], &subButtons[1], &algStep[0], &algStep[1], &oscSync, &lfoSync })
        {
            b->setColour(juce::TextButton::buttonColourId, juce::Colour(card));
            b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(teal));
            b->setColour(juce::TextButton::textColourOffId, juce::Colour(light));
            b->setColour(juce::TextButton::textColourOnId, juce::Colour(ink));
        }
        setSize(1036, 420);
        updateVisibility();
        refreshFromEngine(true);
        startTimerHz(8);
    }
    ~Dx7VoiceEditorPanel() override { stopTimer(); feedback.setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override
    {
        using namespace dx7ui;
        g.setColour(juce::Colour(ink)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
        g.setColour(juce::Colour(line)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
        g.setColour(juce::Colour(teal));
        g.fillRect(juce::Rectangle<float>(0.0f, 36.0f, (float) getWidth(), 2.0f));
        if (!valid)
        {
            g.setColour(juce::Colour(light).withAlpha(0.7f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(juce::String::fromUTF8("Carregue um banco DX7 (.syx) para editar os operadores."),
                       getLocalBounds().withTrimmedTop(40), juce::Justification::centred);
            return;
        }
        const auto title = [&g](const juce::String& text, juce::Rectangle<int> r)
        {
            g.setColour(juce::Colour(card)); g.fillRoundedRectangle(r.toFloat(), 4.0f);
            g.setColour(juce::Colour(teal)); g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText(text, r.withHeight(18).withTrimmedLeft(8), juce::Justification::centredLeft);
        };
        if (page == 0)
        {
            title("OP  FREQ / RATIO  LEVEL", opArea);
            title("OSCILLATOR - OP" + juce::String(selectedOp), oscArea);
            title(subPage == 0 ? "ENVELOPE - OP" + juce::String(selectedOp) : "LEVEL SCALING - OP" + juce::String(selectedOp), envArea);
            title("ALGORITHM / OUT", globalArea);
            g.setColour(operatorColour(selectedOp));
            g.fillRect(juce::Rectangle<int>(oscArea.getX(), oscArea.getY() + 18, 3, 6));
        }
        else
        {
            title("LFO", lfoArea);
            title("PITCH EG", pitchArea);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 6);
        auto header = area.removeFromTop(26);
        pageButtons[0].setBounds(header.removeFromLeft(110));
        pageButtons[1].setBounds(header.removeFromLeft(110));
        area.removeFromTop(8);
        if (page == 0)
        {
            const int gap = 6;
            opArea = area.removeFromLeft(area.getWidth() * 30 / 100);
            area.removeFromLeft(gap);
            globalArea = area.removeFromRight(area.getWidth() * 36 / 100);
            area.removeFromRight(gap);
            oscArea = area.removeFromTop(112);
            area.removeFromTop(gap);
            envArea = area;
            // Operator list.
            auto ops = opArea.withTrimmedTop(22).reduced(4, 2);
            const int rowH = ops.getHeight() / 6;
            for (int i = 0; i < 6; ++i)
            {
                auto row = ops.removeFromTop(rowH).reduced(0, 2);
                opButtons[(size_t) i].setBounds(row.removeFromLeft(46));
                ratioLabels[(size_t) i].setBounds(row.removeFromLeft(62));
                levelSliders[(size_t) i].setBounds(row.reduced(2, 0));
            }
            // Oscillator.
            auto osc = oscArea.withTrimmedTop(22).reduced(8, 2);
            auto modeRow = osc.removeFromTop(24);
            modeButtons[0].setBounds(modeRow.removeFromLeft(70));
            modeButtons[1].setBounds(modeRow.removeFromLeft(70));
            osc.removeFromTop(4);
            const int fh = osc.getHeight() / 3;
            const auto place = [](dx7ui_LabeledSliderRef ls, juce::Rectangle<int> r)
            {
                ls.label.setBounds(r.removeFromLeft(86));
                ls.slider.setBounds(r);
            };
            place({ coarse.label, coarse.slider }, osc.removeFromTop(fh));
            place({ fine.label, fine.slider }, osc.removeFromTop(fh));
            place({ detune.label, detune.slider }, osc.removeFromTop(fh));
            // Envelope / level scaling.
            auto env = envArea.withTrimmedTop(22).reduced(8, 2);
            auto subRow = env.removeFromTop(24);
            subButtons[0].setBounds(subRow.removeFromLeft(100));
            subButtons[1].setBounds(subRow.removeFromLeft(120));
            env.removeFromTop(4);
            envelope.setBounds(env);
            {
                auto scaling = env;
                const int colW = scaling.getWidth() / 2;
                auto left = scaling.removeFromLeft(colW).reduced(0, 0);
                auto right = scaling.reduced(6, 0);
                const int rh = juce::jmax(20, left.getHeight() / 4);
                place({ breakpoint.label, breakpoint.slider }, left.removeFromTop(rh));
                place({ leftDepth.label, leftDepth.slider }, left.removeFromTop(rh));
                place({ rightDepth.label, rightDepth.slider }, left.removeFromTop(rh));
                auto curves = left.removeFromTop(rh);
                leftCurveLabel.setBounds(curves.removeFromLeft(86)); leftCurve.setBounds(curves.removeFromLeft(80));
                place({ rateScaling.label, rateScaling.slider }, right.removeFromTop(rh));
                place({ velocitySens.label, velocitySens.slider }, right.removeFromTop(rh));
                place({ ampModSens.label, ampModSens.slider }, right.removeFromTop(rh));
                auto curves2 = right.removeFromTop(rh);
                rightCurveLabel.setBounds(curves2.removeFromLeft(86)); rightCurve.setBounds(curves2.removeFromLeft(80));
            }
            // Global.
            auto glob = globalArea.withTrimmedTop(22).reduced(8, 2);
            auto algRow = glob.removeFromTop(24);
            algStep[0].setBounds(algRow.removeFromLeft(30));
            algStep[1].setBounds(algRow.removeFromRight(30));
            algLabel.setBounds(algRow);
            glob.removeFromTop(4);
            auto lower = glob.removeFromBottom(juce::jmin(120, glob.getHeight() / 2));
            algorithmView.setBounds(glob);
            auto knobArea = lower.removeFromLeft(lower.getWidth() / 2);
            feedbackLabel.setBounds(knobArea.removeFromBottom(14));
            feedback.setBounds(knobArea.reduced(juce::jmax(0, (knobArea.getWidth() - 60) / 2), 0));
            oscSync.setBounds(lower.removeFromTop(26).reduced(6, 0));
            lower.removeFromTop(6);
            auto tr = lower;
            transpose.label.setBounds(tr.removeFromTop(14));
            transpose.slider.setBounds(tr.removeFromTop(22));
        }
        else
        {
            lfoArea = area.removeFromLeft(area.getWidth() / 2);
            area.removeFromLeft(6);
            pitchArea = area;
            auto lfo = lfoArea.withTrimmedTop(22).reduced(8, 2);
            const int rh = juce::jmax(24, lfo.getHeight() / 7);
            const auto row = [&](LabeledSlider& ls)
            {
                auto r = lfo.removeFromTop(rh);
                ls.label.setBounds(r.removeFromLeft(120)); ls.slider.setBounds(r);
            };
            row(lfoSpeed); row(lfoDelay); row(pitchModDepth); row(ampModDepth); row(pitchModSens);
            auto waveRow = lfo.removeFromTop(rh);
            lfoWaveLabel.setBounds(waveRow.removeFromLeft(120)); lfoWave.setBounds(waveRow.removeFromLeft(160));
            lfoSync.setBounds(lfo.removeFromTop(rh).removeFromLeft(160));
            auto pitch = pitchArea.withTrimmedTop(22).reduced(8, 4);
            pitchEnvelope.setBounds(pitch);
        }
    }

private:
    struct LabeledSlider { juce::Slider slider; juce::Label label; };
    struct dx7ui_LabeledSliderRef { juce::Label& label; juce::Slider& slider; };

    void setupSlider(juce::Slider& s, int minV, int maxV, juce::Slider::SliderStyle style)
    {
        s.setSliderStyle(style);
        s.setRange(minV, maxV, 1);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setColour(juce::Slider::trackColourId, juce::Colour(dx7ui::teal));
        s.setColour(juce::Slider::thumbColourId, juce::Colour(dx7ui::teal));
        s.setColour(juce::Slider::backgroundColourId, juce::Colour(dx7ui::line));
        s.setColour(juce::Slider::textBoxTextColourId, juce::Colour(dx7ui::light));
        s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0x00000000));
        s.setPopupDisplayEnabled(true, true, this);
    }
    static void styleCombo(juce::ComboBox& c)
    {
        c.setColour(juce::ComboBox::backgroundColourId, juce::Colour(dx7ui::card));
        c.setColour(juce::ComboBox::textColourId, juce::Colour(dx7ui::light));
        c.setColour(juce::ComboBox::outlineColourId, juce::Colour(dx7ui::line));
        c.setColour(juce::ComboBox::arrowColourId, juce::Colour(dx7ui::teal));
    }
    int opBase() const { return (6 - selectedOp) * 21; }

    void setByte(int byteIndex, int value)
    {
        if (!valid || !juce::isPositiveAndBelow(byteIndex, 156)) return;
        const auto v = (uint8_t) juce::jlimit(0, 255, value);
        if (raw[(size_t) byteIndex] == v) return;
        raw[(size_t) byteIndex] = v;
        processor.setDx7PatchRaw(index, raw);
        if (envelope.isVisible()) envelope.repaint();
        pitchEnvelope.repaint();
        if (byteIndex == 134) algorithmView.setAlgorithm(raw[134]);
    }

    void updateRatioLabels()
    {
        for (int i = 0; i < 6; ++i)
        {
            const int base = i * 21;
            const int coarseValue = raw[(size_t) (base + 18)], fineValue = raw[(size_t) (base + 19)];
            juce::String text;
            if (raw[(size_t) (base + 17)] == 0)
                text = juce::String((coarseValue == 0 ? 0.5 : (double) coarseValue) * (1.0 + fineValue / 100.0), 2);
            else
                text = juce::String(std::pow(10.0, coarseValue % 4) * std::pow(10.0, fineValue / 100.0), 1) + " Hz";
            ratioLabels[(size_t) i].setText(text, juce::dontSendNotification);
        }
    }

    void updateWidgets()
    {
        if (!valid) return;
        const int base = opBase();
        for (int i = 0; i < 6; ++i)
        {
            levelSliders[(size_t) i].setValue(raw[(size_t) (i * 21 + 16)], juce::dontSendNotification);
            opButtons[(size_t) i].setToggleState(selectedOp == 6 - i, juce::dontSendNotification);
        }
        modeButtons[0].setToggleState(raw[(size_t) (base + 17)] == 0, juce::dontSendNotification);
        modeButtons[1].setToggleState(raw[(size_t) (base + 17)] != 0, juce::dontSendNotification);
        coarse.slider.setValue(raw[(size_t) (base + 18)], juce::dontSendNotification);
        fine.slider.setValue(raw[(size_t) (base + 19)], juce::dontSendNotification);
        detune.slider.setValue(raw[(size_t) (base + 20)], juce::dontSendNotification);
        breakpoint.slider.setValue(raw[(size_t) (base + 8)], juce::dontSendNotification);
        leftDepth.slider.setValue(raw[(size_t) (base + 9)], juce::dontSendNotification);
        rightDepth.slider.setValue(raw[(size_t) (base + 10)], juce::dontSendNotification);
        leftCurve.setSelectedId(juce::jlimit(0, 3, (int) raw[(size_t) (base + 11)]) + 1, juce::dontSendNotification);
        rightCurve.setSelectedId(juce::jlimit(0, 3, (int) raw[(size_t) (base + 12)]) + 1, juce::dontSendNotification);
        rateScaling.slider.setValue(raw[(size_t) (base + 13)], juce::dontSendNotification);
        ampModSens.slider.setValue(raw[(size_t) (base + 14)], juce::dontSendNotification);
        velocitySens.slider.setValue(raw[(size_t) (base + 15)], juce::dontSendNotification);
        algorithmView.setAlgorithm(raw[134] & 0x1f);
        algLabel.setText("ALGORITHM " + juce::String((raw[134] & 0x1f) + 1), juce::dontSendNotification);
        feedback.setValue(raw[135] & 7, juce::dontSendNotification);
        oscSync.setToggleState(raw[136] != 0, juce::dontSendNotification);
        transpose.slider.setValue(raw[144], juce::dontSendNotification);
        lfoSpeed.slider.setValue(raw[137], juce::dontSendNotification);
        lfoDelay.slider.setValue(raw[138], juce::dontSendNotification);
        pitchModDepth.slider.setValue(raw[139], juce::dontSendNotification);
        ampModDepth.slider.setValue(raw[140], juce::dontSendNotification);
        lfoSync.setToggleState(raw[141] != 0, juce::dontSendNotification);
        lfoWave.setSelectedId(juce::jlimit(0, 5, (int) raw[142]) + 1, juce::dontSendNotification);
        pitchModSens.slider.setValue(raw[143], juce::dontSendNotification);
        updateRatioLabels();
        envelope.repaint();
        pitchEnvelope.repaint();
    }

    void refreshFromEngine(bool force)
    {
        std::array<uint8_t, 156> current {};
        const bool ok = processor.dx7PatchRaw(index, current);
        if (ok != valid) { valid = ok; updateVisibility(); resized(); repaint(); }
        if (ok && (force || current != raw))
        {
            raw = current;
            updateWidgets();
        }
    }

    void updateVisibility()
    {
        const bool overview = page == 0 && valid, scaling = overview && subPage == 1, graph = overview && subPage == 0;
        for (int i = 0; i < 6; ++i) { opButtons[(size_t) i].setVisible(overview); ratioLabels[(size_t) i].setVisible(overview); levelSliders[(size_t) i].setVisible(overview); }
        for (auto& b : modeButtons) b.setVisible(overview);
        for (auto& b : subButtons) b.setVisible(overview);
        for (auto* ls : { &coarse, &fine, &detune }) { ls->slider.setVisible(overview); ls->label.setVisible(overview); }
        for (auto* ls : { &breakpoint, &leftDepth, &rightDepth, &rateScaling, &velocitySens, &ampModSens }) { ls->slider.setVisible(scaling); ls->label.setVisible(scaling); }
        for (auto* c : { &leftCurve, &rightCurve }) c->setVisible(scaling);
        leftCurveLabel.setVisible(scaling); rightCurveLabel.setVisible(scaling);
        envelope.setVisible(graph);
        algorithmView.setVisible(overview);
        for (auto& b : algStep) b.setVisible(overview);
        algLabel.setVisible(overview); feedback.setVisible(overview); feedbackLabel.setVisible(overview); oscSync.setVisible(overview);
        transpose.slider.setVisible(overview); transpose.label.setVisible(overview);
        const bool mods = page == 1 && valid;
        for (auto* ls : { &lfoSpeed, &lfoDelay, &pitchModDepth, &ampModDepth, &pitchModSens }) { ls->slider.setVisible(mods); ls->label.setVisible(mods); }
        lfoWave.setVisible(mods); lfoWaveLabel.setVisible(mods); lfoSync.setVisible(mods);
        pitchEnvelope.setVisible(mods); pitchEgLabel.setVisible(false);
    }

    void timerCallback() override { if (isShowing()) refreshFromEngine(false); }

    ClassicPlayerAudioProcessor& processor;
    int index;
    std::array<uint8_t, 156> raw {};
    bool valid = false;
    int page = 0, subPage = 0, selectedOp = 1;
    dx7ui::KnobLook knobLook;
    std::array<juce::TextButton, 2> pageButtons, modeButtons, subButtons, algStep;
    std::array<juce::TextButton, 6> opButtons;
    std::array<juce::Label, 6> ratioLabels;
    std::array<juce::Slider, 6> levelSliders;
    LabeledSlider coarse, fine, detune, breakpoint, leftDepth, rightDepth, rateScaling, velocitySens, ampModSens;
    LabeledSlider transpose, lfoSpeed, lfoDelay, pitchModDepth, ampModDepth, pitchModSens;
    juce::ComboBox leftCurve, rightCurve, lfoWave;
    juce::Label leftCurveLabel, rightCurveLabel, feedbackLabel, lfoWaveLabel, pitchEgLabel, algLabel;
    juce::Slider feedback;
    juce::TextButton oscSync, lfoSync;
    dx7ui::EnvelopeGraph envelope, pitchEnvelope;
    dx7ui::AlgorithmView algorithmView;
    juce::Rectangle<int> opArea, oscArea, envArea, globalArea, lfoArea, pitchArea;
};
