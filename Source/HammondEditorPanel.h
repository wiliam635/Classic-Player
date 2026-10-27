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
            const auto centre = (float) x + (float) width * 0.5f;
            g.setColour(juce::Colours::black);
            g.fillRoundedRectangle(centre - 4.0f, (float)y + 2.0f, 8.0f,
                                   juce::jmax(1.0f, (float)height - 4.0f), 4.0f);
            const auto index = juce::jlimit(0, 2, (int) slider.getProperties()["drawbarColour"]);
            const auto thumbWidth = juce::jmin(31.0f, (float)width * 0.65f);
            const auto thumbHeight = juce::jmin(37.0f, (float)height * 0.36f);
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
            const auto old=processor.hammondConfig(index);
            auto c=HammondEngine::preset(preset.getSelectedId()-2);
            c.routing=old.routing;c.cc=old.cc;c.channel=old.channel;c.level=old.level;
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
            bars[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,42,18);
            addAndMakeVisible(bars[i]);
            bars[i].onValueChange=[this,i]{auto c=processor.hammondConfig(index);const auto value=(int)bars[i].getValue();if(c.bars[i]==value)return;c.bars[i]=value;c.preset=-1;processor.setHammondConfig(index,c);};
            target.addItem("Drawbar "+juce::String(namesOfBars[i]),(int)i+1);
        }
        leslieLabel.setText("LESLIE ROTOR",juce::dontSendNotification);
        percussionLabel.setText("PERCUSSION",juce::dontSendNotification);
        for(auto* label:{&leslieLabel,&percussionLabel}){
            label->setJustificationType(juce::Justification::centredLeft);
            label->setFont(juce::FontOptions(9.0f,juce::Font::bold));addAndMakeVisible(*label);
        }
        const std::array<const char*,3> leslieNames {"STOP","CHORALE","FAST"};
        const std::array<const char*,3> percussionNames {"OFF","2ND","3RD"};
        for(size_t i=0;i<3;++i){
            leslieButtons[i].setButtonText(leslieNames[i]);
            percussionButtons[i].setButtonText(percussionNames[i]);
            for(auto* button:{&leslieButtons[i],&percussionButtons[i]}){
                button->setClickingTogglesState(true);
                button->setRadioGroupId(button==&leslieButtons[i]?8301:8302);
                button->setConnectedEdges((i>0?juce::Button::ConnectedOnLeft:0)|(i<2?juce::Button::ConnectedOnRight:0));
                button->setTooltip(juce::String::fromUTF8(button==&leslieButtons[i]?"Mod wheel (CC1): valores baixos = lento; valores altos = rápido":"Ataque percussivo do órgão"));
                addAndMakeVisible(*button);
            }
            leslieButtons[i].onClick=[this,i]{auto c=processor.hammondConfig(index);if(c.leslie!=(int)i){c.leslie=(int)i;processor.setHammondConfig(index,c);}refresh();};
            percussionButtons[i].onClick=[this,i]{const std::array<int,3> values{0,2,3};auto c=processor.hammondConfig(index);if(c.percussion!=values[i]){c.percussion=values[i];c.preset=-1;processor.setHammondConfig(index,c);}refresh();};
        }
        const std::array<const char*,5> knobNames {"KEY CLICK","LEAKAGE","DRIVE","LEVEL","CHORUS"};
        for(size_t i=0;i<knobs.size();++i){
            knobLabels[i].setText(knobNames[i],juce::dontSendNotification);
            knobLabels[i].setJustificationType(juce::Justification::centred);
            knobLabels[i].setFont(juce::FontOptions(9.0f,juce::Font::bold));addAndMakeVisible(knobLabels[i]);
            knobs[i].setRange(0,100,1);knobs[i].setSliderStyle(juce::Slider::LinearHorizontal);
            knobs[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,52,18);knobs[i].setTextValueSuffix(" %");
            addAndMakeVisible(knobs[i]);
            if (i == 4) { knobLabels[i].setVisible(false); knobs[i].setVisible(false); }
            knobs[i].onValueChange=[this,i]{
                if(i==4){
                    const auto parameterId="layer"+juce::String(index+1)+"Sf2Chorus";
                    if(auto* parameter=processor.parameters.getParameter(parameterId))
                        parameter->setValueNotifyingHost(parameter->convertTo0to1(
                            (float)knobs[i].getValue()));
                    return;
                }
                auto c=processor.hammondConfig(index);
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
        setSize(680,344);applyThemeColours();refresh();startTimerHz(15);
    }
    ~HammondEditorPanel() override {
        stopTimer();
        for (auto& bar : bars) bar.setLookAndFeel(nullptr);
        auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);
    }
    void resized() override
    {
        const int width = getWidth();
        const int height = getHeight();
        const int drawbarBottom = juce::jmax(145, height - 99);
        preset.setBounds(width / 6, 0, width * 2 / 3, 25);
        const int cell = (width - 16) / 9;
        for (size_t i = 0; i < bars.size(); ++i)
        {
            const int x = 8 + (int)i * cell;
            labels[i].setBounds(x, 28, cell, 17);
            bars[i].setBounds(x + 4, 45, cell - 8, drawbarBottom - 43);
        }
        const int half = width / 2;
        leslieLabel.setBounds(10, drawbarBottom + 1, half - 20, 12);
        percussionLabel.setBounds(half + 2, drawbarBottom + 1, half - 14, 12);
        for (int i = 0; i < 3; ++i)
        {
            leslieButtons[(size_t)i].setBounds(10 + i * (half - 16) / 3,
                                               drawbarBottom + 13, (half - 20) / 3, 20);
            percussionButtons[(size_t)i].setBounds(half + 2 + i * (half - 12) / 3,
                                                   drawbarBottom + 13, (half - 16) / 3, 20);
        }
        const int quarter = (width - 16) / 4;
        const int slidersTop = drawbarBottom + 34;
        for (size_t i = 0; i < 4; ++i)
        {
            const int x = 8 + (int)i * quarter;
            knobLabels[i].setBounds(x, slidersTop, quarter - 4, 12);
            knobs[i].setTextBoxStyle(juce::Slider::TextBoxRight, false, 43, 20);
            knobs[i].setBounds(x + 4, slidersTop + 11, quarter - 10, 24);
        }
        const int mappingTop = height - 23;
        target.setBounds(8, mappingTop, quarter - 4, 22);
        learn.setBounds(8 + quarter, mappingTop, quarter - 4, 22);
        clear.setBounds(8 + 2 * quarter, mappingTop, quarter - 4, 22);
        mapping.setBounds(8 + 3 * quarter, mappingTop, quarter - 4, 22);
    }
    void lookAndFeelChanged() override
    {
        juce::Component::lookAndFeelChanged();
        applyThemeColours();repaint();
    }
    void paint(juce::Graphics& g) override
    {
        const auto surface=getLookAndFeel().findColour(juce::ComboBox::backgroundColourId);
        const auto border=getLookAndFeel().findColour(juce::ComboBox::outlineColourId);
        const auto accent=getLookAndFeel().findColour(juce::ComboBox::arrowColourId);
        g.fillAll(surface.darker(0.75f));
        const auto drawPanel=[&g,&surface,&border,&accent](juce::Rectangle<float> bounds)
        {
            g.setGradientFill(juce::ColourGradient(surface.brighter(0.08f),bounds.getX(),bounds.getY(),
                                                   surface.darker(0.24f),bounds.getX(),bounds.getBottom(),false));
            g.fillRoundedRectangle(bounds,5.0f);
            g.setColour(border);g.drawRoundedRectangle(bounds,5.0f,1.0f);
            g.setColour(accent.withAlpha(0.65f));g.fillRect(bounds.getX()+5.0f,bounds.getY()+2.0f,bounds.getWidth()-10.0f,1.5f);
        };
        const auto width=(float)getWidth();
        const float drawbarBottom = (float)juce::jmax(145, getHeight() - 99);
        drawPanel({4.0f,26.0f,width-8.0f,drawbarBottom-26.0f});
        drawPanel({4.0f,drawbarBottom,width*0.5f-8.0f,34.0f});
        drawPanel({width*0.5f,drawbarBottom,width*0.5f-4.0f,34.0f});
        drawPanel({4.0f,drawbarBottom+34.0f,width-8.0f,40.0f});
        drawPanel({4.0f,(float)getHeight()-24.0f,width-8.0f,24.0f});

        const float cell=(width-16.0f)/9.0f;
        const float top=52.0f,bottom=drawbarBottom-28.0f;
        for(int i=0;i<9;++i){
            const float x=8.0f+cell*(float)i+cell*0.48f+8.0f;
            for(int step=0;step<=8;++step){
                const float y=bottom-(bottom-top)*(float)step/8.0f;
                g.setColour((step%2==0?accent:border).withAlpha(step%2==0?0.7f:0.65f));
                g.drawHorizontalLine(juce::roundToInt(y),x,x+(step%2==0?5.0f:3.0f));
                if (cell >= 75.0f) {
                    g.setColour(getLookAndFeel().findColour(juce::ComboBox::textColourId));
                    g.setFont(juce::FontOptions(10.0f));
                    g.drawText(juce::String(step), juce::roundToInt(x + 6.0f),
                               juce::roundToInt(y - 5.0f), 12, 10, juce::Justification::centredLeft);
                }
            }
        }
    }
