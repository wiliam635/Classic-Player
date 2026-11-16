#pragma once
#include "PluginProcessor.h"
#include "MidiLearnButton.h"
#include "ClassicPlayerAssets.h"
#include <functional>
#include <utility>

std::unique_ptr<juce::Component> createHammondEditorContent(
    ClassicPlayerAudioProcessor&, int layer,
    std::function<void()> reverbCallback = {},
    std::function<void()> compressorCallback = {},
    std::function<void()> chorusCallback = {},
    std::function<void()> eqCallback = {},
    std::function<void()> savePresetCallback = {},
    std::function<void()> loadPresetCallback = {});

// Instrument controls only: deliberately no on-screen keyboard.
class HammondEditorPanel final : public juce::Component, private juce::Timer
{
    class DrawbarLook final : public juce::LookAndFeel_V4
    {
    public:
        DrawbarLook()
        {
            images[0] = load(ClassicPlayerAssets::drawbarred_png, ClassicPlayerAssets::drawbarred_pngSize);
            images[1] = load(ClassicPlayerAssets::drawbarivory_png, ClassicPlayerAssets::drawbarivory_pngSize);
            images[2] = load(ClassicPlayerAssets::drawbarblack_png, ClassicPlayerAssets::drawbarblack_pngSize);
        }
        void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                              float sliderPos, float, float, juce::Slider::SliderStyle,
                              juce::Slider& slider) override
        {
            const juce::Graphics::ScopedSaveState graphicsState(g);
            const auto centre = (float) x + (float) width * 0.5f;
            g.setColour(juce::Colours::black);
            g.fillRoundedRectangle(centre - 4.0f, (float)y + 2.0f, 8.0f,
                                   juce::jmax(1.0f, (float)height - 4.0f), 4.0f);
            g.setColour(slider.findColour(juce::Slider::trackColourId).withAlpha(0.7f));
            g.drawRoundedRectangle(centre - 4.0f, (float)y + 2.0f, 8.0f,
                                   juce::jmax(1.0f, (float)height - 4.0f), 4.0f, 1.0f);
            const auto index = juce::jlimit(0, 2, (int) slider.getProperties()["drawbarColour"]);
            const auto thumbWidth = juce::jmin(31.0f, (float)width * 0.65f);
            const auto thumbHeight = juce::jmin(37.0f, (float)height * 0.36f);
            // The translucent track colour also sets Graphics' image opacity.
            // Restore full opacity before drawing the physical drawbar cap.
            g.setOpacity(1.0f);
            if (images[(size_t)index].isValid())
                g.drawImage(images[(size_t)index], juce::Rectangle<float>(centre - thumbWidth * 0.5f,
                            sliderPos - thumbHeight * 0.5f, thumbWidth, thumbHeight),
                            juce::RectanglePlacement::stretchToFit);
        }
    private:
        static juce::Image load(const char* data, int size)
        {
            const auto image = juce::ImageCache::getFromMemory(data, size);
            return image.isValid() ? image.getClippedImage({260, 125, 790, 1020}) : juce::Image{};
        }
        std::array<juce::Image, 3> images;
    } drawbarLook;

    // Black ribbed organ knob with a white pointer and tick marks.
    class OrganKnobLook final : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float pos,
                              float startAngle, float endAngle, juce::Slider& slider) override
        {
            const auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height);
            const auto size = juce::jmin(bounds.getWidth(), bounds.getHeight());
            const auto centre = bounds.getCentre();
            const float radius = size * 0.5f - 6.0f;
            const int ticks = (int) slider.getProperties().getWithDefault("ticks", 11);
            g.setColour(juce::Colour(0xffb59a5c).withAlpha(0.75f));
            for (int i = 0; i < ticks; ++i)
            {
                const float a = startAngle + (endAngle - startAngle) * (float) i / (float) juce::jmax(1, ticks - 1);
                const juce::Point<float> d(std::sin(a), -std::cos(a));
                g.drawLine({ centre + d * (radius + 2.0f), centre + d * (radius + 5.5f) }, 1.4f);
            }
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff5a5a5c), centre.x - radius * 0.4f, centre.y - radius * 0.6f,
                                                   juce::Colour(0xff0b0b0c), centre.x, centre.y + radius, true));
            g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
            // Ribbed rim.
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            for (int i = 0; i < 48; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 48.0f;
                const juce::Point<float> d(std::sin(a), -std::cos(a));
                g.drawLine({ centre + d * (radius - 3.0f), centre + d * (radius - 0.5f) }, 1.0f);
            }
            g.setColour(juce::Colour(0xff6b6b6d)); g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.4f);
            g.setColour(juce::Colour(0xff111112));
            g.fillEllipse(centre.x - radius * 0.62f, centre.y - radius * 0.62f, radius * 1.24f, radius * 1.24f);
            const float angle = startAngle + pos * (endAngle - startAngle);
            const juce::Point<float> d(std::sin(angle), -std::cos(angle));
            g.setColour(juce::Colours::white);
            g.drawLine({ centre + d * (radius * 0.18f), centre + d * (radius * 0.9f) }, 3.2f);
        }
    } knobLook;

    // Ivory rocker switch: upper and lower position are both labelled.
    class Rocker final : public juce::Component
    {
    public:
        Rocker(juce::String upper, juce::String lower) : top(std::move(upper)), bottom(std::move(lower)) {}
        std::function<void(bool)> onChange;
        void setState(bool upperActive) { if (state != upperActive) { state = upperActive; repaint(); } }
        bool getState() const { return state; }
        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced(2.0f);
            g.setColour(juce::Colour(0xff050505)); g.fillRoundedRectangle(r.expanded(1.5f), 5.0f);
            g.setGradientFill(juce::ColourGradient(state ? juce::Colour(0xfffbf5de) : juce::Colour(0xffcfc7a9), r.getX(), r.getY(),
                                                   state ? juce::Colour(0xffc7bf9f) : juce::Colour(0xfffbf5de), r.getX(), r.getBottom(), false));
            g.fillRoundedRectangle(r, 4.0f);
            g.setColour(juce::Colour(0xff8f876c)); g.drawRoundedRectangle(r, 4.0f, 1.0f);
            const auto half = r.getHeight() * 0.5f;
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.setColour(state ? juce::Colour(0xff161513) : juce::Colour(0xff161513).withAlpha(0.38f));
            g.drawText(top, r.withHeight(half).translated(0, 3), juce::Justification::centred);
            g.setColour(!state ? juce::Colour(0xff161513) : juce::Colour(0xff161513).withAlpha(0.38f));
            g.drawText(bottom, r.withTrimmedTop(half).translated(0, -3), juce::Justification::centred);
        }
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) return;
            const bool upper = e.y < getHeight() / 2;
            if (upper == state) return;
            state = upper; repaint();
            if (onChange) onChange(state);
        }
    private:
        juce::String top, bottom; bool state = true;
    };
