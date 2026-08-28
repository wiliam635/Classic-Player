#pragma once
#include "PluginProcessor.h"

std::unique_ptr<juce::Component> createHammondEditorContent(ClassicPlayerAudioProcessor&, int layer);

// Instrument controls only: deliberately no on-screen keyboard.
class HammondEditorPanel final : public juce::Component, private juce::Timer
{
public:
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
        const std::array<const char*,9> namesOfBars {"16'","5 1/3'","8'","4'","2 2/3'","2'","1 3/5'","1 1/3'","1'"};
        for(size_t i=0;i<bars.size();++i){
            labels[i].setText(namesOfBars[i],juce::dontSendNotification);
            labels[i].setJustificationType(juce::Justification::centred);addAndMakeVisible(labels[i]);
            bars[i].setSliderStyle(juce::Slider::LinearVertical);bars[i].setRange(0,8,1);
            bars[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,48,22);addAndMakeVisible(bars[i]);
            bars[i].onValueChange=[this,i]{auto c=processor.hammondConfig(index);const auto value=(int)bars[i].getValue();if(c.bars[i]==value)return;c.bars[i]=value;c.preset=-1;processor.setHammondConfig(index,c);};
            target.addItem("Drawbar "+juce::String(namesOfBars[i]),(int)i+1);
        }
        leslie.addItem("LESLIE STOP",1);leslie.addItem("LESLIE CHORALE",2);leslie.addItem("LESLIE FAST",3);
        percussion.addItem("PERC OFF",1);percussion.addItem("PERC 2ND",2);percussion.addItem("PERC 3RD",3);
        addAndMakeVisible(leslie);addAndMakeVisible(percussion);
        leslie.setTooltip("Mod wheel (CC1): 0-63 lento, 64-127 rapido");
        leslie.onChange=[this]{auto c=processor.hammondConfig(index);const auto value=leslie.getSelectedId()-1;if(c.leslie==value)return;c.leslie=value;processor.setHammondConfig(index,c);};
        percussion.onChange=[this]{auto c=processor.hammondConfig(index);const auto value=percussion.getSelectedId()==1?0:percussion.getSelectedId();if(c.percussion==value)return;c.percussion=value;c.preset=-1;processor.setHammondConfig(index,c);};
        const std::array<const char*,4> knobNames {"KEY CLICK","LEAKAGE","DRIVE","LEVEL"};
        for(size_t i=0;i<knobs.size();++i){
            knobLabels[i].setText(knobNames[i],juce::dontSendNotification);
            knobLabels[i].setJustificationType(juce::Justification::centred);addAndMakeVisible(knobLabels[i]);
            knobs[i].setRange(0,1,.01);knobs[i].setSliderStyle(juce::Slider::LinearHorizontal);
            knobs[i].setTextBoxStyle(juce::Slider::TextBoxBelow,false,52,18);addAndMakeVisible(knobs[i]);
            knobs[i].onValueChange=[this,i]{
                auto c=processor.hammondConfig(index);float* values[]{&c.click,&c.leakage,&c.drive,&c.level};
                const auto value=(float)knobs[i].getValue();
                if(std::abs(*values[i]-value)<0.0001f)return;
                *values[i]=value;if(i!=3)c.preset=-1;processor.setHammondConfig(index,c);
            };
        }
        target.addItem("Leslie / Mod wheel",10);target.addItem("Level",11);target.setSelectedId(1,juce::dontSendNotification);
        for(auto* component:std::initializer_list<juce::Component*>{&target,&learn,&clear,&mapping})addAndMakeVisible(component);
        target.onChange=[this]{auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);refresh();};
        learn.onClick=[this]{auto c=processor.hammondConfig(index);c.learning=c.learning>=0?-1:target.getSelectedId()-1;processor.setHammondConfig(index,c);refresh();};
        clear.onClick=[this]{auto c=processor.hammondConfig(index);c.cc[(size_t)(target.getSelectedId()-1)]=-1;c.learning=-1;processor.setHammondConfig(index,c);refresh();};
        setSize(680,344);refresh();startTimerHz(15);
    }
    ~HammondEditorPanel() override {
        stopTimer();
        auto c=processor.hammondConfig(index);c.learning=-1;processor.setHammondConfig(index,c);
    }
    void resized() override
    {
        preset.setBounds(8,0,getWidth()-16,28);
        const int w=(getWidth()-16)/9;
        for(size_t i=0;i<9;++i){labels[i].setBounds(8+(int)i*w,32,w,22);bars[i].setBounds(8+(int)i*w,54,w,128);}
        leslie.setBounds(8,188,getWidth()/2-16,28);percussion.setBounds(getWidth()/2,188,getWidth()/2-8,28);
        const int k=(getWidth()-16)/4;
        for(size_t i=0;i<4;++i){knobLabels[i].setBounds(8+(int)i*k,224,k,18);knobs[i].setBounds(8+(int)i*k,242,k,48);}
        target.setBounds(8,300,180,28);learn.setBounds(196,300,116,28);clear.setBounds(320,300,90,28);
        mapping.setBounds(420,300,getWidth()-428,28);
    }
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff151f28)); }
private:
    void timerCallback() override { refresh(); }
    void refresh()
    {
        const auto c=processor.hammondConfig(index);
        preset.setSelectedId(c.preset+2,juce::dontSendNotification);
        for(size_t i=0;i<9;++i)if(!bars[i].isMouseButtonDown())bars[i].setValue(c.bars[i],juce::dontSendNotification);
        leslie.setSelectedId(c.leslie+1,juce::dontSendNotification);
        percussion.setSelectedId(c.percussion==0?1:c.percussion,juce::dontSendNotification);
        const std::array<float,4> values {c.click,c.leakage,c.drive,c.level};
        for(size_t i=0;i<4;++i)if(!knobs[i].isMouseButtonDown())knobs[i].setValue(values[i],juce::dontSendNotification);
        learn.setButtonText(c.learning>=0?"CANCELAR":"LEARN CC");
        const auto selected=(size_t)(target.getSelectedId()-1);
        const auto learned=c.cc[selected]<0?juce::String{}:"CC "+juce::String(c.cc[selected])+" / CH "+juce::String(c.channel[selected]);
        mapping.setText(selected==9?"MOD: CC 1"+(learned.isEmpty()?juce::String{}:" + "+learned):learned.isEmpty()?"Sem CC":learned,juce::dontSendNotification);
    }
    ClassicPlayerAudioProcessor& processor;int index;
    std::array<juce::Slider,9> bars;
    std::array<juce::Label,9> labels;
    std::array<juce::Slider,4> knobs;
    std::array<juce::Label,4> knobLabels;
    juce::ComboBox preset,leslie,percussion,target;
    juce::TextButton learn {"LEARN CC"},clear {"LIMPAR"};
    juce::Label mapping;
};