private:
    void applyThemeColours()
    {
        const auto& look=getLookAndFeel();
        const auto surface=look.findColour(juce::ComboBox::backgroundColourId);
        const auto border=look.findColour(juce::ComboBox::outlineColourId);
        const auto text=look.findColour(juce::ComboBox::textColourId);
        const auto accent=look.findColour(juce::ComboBox::arrowColourId);
        const auto inset=surface.darker(0.72f);
        for(auto* selector:{&preset,&target}){
            selector->setColour(juce::ComboBox::backgroundColourId,surface);
            selector->setColour(juce::ComboBox::outlineColourId,border);
            selector->setColour(juce::ComboBox::textColourId,text);
            selector->setColour(juce::ComboBox::arrowColourId,accent);
        }
        for(auto& label:labels)label.setColour(juce::Label::textColourId,text);
        for(auto* label:{&leslieLabel,&percussionLabel})label->setColour(juce::Label::textColourId,text.withAlpha(0.78f));
        for(auto& label:knobLabels)label.setColour(juce::Label::textColourId,text.withAlpha(0.78f));
        for(auto& bar:bars){
            bar.setColour(juce::Slider::trackColourId,accent);
            bar.setColour(juce::Slider::thumbColourId,juce::Colour(0xffd8dde0));
            bar.setColour(juce::Slider::textBoxTextColourId,text);
            bar.setColour(juce::Slider::textBoxBackgroundColourId,inset);
            bar.setColour(juce::Slider::textBoxOutlineColourId,border);
        }
        for(auto& knob:knobs){
            knob.setColour(juce::Slider::trackColourId,accent);
            knob.setColour(juce::Slider::thumbColourId,juce::Colour(0xffd8dde0));
            knob.setColour(juce::Slider::textBoxTextColourId,text);
            knob.setColour(juce::Slider::textBoxBackgroundColourId,inset);
            knob.setColour(juce::Slider::textBoxOutlineColourId,border);
        }
        for(auto* group:{&leslieButtons,&percussionButtons})for(auto& button:*group){
            button.setColour(juce::TextButton::buttonColourId,surface);
            button.setColour(juce::TextButton::buttonOnColourId,accent);
            button.setColour(juce::TextButton::textColourOffId,text);
            button.setColour(juce::TextButton::textColourOnId,accent.contrasting());
        }
    }
    void timerCallback() override { refresh(); }
    void refresh()
    {
        const auto c=processor.hammondConfig(index);
        preset.setSelectedId(c.preset+2,juce::dontSendNotification);
        for(size_t i=0;i<9;++i)if(!bars[i].isMouseButtonDown())bars[i].setValue(c.bars[i],juce::dontSendNotification);
        for(int i=0;i<3;++i)leslieButtons[(size_t)i].setToggleState(c.leslie==i,juce::dontSendNotification);
        const std::array<int,3> percussionValues{0,2,3};
        for(int i=0;i<3;++i)percussionButtons[(size_t)i].setToggleState(c.percussion==percussionValues[(size_t)i],juce::dontSendNotification);
        const auto chorusId="layer"+juce::String(index+1)+"Sf2Chorus";
        const auto* chorus=processor.parameters.getRawParameterValue(chorusId);
        const std::array<float,5> values {c.click*100.0f,c.leakage*100.0f,c.drive*100.0f,
                                          c.level*100.0f,chorus!=nullptr?chorus->load():0.0f};
        for(size_t i=0;i<5;++i)if(!knobs[i].isMouseButtonDown())knobs[i].setValue(values[i],juce::dontSendNotification);
        learn.setButtonText(translate(c.learning>=0?"CANCELAR":"LEARN CC"));
        const auto selected=(size_t)(target.getSelectedId()-1);
        const auto learned=c.cc[selected]<0?juce::String{}:"CC "+juce::String(c.cc[selected])+" / CH "+juce::String(c.channel[selected]);
        mapping.setText(translate(selected==9?"MOD: CC 1"+(learned.isEmpty()?juce::String{}:" + "+learned):learned.isEmpty()?"Sem CC":learned),juce::dontSendNotification);
    }
    juce::String translate(const juce::String& value) const
    { return translateUiText ? translateUiText(value) : value; }
    ClassicPlayerAudioProcessor& processor;int index;
    std::array<juce::Slider,9> bars;
    std::array<juce::Label,9> labels;
    std::array<juce::Slider,5> knobs;
    std::array<juce::Label,5> knobLabels;
    juce::ComboBox preset,target;
    juce::Label leslieLabel,percussionLabel;
    std::array<juce::TextButton,3> leslieButtons,percussionButtons;
    MidiLearnButton learn {"LEARN CC"};
    juce::TextButton clear {"LIMPAR"};
    juce::Label mapping;
    std::function<juce::String(const juce::String&)> translateUiText;
};