public:
    void setUiTextTranslator(std::function<juce::String(const juce::String&)> translator)
    {
        translateUiText = std::move(translator);
        refresh();
    }

    HammondEditorPanel(ClassicPlayerAudioProcessor& p,int layer):processor(p),index(layer)
    {
        preset.addItem("Personalizado",1);
        const auto names=HammondEngine::presetNames();
        for(int i=0;i<names.size();++i)preset.addItem(names[i],i+2);
        addAndMakeVisible(preset);
        preset.onChange=[this]{
            if(preset.getSelectedId()<2)return;
            processor.setLayerReleaseControl(index, 0.0f);
            const auto old=processor.hammondConfig(index);
            auto c=HammondEngine::preset(preset.getSelectedId()-2);
            c.routing=old.routing;c.routing.release=0.0f;c.cc=old.cc;c.channel=old.channel;c.level=old.level;
            c.vibrato=old.vibrato;c.leslieBalance=old.leslieBalance;
            processor.setHammondConfig(index,c);refresh();
        };
        // Keep these labels strictly ASCII: some macOS/Windows UI fonts render
        // the prime/foot mark as accented or superscript characters at small sizes.
        const std::array<const char*,9> namesOfBars {"16 ft","5 1/3 ft","8 ft","4 ft","2 2/3 ft","2 ft","1 3/5 ft","1 1/3 ft","1 ft"};
        for(size_t i=0;i<bars.size();++i){
            labels[i].setText(namesOfBars[i],juce::dontSendNotification);
            labels[i].setJustificationType(juce::Justification::centred);
            labels[i].setFont(juce::FontOptions(11.0f,juce::Font::bold));addAndMakeVisible(labels[i]);
            bars[i].setSliderStyle(juce::Slider::LinearVertical);bars[i].setRange(0,8,1);
            bars[i].getProperties().set("drawbarColour", i < 2 ? 0 : (i == 4 || i == 6 || i == 7 ? 2 : 1));
            bars[i].setLookAndFeel(&drawbarLook);
            bars[i].getProperties().set("midiKey", "pk:ham:" + juce::String(index) + ":bar" + juce::String((int) i));
            bars[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,42,18);
            addAndMakeVisible(bars[i]);
            bars[i].onValueChange=[this,i]{auto c=processor.hammondConfig(index);const auto value=(int)bars[i].getValue();if(c.bars[i]==value)return;c.bars[i]=value;c.preset=-1;processor.setHammondConfig(index,c);};
            target.addItem("Drawbar "+juce::String(namesOfBars[i]),(int)i+1);
        }
        // Tabs.
        for(size_t i=0;i<2;++i){
            tabs[i].setButtonText(i==0?"MAIN":"OPTIONS");
            tabs[i].setClickingTogglesState(true);tabs[i].setRadioGroupId(8310);
            tabs[i].setConnectedEdges((i==0?juce::Button::ConnectedOnRight:juce::Button::ConnectedOnLeft));
            tabs[i].onClick=[this,i]{showTab((int)i);};
            addAndMakeVisible(tabs[i]);
        }
        tabs[0].setToggleState(true,juce::dontSendNotification);
        // Switches.
        vibratoSwitch.onChange=[this](bool on){auto c=processor.hammondConfig(index);
            c.vibrato=on?vibratoType+1:0;processor.setHammondConfig(index,c);};
        percussionSwitch.onChange=[this](bool on){auto c=processor.hammondConfig(index);
            c.percussion=on?percHarmonic:0;c.preset=-1;processor.setHammondConfig(index,c);};
        percVolumeSwitch.onChange=[this](bool soft){auto c=processor.hammondConfig(index);
            c.percSoft=soft;c.preset=-1;processor.setHammondConfig(index,c);};
        percTimeSwitch.onChange=[this](bool fast){auto c=processor.hammondConfig(index);
            c.percSlow=!fast;c.preset=-1;processor.setHammondConfig(index,c);};
        percHarmonicSwitch.onChange=[this](bool third){percHarmonic=third?3:2;auto c=processor.hammondConfig(index);
            if(c.percussion!=0){c.percussion=percHarmonic;c.preset=-1;processor.setHammondConfig(index,c);}};
        for(auto* r:{&vibratoSwitch,&percussionSwitch,&percVolumeSwitch,&percTimeSwitch,&percHarmonicSwitch})
            addAndMakeVisible(*r);
        const std::array<const char*,3> leslieNames {"STOP","SLOW","FAST"};
        for(size_t i=0;i<3;++i){
            leslieButtons[i].setButtonText(leslieNames[i]);
            leslieButtons[i].setClickingTogglesState(true);leslieButtons[i].setRadioGroupId(8301);
            leslieButtons[i].setConnectedEdges((i>0?juce::Button::ConnectedOnLeft:0)|(i<2?juce::Button::ConnectedOnRight:0));
            leslieButtons[i].setTooltip(juce::String::fromUTF8("Mod wheel (CC1): valores baixos = lento; valores altos = rápido"));
            addAndMakeVisible(leslieButtons[i]);
            leslieButtons[i].onClick=[this,i]{auto c=processor.hammondConfig(index);if(c.leslie!=(int)i){c.leslie=(int)i;processor.setHammondConfig(index,c);}refresh();};
        }
        // Knobs: click, leakage, drive, level, leslie balance, vibrato type.
        const std::array<const char*,6> knobNames {"KEY CLICK","LEAKAGE","DRIVE","LEVEL","HORN / DRUM","TYPE"};
        for(size_t i=0;i<knobs.size();++i){
            knobLabels[i].setText(knobNames[i],juce::dontSendNotification);
            knobLabels[i].setJustificationType(juce::Justification::centred);
            knobLabels[i].setFont(juce::FontOptions(10.0f,juce::Font::bold));addAndMakeVisible(knobLabels[i]);
            static const char* hammondKnobKeys[] { "click", "leakage", "drive", "level" };
            if (i < 4) knobs[i].getProperties().set("midiKey", "pk:ham:" + juce::String(index) + ":" + hammondKnobKeys[i]);
            knobs[i].setLookAndFeel(&knobLook);
            knobs[i].setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knobs[i].setRotaryParameters(juce::degreesToRadians(-150.0f),juce::degreesToRadians(150.0f),true);
            if(i==5){knobs[i].setRange(0,5,1);knobs[i].setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
                knobs[i].getProperties().set("ticks",6);}
            else{knobs[i].setRange(0,100,1);knobs[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,52,14);
                knobs[i].setTextValueSuffix(" %");}
            knobs[i].setPopupDisplayEnabled(false,false,nullptr);
            addAndMakeVisible(knobs[i]);
            knobs[i].onValueChange=[this,i]{
                auto c=processor.hammondConfig(index);
                if(i==5){
                    vibratoType=(int)knobs[i].getValue();
                    if(c.vibrato!=0&&c.vibrato!=vibratoType+1){c.vibrato=vibratoType+1;processor.setHammondConfig(index,c);}
                    return;
                }
                if(i==4){
                    const auto value=(float)knobs[i].getValue()/100.0f;
                    if(std::abs(c.leslieBalance-value)<0.0001f)return;
                    c.leslieBalance=value;processor.setHammondConfig(index,c);return;
                }
                float* values[]{&c.click,&c.leakage,&c.drive,&c.level};
                const auto value=(float)knobs[i].getValue()/100.0f;
                if(std::abs(*values[i]-value)<0.0001f)return;
                *values[i]=value;if(i<3)c.preset=-1;processor.setHammondConfig(index,c);
            };
        }
        target.addItem("Leslie / Mod wheel",10);target.addItem("Level",11);target.setSelectedId(1,juce::dontSendNotification);
        for(auto* component:std::initializer_list<juce::Component*>{&target,&learn,&clear,&mapping})addAndMakeVisible(component);
        target.onChange=[this]{auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);refresh();};
        learn.onClick=[this]{auto c=processor.hammondConfig(index);c.learning=c.learning>=0?-1:target.getSelectedId()-1;processor.setHammondConfig(index,c);refresh();};
        clear.onClick=[this]{auto c=processor.hammondConfig(index);c.cc[(size_t)(target.getSelectedId()-1)]=-1;c.learning=-1;processor.setHammondConfig(index,c);refresh();};
        learn.onClearMapping = clear.onClick;
        setSize(1036,300);applyThemeColours();showTab(0);refresh();startTimerHz(15);
    }
    ~HammondEditorPanel() override {
        stopTimer();
        for (auto& bar : bars) bar.setLookAndFeel(nullptr);
        for (auto& knob : knobs) knob.setLookAndFeel(nullptr);
        auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);
    }
    void resized() override
    {
        const int width = getWidth();
        const int height = getHeight();
        tabs[0].setBounds(10, 4, 84, 22);
        tabs[1].setBounds(94, 4, 84, 22);
        preset.setBounds(width - 290, 4, 280, 22);
        // MAIN page ---------------------------------------------------
        const int panelH = 112;
        const int drawbarBottom = height - panelH - 8;
        const int cell = (width - 24) / 9;
        for (size_t i = 0; i < bars.size(); ++i)
        {
            const int x = 12 + (int)i * cell;
            labels[i].setBounds(x, 31, cell, 15);
            bars[i].setBounds(x + 4, 46, cell - 8, drawbarBottom - 46);
        }
        const auto panel = juce::Rectangle<int>(12, height - panelH - 4, width - 24, panelH);
        mainPanel = panel;
        const int px = panel.getX(), py = panel.getY();
        const int pw = panel.getWidth();
        // Section x ranges.
        vibratoRange = { px + 8, px + (int)(pw * 0.27f) };
        driveRange = { vibratoRange.getEnd() + 6, vibratoRange.getEnd() + 6 + (int)(pw * 0.13f) };
        leslieRange = { driveRange.getEnd() + 6, driveRange.getEnd() + 6 + (int)(pw * 0.20f) };
        percRange = { leslieRange.getEnd() + 6, px + pw - 8 };
        const int controlsTop = py + 26;
        vibratoSwitch.setBounds(vibratoRange.getStart() + 6, controlsTop, 52, 58);
        knobs[5].setBounds(vibratoRange.getStart() + 74, controlsTop - 2, 86, 66);
        knobLabels[5].setBounds(vibratoRange.getStart() + 74, controlsTop + 62, 86, 12);
        knobs[2].setBounds(((driveRange.getStart() + driveRange.getEnd()) / 2) - 40, controlsTop - 2, 80, 80);
        knobLabels[2].setBounds(((driveRange.getStart() + driveRange.getEnd()) / 2) - 40, controlsTop + 78, 80, 12);
        {
            const int w = (leslieRange.getLength() - 12) / 3;
            for (int i = 0; i < 3; ++i)
                leslieButtons[(size_t)i].setBounds(leslieRange.getStart() + 6 + i * w, controlsTop + 14, w, 26);
        }
        {
            const int w = juce::jmin(66, percRange.getLength() / 4);
            const int startX = ((percRange.getStart() + percRange.getEnd()) / 2) - w * 2;
            percussionSwitch.setBounds(startX, controlsTop, w - 8, 58);
            percVolumeSwitch.setBounds(startX + w, controlsTop, w - 8, 58);
            percTimeSwitch.setBounds(startX + 2 * w, controlsTop, w - 8, 58);
            percHarmonicSwitch.setBounds(startX + 3 * w, controlsTop, w - 8, 58);
            percLabelX = startX; percLabelW = w;
        }
        // OPTIONS page ------------------------------------------------
        const int top = 40;
        const int groupW = (width - 24) / 3;
        optionsGroups = { juce::Rectangle<int>(12, top, groupW - 6, 164),
                          juce::Rectangle<int>(12 + groupW, top, groupW * 1 + 0 - 6, 164),
                          juce::Rectangle<int>(12 + groupW * 2, top, groupW - 6, 164) };
        const auto place = [this](size_t k, const juce::Rectangle<int>& area, int slot, int slots)
        {
            const int w = area.getWidth() / slots;
            knobs[k].setBounds(area.getX() + slot * w + w / 2 - 44, area.getY() + 34, 88, 92);
            knobLabels[k].setBounds(area.getX() + slot * w, area.getY() + 128, w, 14);
        };
        place(3, optionsGroups[0], 0, 1);
        place(0, optionsGroups[1], 0, 2); place(1, optionsGroups[1], 1, 2);
        place(4, optionsGroups[2], 0, 1);
        mappingPanel = juce::Rectangle<int>(12, height - 48, width - 24, 40);
        const int quarter = (mappingPanel.getWidth() - 16) / 4;
        const int mappingTop = mappingPanel.getY() + 9;
        target.setBounds(mappingPanel.getX() + 8, mappingTop, quarter - 4, 22);
        learn.setBounds(mappingPanel.getX() + 8 + quarter, mappingTop, quarter - 4, 22);
        clear.setBounds(mappingPanel.getX() + 8 + 2 * quarter, mappingTop, quarter - 4, 22);
        mapping.setBounds(mappingPanel.getX() + 8 + 3 * quarter, mappingTop, quarter - 4, 22);
        showTab(currentTab);
    }
    void lookAndFeelChanged() override
    {
        juce::Component::lookAndFeelChanged();
        applyThemeColours();repaint();
    }
    void paint(juce::Graphics& g) override
    {
        const auto gold = juce::Colour(0xffb59a5c);
        const auto w = (float) getWidth(), h = (float) getHeight();
        // Walnut frame with a faint grain.
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffa4521f), 0, 0, juce::Colour(0xff6c2e10), 0, h, false));
        g.fillAll();
        g.setColour(juce::Colour(0xff4a1d09).withAlpha(0.28f));
        for (int y = 3; y < getHeight(); y += 5)
            g.drawHorizontalLine(y, 0.0f, w);
        // Dark inset behind the active page.
        const auto inset = juce::Rectangle<float>(6.0f, 28.0f, w - 12.0f, h - 34.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff26272a), 0, inset.getY(), juce::Colour(0xff151618), 0, inset.getBottom(), false));
        g.fillRoundedRectangle(inset, 8.0f);
        g.setColour(juce::Colour(0xff0a0a0a)); g.drawRoundedRectangle(inset, 8.0f, 2.0f);
        const auto sectionTitle = [&g, &gold](const juce::String& text, int x, int y, int width)
        {
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            const int tw = text.length() * 7 + 14;
            const int cx = x + width / 2;
            g.setColour(gold.withAlpha(0.55f));
            g.drawHorizontalLine(y + 7, (float) x, (float) (cx - tw / 2));
            g.drawHorizontalLine(y + 7, (float) (cx + tw / 2), (float) (x + width));
            g.setColour(gold);
            g.drawText(text, cx - tw / 2, y, tw, 14, juce::Justification::centred);
        };
        if (currentTab == 0)
        {
            const float cell = (w - 24.0f) / 9.0f;
            const float drawbarBottom = (float) (getHeight() - 112 - 8);
            const float top = 52.0f, bottom = drawbarBottom - 28.0f;
            for (int i = 0; i < 9; ++i)
            {
                const float x = 12.0f + cell * (float) i + cell * 0.5f + 12.0f;
                for (int step = 0; step <= 8; ++step)
                {
                    const float y = bottom - (bottom - top) * (float) step / 8.0f;
                    g.setColour(gold.withAlpha(step % 2 == 0 ? 0.8f : 0.5f));
                    g.drawHorizontalLine(juce::roundToInt(y), x, x + (step % 2 == 0 ? 5.0f : 3.0f));
                    if (cell >= 75.0f && step > 0)
                    {
                        g.setColour(juce::Colours::white.withAlpha(0.82f));
                        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                        g.drawText(juce::String(step), juce::roundToInt(x + 7.0f), juce::roundToInt(y - 5.0f), 12, 10, juce::Justification::centredLeft);
                    }
                }
            }
            const auto p = mainPanel.toFloat();
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2a2b2e), 0, p.getY(), juce::Colour(0xff1a1b1d), 0, p.getBottom(), false));
            g.fillRoundedRectangle(p, 5.0f);
            g.setColour(juce::Colour(0xff050505)); g.drawRoundedRectangle(p, 5.0f, 1.5f);
            const int ty = mainPanel.getY() + 6;
            sectionTitle("VIBRATO / CHORUS", vibratoRange.getStart(), ty, vibratoRange.getLength());
            sectionTitle("DRIVE", driveRange.getStart(), ty, driveRange.getLength());
            sectionTitle("LESLIE", leslieRange.getStart(), ty, leslieRange.getLength());
            sectionTitle("PERCUSSION", percRange.getStart(), ty, percRange.getLength());
            g.setColour(gold.withAlpha(0.85f)); g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            const int ly = mainPanel.getY() + 26 + 62;
            g.drawText("ON / OFF", vibratoSwitch.getX() - 10, ly, vibratoSwitch.getWidth() + 20, 12, juce::Justification::centred);
            const std::array<const char*,4> pn { "PERC", "VOLUME", "TIME", "HARMONIC" };
            for (int i = 0; i < 4; ++i)
                g.drawText(pn[(size_t) i], percLabelX + i * percLabelW - 4, ly, percLabelW, 12, juce::Justification::centred);
            // Vibrato type names around the TYPE knob.
            const auto kb = knobs[5].getBounds().toFloat();
            const auto centre = kb.getCentre();
            const float rr = juce::jmin(kb.getWidth(), kb.getHeight()) * 0.5f + 7.0f;
            const std::array<const char*,6> tn { "V1", "V2", "V3", "C1", "C2", "C3" };
            g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            for (int i = 0; i < 6; ++i)
            {
                const float a = juce::degreesToRadians(-150.0f) + juce::degreesToRadians(300.0f) * (float) i / 5.0f;
                const auto pos = centre + juce::Point<float>(std::sin(a) * (rr + 6.0f), -std::cos(a) * (rr + 1.0f));
                g.setColour(i == vibratoType ? juce::Colours::white : gold.withAlpha(0.8f));
                g.drawText(tn[(size_t) i], juce::Rectangle<float>(pos.x - 11, pos.y - 6, 22, 12).toNearestInt(), juce::Justification::centred);
            }
        }
        else
        {
            const std::array<const char*,3> titles { "MASTER", "CLICK / LEAKAGE", "LESLIE" };
            for (size_t i = 0; i < 3; ++i)
            {
                const auto r = optionsGroups[i].toFloat();
                g.setColour(juce::Colour(0xff2a2b2e).withAlpha(0.7f)); g.fillRoundedRectangle(r, 5.0f);
                g.setColour(juce::Colour(0xff050505)); g.drawRoundedRectangle(r, 5.0f, 1.2f);
                sectionTitle(titles[i], optionsGroups[i].getX() + 8, optionsGroups[i].getY() + 8, optionsGroups[i].getWidth() - 16);
            }
            const auto m = mappingPanel.toFloat();
            g.setColour(juce::Colour(0xff2a2b2e).withAlpha(0.7f)); g.fillRoundedRectangle(m, 5.0f);
            g.setColour(juce::Colour(0xff050505)); g.drawRoundedRectangle(m, 5.0f, 1.2f);
        }
    }
