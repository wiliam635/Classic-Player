#pragma once
#include "PluginProcessor.h"
#include "MidiLearnButton.h"
#include <functional>
#include <utility>

std::unique_ptr<juce::Component> createHammondEditorContent(
    ClassicPlayerAudioProcessor&, int layer,
    std::function<void()> reverbCallback = {},
    std::function<void()> compressorCallback = {},
    std::function<void()> eqCallback = {},
    std::function<void()> savePresetCallback = {},
    std::function<void()> loadPresetCallback = {});

// Instrument controls only: deliberately no on-screen keyboard.
class HammondEditorPanel final : public juce::Component, private juce::Timer
{
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
                button->setTooltip(button==&leslieButtons[i]?"Mod wheel (CC1): valores baixos = lento; valores altos = rápido":"Ataque percussivo do órgão");
                addAndMakeVisible(*button);
            }
            leslieButtons[i].onClick=[this,i]{auto c=processor.hammondConfig(index);if(c.leslie!=(int)i){c.leslie=(int)i;processor.setHammondConfig(index,c);}refresh();};
            percussionButtons[i].onClick=[this,i]{const std::array<int,3> values{0,2,3};auto c=processor.hammondConfig(index);if(c.percussion!=values[i]){c.percussion=values[i];c.preset=-1;processor.setHammondConfig(index,c);}refresh();};
        }
        const std::array<const char*,4> knobNames {"KEY CLICK","LEAKAGE","DRIVE","LEVEL"};
        for(size_t i=0;i<knobs.size();++i){
            knobLabels[i].setText(knobNames[i],juce::dontSendNotification);
            knobLabels[i].setJustificationType(juce::Justification::centred);
            knobLabels[i].setFont(juce::FontOptions(9.0f,juce::Font::bold));addAndMakeVisible(knobLabels[i]);
            knobs[i].setRange(0,100,1);knobs[i].setSliderStyle(juce::Slider::LinearHorizontal);
            knobs[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,52,18);knobs[i].setTextValueSuffix(" %");
            addAndMakeVisible(knobs[i]);
            knobs[i].onValueChange=[this,i]{
                auto c=processor.hammondConfig(index);float* values[]{&c.click,&c.leakage,&c.drive,&c.level};
                const auto value=(float)knobs[i].getValue()/100.0f;
                if(std::abs(*values[i]-value)<0.0001f)return;
                *values[i]=value;if(i!=3)c.preset=-1;processor.setHammondConfig(index,c);
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
        auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);
    }
    void resized() override
    {
        if (getHeight() <= 240)
        {
            preset.setBounds(8,0,getWidth()-16,26);
            const int w=(getWidth()-16)/9;
            for(size_t i=0;i<9;++i){labels[i].setBounds(8+(int)i*w,28,w,16);bars[i].setBounds(8+(int)i*w,44,w,78);}
            const int half=getWidth()/2;
            leslieLabel.setBounds(8,123,half-16,12);percussionLabel.setBounds(half,123,half-8,12);
            const int segmentWidth=(half-16)/3;
            for(int i=0;i<3;++i){leslieButtons[(size_t)i].setBounds(8+i*segmentWidth,135,segmentWidth,18);percussionButtons[(size_t)i].setBounds(half+i*((half-8)/3),135,(half-8)/3,18);}
            const int k=(getWidth()-16)/4;
            for(size_t i=0;i<4;++i){knobLabels[i].setBounds(8+(int)i*k,155,k,12);knobs[i].setBounds(8+(int)i*k,167,k,24);}
            target.setBounds(8,192,180,28);learn.setBounds(196,192,116,28);clear.setBounds(320,192,90,28);
            mapping.setBounds(420,192,getWidth()-428,28);
            return;
        }
        preset.setBounds(8,0,getWidth()-16,28);
        const int w=(getWidth()-16)/9;
        for(size_t i=0;i<9;++i){labels[i].setBounds(8+(int)i*w,32,w,22);bars[i].setBounds(8+(int)i*w,54,w,128);}
        const int half=getWidth()/2;
        leslieLabel.setBounds(8,186,half-16,14);percussionLabel.setBounds(half,186,half-8,14);
        const int segmentWidth=(half-16)/3;
        for(int i=0;i<3;++i){leslieButtons[(size_t)i].setBounds(8+i*segmentWidth,200,segmentWidth,24);percussionButtons[(size_t)i].setBounds(half+i*((half-8)/3),200,(half-8)/3,24);}
        const int k=(getWidth()-16)/4;
        for(size_t i=0;i<4;++i){knobLabels[i].setBounds(8+(int)i*k,227,k,18);knobs[i].setBounds(8+(int)i*k,245,k,46);}
        target.setBounds(8,300,180,28);learn.setBounds(196,300,116,28);clear.setBounds(320,300,90,28);
        mapping.setBounds(420,300,getWidth()-428,28);
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
        const auto compact=getHeight()<=240;
        const auto width=(float)getWidth();
        drawPanel({4.0f,compact?26.0f:30.0f,width-8.0f,compact?97.0f:154.0f});
        drawPanel({4.0f,compact?122.0f:184.0f,width-8.0f,compact?32.0f:44.0f});
        drawPanel({4.0f,compact?154.0f:228.0f,width-8.0f,compact?38.0f:66.0f});
        drawPanel({4.0f,compact?191.0f:294.0f,width-8.0f,compact?28.0f:38.0f});

        const float cell=(width-16.0f)/9.0f;
        const float top=compact?49.0f:63.0f,bottom=compact?114.0f:176.0f;
        for(int i=0;i<9;++i){
            const float x=8.0f+cell*(float)i+cell*0.48f+8.0f;
            for(int step=0;step<=8;++step){
                const float y=bottom-(bottom-top)*(float)step/8.0f;
                g.setColour((step%2==0?accent:border).withAlpha(step%2==0?0.7f:0.65f));
                g.drawHorizontalLine(juce::roundToInt(y),x,x+(step%2==0?5.0f:3.0f));
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
        const std::array<float,4> values {c.click*100.0f,c.leakage*100.0f,c.drive*100.0f,c.level*100.0f};
        for(size_t i=0;i<4;++i)if(!knobs[i].isMouseButtonDown())knobs[i].setValue(values[i],juce::dontSendNotification);
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
    std::array<juce::Slider,4> knobs;
    std::array<juce::Label,4> knobLabels;
    juce::ComboBox preset,target;
    juce::Label leslieLabel,percussionLabel;
    std::array<juce::TextButton,3> leslieButtons,percussionButtons;
    MidiLearnButton learn {"LEARN CC"};
    juce::TextButton clear {"LIMPAR"};
    juce::Label mapping;
    std::function<juce::String(const juce::String&)> translateUiText;
};