private:
    void showTab(int tab)
    {
        currentTab = tab;
        const bool main = tab == 0;
        for (size_t i = 0; i < 9; ++i) { bars[i].setVisible(main); labels[i].setVisible(main); }
        for (auto* r : { &vibratoSwitch, &percussionSwitch, &percVolumeSwitch, &percTimeSwitch, &percHarmonicSwitch })
            r->setVisible(main);
        for (auto& b : leslieButtons) b.setVisible(main);
        for (size_t i : { (size_t) 2, (size_t) 5 }) { knobs[i].setVisible(main); knobLabels[i].setVisible(main); }
        for (size_t i : { (size_t) 0, (size_t) 1, (size_t) 3, (size_t) 4 }) { knobs[i].setVisible(!main); knobLabels[i].setVisible(!main); }
        for (auto* c : std::initializer_list<juce::Component*>{ &target, &learn, &clear, &mapping }) c->setVisible(!main);
        tabs[(size_t) tab].setToggleState(true, juce::dontSendNotification);
        repaint();
    }
    void applyThemeColours()
    {
        const auto gold = juce::Colour(0xffb59a5c);
        const auto dark = juce::Colour(0xff1c1d1f);
        for(auto* selector:{&preset,&target}){
            selector->setColour(juce::ComboBox::backgroundColourId,dark);
            selector->setColour(juce::ComboBox::outlineColourId,juce::Colour(0xff050505));
            selector->setColour(juce::ComboBox::textColourId,juce::Colour(0xffe8dcb8));
            selector->setColour(juce::ComboBox::arrowColourId,gold);
        }
        for(auto& label:labels)label.setColour(juce::Label::textColourId,juce::Colours::white);
        for(auto& label:knobLabels)label.setColour(juce::Label::textColourId,gold.withAlpha(0.9f));
        for(auto& bar:bars){
            bar.setColour(juce::Slider::trackColourId,gold);
            bar.setColour(juce::Slider::thumbColourId,juce::Colour(0xffd8dde0));
            bar.setColour(juce::Slider::textBoxTextColourId,juce::Colour(0xffe8dcb8));
            bar.setColour(juce::Slider::textBoxBackgroundColourId,juce::Colour(0xff0f1011));
            bar.setColour(juce::Slider::textBoxOutlineColourId,juce::Colour(0xff050505));
        }
        for(auto& knob:knobs){
            knob.setColour(juce::Slider::textBoxTextColourId,juce::Colour(0xffe8dcb8));
            knob.setColour(juce::Slider::textBoxBackgroundColourId,juce::Colour(0x00000000));
            knob.setColour(juce::Slider::textBoxOutlineColourId,juce::Colour(0x00000000));
        }
        const auto styleButton=[&](juce::TextButton& button){
            button.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff2b1d1a));
            button.setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xffd9cfae));
            button.setColour(juce::TextButton::textColourOffId,juce::Colour(0xffe8dcb8));
            button.setColour(juce::TextButton::textColourOnId,juce::Colour(0xff161513));
        };
        for(auto& button:leslieButtons)styleButton(button);
        for(auto& button:tabs)styleButton(button);
        mapping.setColour(juce::Label::textColourId,juce::Colour(0xffe8dcb8));
    }
    void timerCallback() override { if (isShowing()) refresh(); }
    void refresh()
    {
        const auto c=processor.hammondConfig(index);
        preset.setSelectedId(c.preset+2,juce::dontSendNotification);
        for(size_t i=0;i<9;++i)if(!bars[i].isMouseButtonDown())bars[i].setValue(c.bars[i],juce::dontSendNotification);
        for(int i=0;i<3;++i)leslieButtons[(size_t)i].setToggleState(c.leslie==i,juce::dontSendNotification);
        if(c.percussion!=0)percHarmonic=c.percussion;
        if(c.vibrato>0)vibratoType=c.vibrato-1;
        vibratoSwitch.setState(c.vibrato>0);
        percussionSwitch.setState(c.percussion!=0);
        percVolumeSwitch.setState(c.percSoft);
        percTimeSwitch.setState(!c.percSlow);
        percHarmonicSwitch.setState(percHarmonic==3);
        const std::array<float,6> values {c.click*100.0f,c.leakage*100.0f,c.drive*100.0f,
                                          c.level*100.0f,c.leslieBalance*100.0f,(float)vibratoType};
        for(size_t i=0;i<6;++i)if(!knobs[i].isMouseButtonDown())knobs[i].setValue(values[i],juce::dontSendNotification);
        learn.setButtonText(translate(c.learning>=0?"CANCELAR":"LEARN CC"));
        const auto selected=(size_t)(target.getSelectedId()-1);
        const auto learned=c.cc[selected]<0?juce::String{}:"CC "+juce::String(c.cc[selected])+" / CH "+juce::String(c.channel[selected]);
        mapping.setText(translate(selected==9?"MOD: CC 1"+(learned.isEmpty()?juce::String{}:" + "+learned):learned.isEmpty()?"Sem CC":learned),juce::dontSendNotification);
        if(lastVibratoType!=vibratoType){lastVibratoType=vibratoType;repaint();}
    }
    juce::String translate(const juce::String& value) const
    { return translateUiText ? translateUiText(value) : value; }
    ClassicPlayerAudioProcessor& processor;int index;
    std::array<juce::Slider,9> bars;
    std::array<juce::Label,9> labels;
    std::array<juce::Slider,6> knobs;
    std::array<juce::Label,6> knobLabels;
    juce::ComboBox preset,target;
    std::array<juce::TextButton,3> leslieButtons;
    std::array<juce::TextButton,2> tabs;
    Rocker vibratoSwitch {"ON","OFF"}, percussionSwitch {"ON","OFF"}, percVolumeSwitch {"SOFT","NORM"},
           percTimeSwitch {"FAST","SLOW"}, percHarmonicSwitch {"3RD","2ND"};
    MidiLearnButton learn {"LEARN CC"};
    juce::TextButton clear {"LIMPAR"};
    juce::Label mapping;
    juce::Rectangle<int> mainPanel, mappingPanel;
    std::array<juce::Rectangle<int>,3> optionsGroups;
    juce::Range<int> vibratoRange, driveRange, leslieRange, percRange;
    int percLabelX = 0, percLabelW = 60, currentTab = 0, vibratoType = 0, lastVibratoType = -1, percHarmonic = 3;
    std::function<juce::String(const juce::String&)> translateUiText;
};
