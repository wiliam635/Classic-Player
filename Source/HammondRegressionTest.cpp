#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "HammondEditorPanel.h"
#include "AnalogEditorPanel.h"
#include "AnalogBrowserPresets.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
static void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static void continuousPadRegression()
{
    juce::TemporaryFile fixture(".wav");
    {
        juce::WavAudioFormat format;
        auto stream=fixture.getFile().createOutputStream();check(stream!=nullptr,"pad WAV stream");
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.release(),44100,1,24,{},0));
        check(writer!=nullptr,"pad WAV writer");juce::AudioBuffer<float> source(1,4410);
        for(int i=0;i<source.getNumSamples();++i)source.setSample(0,i,.25f*(float)i/4410.f);
        check(writer->writeFromAudioSampleBuffer(source,0,source.getNumSamples()),"pad WAV write");
    }
    ContinuousPadBank bank;bank.prepare(48000);bank.setFadeSeconds(.02);
    check(bank.load(0,fixture.getFile()).wasOk(),"pad load");
    check(bank.load(1,fixture.getFile()).wasOk(),"second pad load");
    juce::AudioBuffer<float> audio(2,128);
    auto block=[&]{audio.clear();bank.render(audio,1.f);};
    block();check(audio.getMagnitude(0,128)==0,"pad autoplays after load");
    bank.trigger(0);float previous=0,maxJump=0;
    for(int b=0;b<400;++b)
    {
        if(b==100)bank.trigger(0);
        block();for(int i=0;i<128;++i){const auto value=audio.getSample(0,i);maxJump=juce::jmax(maxJump,std::abs(value-previous));previous=value;}
    }
    check(maxJump<.001f,"loop seam or repeated trigger discontinuity");
    check(bank.peak()>0,"continuous meter silent");
    bank.trigger(1);block();check(bank.selected()==1,"pad selection did not switch");
    bank.stop();for(int i=0;i<12;++i)block();check(audio.getMagnitude(0,128)<1e-6,"STOP did not fade to silence");
    bank.learn(0);bank.midi(juce::MidiMessage::controllerEvent(10,64,127));check(bank.learningTarget()==0,"pad learned sustain");
    bank.midi(juce::MidiMessage::controllerEvent(10,21,127));check(bank.mapping(0)==21&&bank.selected()==-1,"learn starts playback");
    bank.midi(juce::MidiMessage::controllerEvent(10,21,0));block();check(bank.selected()==-1,"CC release triggered pad");
    bank.midi(juce::MidiMessage::controllerEvent(10,21,127));block();check(bank.selected()==0,"CC press did not trigger pad");
    const auto saved=bank.save();bank.restore(saved);block();check(bank.selected()==-1&&audio.getMagnitude(0,128)==0,"restore autoplays");
    check(bank.mapping(0)==21&&bank.path(0)==fixture.getFile().getFullPathName(),"pad settings lost");
    auto p=std::make_unique<ClassicPlayerAudioProcessor>();
    p->setLayerType(1,ClassicPlayerAudioProcessor::LayerType::continuousPads);p->continuousPads(1).restore(saved);
    juce::MemoryBlock state;p->getStateInformation(state);p->setStateInformation(state.getData(),(int)state.getSize());
    check(p->layerType(1)==ClassicPlayerAudioProcessor::LayerType::continuousPads&&p->continuousPads(1).mapping(0)==21,"program lost continuous pads");
    p->removeLayer(0);check(p->layerType(0)==ClassicPlayerAudioProcessor::LayerType::continuousPads&&p->continuousPads(0).mapping(0)==21,"removal lost pad bank");
    std::cout<<"Continuous pads: looping, resampling, crossfade, STOP, MIDI, persistence and layer move passed\n";
}
struct LiveSetLayoutRegressionAccess
{
    static void run(const char* screenshot)
    {
        juce::TemporaryFile storage;
        const auto root=storage.getFile();
        check(root.createDirectory().wasOk(),"Live Set temporary storage");
        struct Cleanup { juce::File directory; ~Cleanup() { directory.deleteRecursively(); } } cleanup { root };
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        auto processor = std::make_unique<ClassicPlayerAudioProcessor>(root);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        while (processor->activeLayerCount()>2)
            check(processor->removeLayer(processor->activeLayerCount()-1),"Live Set fixture layers");
        processor->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::hammond);
        processor->setLayerType(1,ClassicPlayerAudioProcessor::LayerType::analog);
        juce::File program;
        check(processor->saveProgram("Test instruments",program).wasOk(),"Live Set fixture program");
        check(processor->assignLiveSetSlot(0,0,program).wasOk(),"Live Set fixture assignment");
        check(processor->liveSetSlotLayerSummary(0,0)=="2 CAMADAS","built-in instruments missing from layer count");
        auto editor = std::make_unique<ClassicPlayerAudioProcessorEditor>(*processor);
        editor->showLiveSet(true);
        editor->activationPanel.setVisible(false);
        for (auto size : { juce::Point<int>(1280,700), juce::Point<int>(1000,600) })
        {
            editor->setSize(size.x,size.y);
            editor->showLiveSet(false);
            check(editor->audioMidiSettingsButton.isVisible(),"standalone audio settings button hidden");
            check(editor->audioMidiSettingsButton.getWidth()>=120
                &&editor->audioMidiSettingsButton.getHeight()>=24,"audio settings button collapsed");
            check(editor->getLocalBounds().contains(editor->audioMidiSettingsButton.getBounds()),"audio settings button clipped");
            check(!editor->audioMidiSettingsButton.getBounds().intersects(editor->keyboardVisibilityButton.getBounds()),
                  "audio settings overlaps keyboard toggle");
            editor->showLiveSet(true);
            check(editor->liveSettingsButton.isVisible(),"Live Set settings entry hidden");
            check(!editor->audioMidiSettingsButton.isVisible(),"mixer audio button overlaps Live Set");
            for (size_t i=0;i<editor->liveSetSlotButtons.size();++i)
            {
                const auto bounds=editor->liveSetSlotButtons[i].getBounds();
                check(editor->getLocalBounds().contains(bounds),"Live Set card clipped");
                check(bounds.getWidth()>200 && bounds.getHeight()>150,"Live Set card too small");
                check(!editor->liveSetSlotLearnButtons[i].isVisible(),"Learn visible outside edit mode");
                for (size_t j=i+1;j<editor->liveSetSlotButtons.size();++j)
                    check(!bounds.intersects(editor->liveSetSlotButtons[j].getBounds()),"Live Set cards overlap");
            }
        }
        editor->liveSetSlotButtons[0].onClick();
        check(processor->activeLayerCount()==2,"Live Set load changed layer count");
        check(processor->layerType(0)==ClassicPlayerAudioProcessor::LayerType::hammond,
              "Live Set load changed instrument");
        check((bool)editor->liveSetSlotButtons[0].getProperties()["liveActive"],"loaded card not highlighted");
        editor->liveNextButton.onClick();
        check(!(bool)editor->liveSetSlotButtons[0].getProperties()["liveActive"],"wrong bank highlighted");
        editor->livePreviousButton.onClick();
        check((bool)editor->liveSetSlotButtons[0].getProperties()["liveActive"],"loaded card lost on bank return");
        editor->editLiveSetButton.onClick();
        check(editor->liveSetSlotLearnButtons[0].isVisible(),"edit MIDI Learn hidden");
        editor->editLiveSetButton.onClick();
        editor->showLiveSet(false);
        check(editor->master.getSliderStyle()==juce::Slider::RotaryHorizontalVerticalDrag,"mixer master style not restored");
        editor->showLiveSet(true);
        if (screenshot != nullptr)
        {
            editor->setSize(1280,700);
            const char* names[]={"Piano + Pad","Worship Atmosphere","EP + Strings","Organ Leslie",
                                 "Piano Solo","Brass Layer","Synth Lead","Guitar + Pad"};
            for (size_t i=0;i<8;++i)
            {
                auto& props=editor->liveSetSlotButtons[i].getProperties();
                props.set("liveTitle",names[i]);
                props.set("liveSummary",i==4||i==6?"1 CAMADA":i==1?"3 CAMADAS":"2 CAMADAS");
            }
            const auto image=editor->createComponentSnapshot(editor->getLocalBounds());
            juce::FileOutputStream stream{juce::File(screenshot)};
            juce::PNGImageFormat png;
            check(stream.openedOk()&&png.writeImageToStream(image,stream),"Live Set screenshot");
            editor->showLiveSet(false);
            juce::FileOutputStream mixerStream{juce::File(juce::String(screenshot)+".mixer.png")};
            check(mixerStream.openedOk()&&png.writeImageToStream(
                editor->createComponentSnapshot(editor->getLocalBounds()),mixerStream),"mixer audio settings screenshot");
        }
        editor->showLiveSet(false);
        processor->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::drumPads);
        processor->setLayerType(1,ClassicPlayerAudioProcessor::LayerType::continuousPads);
        for(int i=0;i<2;++i)editor->strips[(size_t)i]->refresh();
        editor->setSize(1280,700);editor->layoutLayerStrips();
        check(editor->strips[0]->getWidth()>=420&&editor->strips[1]->getWidth()>=420,"pad strips not widened");
        for(int layer=0;layer<2;++layer)
        {
            int visiblePads=0;
            std::function<void(juce::Component&)> inspect=[&](juce::Component& component)
            {
                for(auto* child:component.getChildren())if(child->isVisible())
                {
                    check(component.getLocalBounds().contains(child->getBounds()),"pad control outside layer");
                    if(child->getName().startsWith("DRUM_PAD_")){++visiblePads;check(child->getWidth()>=50&&child->getHeight()>=30,"pad too small");}
                    if(auto* button=dynamic_cast<juce::TextButton*>(child))
                        check(button->getButtonText()!="LOAD"&&button->getButtonText()!="LEARN","configuration button visible in mixer pad layer");
                    inspect(*child);
                }
            };
            inspect(*editor->strips[(size_t)layer]);check(visiblePads==(layer==0?8:12),"wrong visible pad count");
        }
        if(screenshot)
        {juce::FileOutputStream out{juce::File(juce::String(screenshot)+".pads.png")};juce::PNGImageFormat png;
         check(png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()),out),"pad screenshot");}
        std::cout<<"Live Set and 1280x700 pad layout passed\n";
    }
};
using Configs=std::array<HammondEngine::Config,HammondEngine::layerCount>;
static Configs configs(){
    Configs c;for(auto& x:c)x.routing.enabled=false;
    c[0].routing.enabled=true;c[0].routing.reverb=0;c[0].leslie=0;
    c[0].click=c[0].leakage=c[0].drive=0;return c;
}
static std::vector<float> render(int block,double rate,bool routed,bool allBars=false)
{
    auto engine=std::make_unique<HammondEngine>();engine->prepare(rate,block);
    auto c=configs();c[0].bars={0,0,8,0,0,0,0,0,0};if(allBars)c[0].bars.fill(8);
    std::vector<float> result;
    for(int start=0;start<16000;start+=block){
        const int count=std::min(block,16000-start);
        juce::AudioBuffer<float> out(2,count);out.clear();juce::MidiBuffer midi;
        const auto add=[&](int at,juce::MidiMessage m){if(at>=start&&at<start+count)midi.addEvent(m,at-start);};
        add(73,juce::MidiMessage::noteOn(1,71,.8f));add(73,juce::MidiMessage::noteOn(1,72,.8f));
        add(4049,juce::MidiMessage::noteOff(1,71));add(8051,juce::MidiMessage::noteOff(1,72));
        std::array<juce::MidiBuffer,HammondEngine::layerCount> local;
        juce::MidiBuffer empty;
        if(routed)local[0].swapWith(midi);
        engine->process(out,routed?empty:midi,c,routed?&local:nullptr);
        result.insert(result.end(),out.getReadPointer(0),out.getReadPointer(0)+count);
    }
    return result;
}
static void nativeWindowStyle()
{
    auto processor = std::make_unique<ClassicPlayerAudioProcessor>();
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
    const auto flags = editor->getLookAndFeel().getAlertBoxWindowFlags();
    check((flags & juce::ComponentPeer::windowHasTitleBar) != 0, "missing native dialog title bar");
    check((flags & juce::ComponentPeer::windowHasCloseButton) != 0, "missing native dialog close button");
    // AlertWindow's layout dereferences the primary display. Headless CI and
    // sandboxed macOS tests can have no display; still verify the style flags.
    if (!juce::Desktop::getInstance().getDisplays().displays.isEmpty())
    {
        juce::AlertWindow dialog("Window style test", {}, juce::MessageBoxIconType::NoIcon);
        dialog.setLookAndFeel(&editor->getLookAndFeel());
        const auto native = dialog.isUsingNativeTitleBar();
        dialog.setLookAndFeel(nullptr);
        check(native, "dialog did not adopt native title bar");
    }
    else
        std::cout << "No display: native dialog instantiation skipped\n";
    std::cout << "Native dialog title bar and close flags passed\n";
}

static void analogEditorLifecycle()
{
    auto processor=std::make_unique<ClassicPlayerAudioProcessor>();
    processor->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::analog);
    const auto parameter=[&](const char* suffix) {
        return processor->parameters.getParameter("layer1"+juce::String(suffix));
    };
    const auto read=[&](const char* suffix) {
        return processor->parameters.getRawParameterValue("layer1"+juce::String(suffix))->load();
    };
    const auto slider=[](juce::Component& panel,int index) {
        int count=0;
        for(int i=0;i<panel.getNumChildComponents();++i)
            if(auto* s=dynamic_cast<juce::Slider*>(panel.getChildComponent(i)))
                if(count++==index)return s;
        throw std::runtime_error("Analog slider missing");
    };
    for(size_t preset:{size_t(0),size_t(10),size_t(18),size_t(31)})
    {
        parameter("Cutoff")->setValueNotifyingHost(parameter("Cutoff")->convertTo0to1(100));
        auto panel=createAnalogCommonControls(*processor,0);
        const auto selected=AnalogBrowserPresets::config(preset,Sf2Engine::LayerConfig{});
        processor->setAnalogSynthConfig(0,selected);
        check(std::abs(slider(*panel,1)->getValue()-selected.cutoff)<0.0001,
              "preset did not update visible cutoff");
        slider(*panel,0)->setValue(37,juce::sendNotificationSync);
        check(std::abs(read("Cutoff")-selected.cutoff)<0.0001,
              "volume change restored stale cutoff");
        parameter("Gain")->setValueNotifyingHost(parameter("Gain")->convertTo0to1(61));
        parameter("Reverb")->setValueNotifyingHost(parameter("Reverb")->convertTo0to1(29));
        check(std::abs(slider(*panel,0)->getValue()-61)<0.001,"MIDI/host value not reflected");
        panel.reset();
        check(std::abs(read("Cutoff")-selected.cutoff)<0.0001,"closing changed preset cutoff");
        check(std::abs(read("Gain")-61)<0.001&&std::abs(read("Reverb")-29)<0.001,
              "closing restored stale mix values");
        panel=createAnalogCommonControls(*processor,0);
        check(std::abs(slider(*panel,1)->getValue()-selected.cutoff)<0.0001,
              "reopening rounded or reset cutoff");
        check(processor->analogSynthConfig(0).browserCompatible,"closing changed synth mode");
    }
    std::cout<<"Analog editor preset/volume/host updates and close/reopen lifecycle passed\n";
}

struct DrumPadRegressionAccess
{
    static void run()
    {
        auto p=std::make_unique<ClassicPlayerAudioProcessor>();
        p->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::drumPads);
        p->prepareToPlay(48000,128);
        auto& pad=p->drumPads[0];pad.audio.setSize(2,48000);
        for(int ch=0;ch<2;++ch)for(int i=0;i<48000;++i)pad.audio.setSample(ch,i,.25f);
        pad.position.store(0);
        const auto gain=[&](float value){auto* g=p->parameters.getParameter("layer1Gain");g->setValueNotifyingHost(g->convertTo0to1(value));};
        const auto block=[&]{
            juce::AudioBuffer<float> b(2,128);b.clear();juce::MidiBuffer m;
            p->processDrumPads(b,m);return b.getSample(0,127);
        };
        gain(100);check(std::abs(block()-.25f)<1e-6,"drum unity gain");
        check(std::abs(p->layerPeak(0)-.25f)<1e-6,"drum meter unity gain");
        gain(50);const auto transition=block();check(transition>.125f&&transition<.25f,"drum gain not smoothed");
        for(int i=0;i<10;++i)block();
        check(std::abs(block()-.125f)<1e-6,"drum half gain");
        check(std::abs(p->layerPeak(0)-.125f)<1e-6,"drum meter ignores layer gain");
        gain(0);for(int i=0;i<10;++i)block();check(std::abs(block())<1e-6,"drum mute gain");
        check(p->layerPeak(0)<1e-6,"muted drum meter not silent");
        gain(50);auto config=p->layerConfig(0);config.enabled=false;p->setLayerConfig(0,config);
        for(int i=0;i<10;++i)block();check(std::abs(block())<1e-6,"drum layer mute ignored");
        config.enabled=true;p->setLayerConfig(0,config);for(int i=0;i<10;++i)block();
        check(std::abs(block()-.125f)<1e-6,"drum layer unmute failed");
        juce::MemoryBlock state;p->getStateInformation(state);
        p->setStateInformation(state.getData(),(int)state.getSize());
        check(std::abs(p->parameters.getRawParameterValue("layer1Gain")->load()-50)<1e-6,"drum gain persistence");
        pad.audio.setSize(2,48000);
        for(int ch=0;ch<2;++ch)for(int i=0;i<48000;++i)pad.audio.setSample(ch,i,.25f);
        const auto sendCC=[&](int cc,int value,int channel=10){
            juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::controllerEvent(channel,cc,value),0);p->processBlock(audio,midi);
        };
        pad.midiCC.store(-1);pad.trigger.store(0);p->beginDrumPadMidiLearn(0);
        sendCC(64,127);
        check(p->isDrumPadMidiLearning(0)&&p->drumPadMidiCC(0)<0,"sustain captured by drum learn");
        sendCC(123,0);
        check(p->isDrumPadMidiLearning(0)&&p->drumPadMidiCC(0)<0,"channel mode captured by drum learn");
        sendCC(73,127);
        check(!p->isDrumPadMidiLearning(0)&&p->drumPadMidiCC(0)==73,"valid drum CC not learned");
        pad.position.store(-1);sendCC(64,127);check(!p->isDrumPadPlaying(0),"sustain triggered drum pad");
        sendCC(73,127);check(p->isDrumPadPlaying(0),"learned CC did not trigger drum pad");
        p->beginDrumPadMidiLearn(0);
        const auto sendNote=[&](int channel,int note){
            juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(channel,note,(juce::uint8)110),0);p->processBlock(audio,midi);
        };
        sendNote(1,36);
        check(p->isDrumPadMidiLearning(0),"melodic channel captured by drum learn");
        sendNote(10,36);
        check(!p->isDrumPadMidiLearning(0)&&pad.midiNote.load()==36,"channel 10 note not learned");
        pad.position.store(-1);sendNote(1,36);check(!p->isDrumPadPlaying(0),"melodic note triggered drum pad");
        sendNote(10,36);check(p->isDrumPadPlaying(0),"channel 10 note did not trigger drum pad");
        std::cout<<"Drum volume, channel 10 note and sustain-safe CC Learn passed\n";
    }
};

static void hammondPresetAndWheel()
{
    auto p=std::make_unique<ClassicPlayerAudioProcessor>();
    p->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::hammond);
    for(int preset:{1,3,28,32})
    {
        p->setHammondConfig(0,HammondEngine::preset(0));
        auto panel=std::make_unique<HammondEditorPanel>(*p,0);
        auto* selection=dynamic_cast<juce::ComboBox*>(panel->getChildComponent(0));
        check(selection!=nullptr,"Hammond preset selector missing");
        selection->setSelectedId(preset+2,juce::sendNotificationSync);
        // Flush the callbacks used by delayed slider notifications from widget
        // setup, without synthesizing an actual change of value.
        for(int i=0;i<panel->getNumChildComponents();++i)
            if(auto* slider=dynamic_cast<juce::Slider*>(panel->getChildComponent(i)))
                if(slider->onValueChange)slider->onValueChange();
        check(p->hammondConfig(0).preset==preset,"opening panel lost preset name");
        panel.reset();panel=std::make_unique<HammondEditorPanel>(*p,0);
        auto* combo=dynamic_cast<juce::ComboBox*>(panel->getChildComponent(0));
        check(combo&&combo->getSelectedId()==preset+2,"reopened preset label");
        panel.reset();
        juce::MemoryBlock state;p->getStateInformation(state);
        p->setStateInformation(state.getData(),(int)state.getSize());
        check(p->hammondConfig(0).preset==preset,"preset identity persistence");
    }
    auto engine=std::make_unique<HammondEngine>();engine->prepare(48000,128);
    auto c=configs();c[0]=HammondEngine::preset(1);c[0].routing.midiChannel=3;
    c[0].cc[0]=1;c[0].channel[0]=3; // Legacy CC1 drawbar assignment must not steal the wheel.
    const auto bars=c[0].bars;
    const auto send=[&](int channel,int value,bool routed){
        juce::AudioBuffer<float> audio(2,128);audio.clear();juce::MidiBuffer m,empty;
        m.addEvent(juce::MidiMessage::controllerEvent(channel,1,value),0);
        std::array<juce::MidiBuffer,HammondEngine::layerCount> local;
        if(routed)local[0].swapWith(m);
        engine->process(audio,routed?empty:m,c,routed?&local:nullptr);
    };
    send(2,127,false);check(c[0].leslie==1,"wheel wrong channel");
    for(bool routed:{false,true}){
        send(3,64,routed);check(c[0].leslie==2,"wheel fast boundary");
        send(3,63,routed);check(c[0].leslie==1,"wheel slow boundary");
        send(3,127,routed);check(c[0].leslie==2,"wheel maximum");
        send(3,0,routed);check(c[0].leslie==1,"wheel minimum stopped rotor");
    }
    check(c[0].preset==1&&c[0].bars==bars,"wheel changed registration or preset name");
    std::cout<<"Hammond preset reopen, CC1 channel/threshold and host/routed checks passed\n";
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    try{
        DrumPadRegressionAccess::run();
        continuousPadRegression();
        hammondPresetAndWheel();
        analogEditorLifecycle();
        nativeWindowStyle();
        LiveSetLayoutRegressionAccess::run(argc>2 ? argv[2] : nullptr);
        for(double rate:{44100.,48000.}){
            const auto a=render(512,rate,false),b=render(127,rate,false),d=render(127,rate,true);
            float error=0,tail=0,signal=0,jump=0;
            for(size_t i=0;i<a.size();++i){
                check(std::isfinite(a[i]),"nonfinite audio");error=std::max(error,std::abs(a[i]-b[i]));
                check(a[i]==d[i],"host/routed mismatch");if(i>14000)tail=std::max(tail,std::abs(a[i]));
                signal=std::max(signal,std::abs(a[i]));if(i>0)jump=std::max(jump,std::abs(a[i]-a[i-1]));
            }
            check(error<1e-6,"block-size dependence");check(tail<1e-5,"stuck note");
            std::cout<<"Measured rate "<<rate<<" peak "<<signal<<" jump "<<jump<<"\n";
            if(jump>=.01)for(size_t i=1;i<a.size();++i)if(std::abs(a[i]-a[i-1])>=.01)
                std::cout<<"Jump at "<<i<<" from "<<a[i-1]<<" to "<<a[i]<<"\n";
            check(signal>1e-3,"silent instrument");check(jump<.01,"release discontinuity");
            const auto full=render(128,rate,false,true);
            for(float f:full)check(std::isfinite(f)&&std::abs(f)<1,"full drawbar clipping");
            std::cout<<"B/C release, host/routed, blocks 127/512 @ "<<rate<<" error "<<error<<" jump "<<jump<<"\n";
        }
        auto engine=std::make_unique<HammondEngine>();engine->prepare(48000,128);
        auto c=configs();c[0].learning=0;
        juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(3,64,127),0);
        audio.clear();engine->process(audio,midi,c);check(c[0].learning==0,"sustain consumed learn");
        midi.clear();midi.addEvent(juce::MidiMessage::controllerEvent(3,21,127),0);
        audio.clear();engine->process(audio,midi,c);check(c[0].cc[0]==21&&c[0].channel[0]==3&&c[0].bars[0]==8,"learn");
        midi.clear();midi.addEvent(juce::MidiMessage::controllerEvent(2,21,0),0);
        audio.clear();engine->process(audio,midi,c);check(c[0].bars[0]==8,"learn wrong channel");
        midi.clear();midi.addEvent(juce::MidiMessage::controllerEvent(3,21,0),0);
        audio.clear();engine->process(audio,midi,c);check(c[0].bars[0]==0,"drawbar CC");
        const auto restored=HammondEngine::restore(HammondEngine::save(c[0],0));
        check(restored.cc==c[0].cc&&restored.bars==c[0].bars,"engine state");
        check(HammondEngine::presetNames().size()==33&&HammondEngine::preset(28).leslie==2,"presets");
        auto p=std::make_unique<ClassicPlayerAudioProcessor>();
        p->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::hammond);
        p->setHammondConfig(0,c[0]);juce::MemoryBlock state;p->getStateInformation(state);
        auto q=std::make_unique<ClassicPlayerAudioProcessor>();
        q->setStateInformation(state.getData(),(int)state.getSize());
        check(q->layerType(0)==ClassicPlayerAudioProcessor::LayerType::hammond,"layer type restore");
        check(q->hammondConfig(0).cc==c[0].cc&&q->hammondConfig(0).bars==c[0].bars,"program config restore");
        check(q->addLayer(ClassicPlayerAudioProcessor::LayerType::hammond),"add layer");
        const int last=q->activeLayerCount()-1;q->setHammondConfig(last,HammondEngine::preset(28));
        q->removeLayer(last-1);
        check(q->layerType(last-1)==ClassicPlayerAudioProcessor::LayerType::hammond&&q->hammondConfig(last-1).leslie==2,"layer removal loses Hammond");
        q->setLayerType(0,ClassicPlayerAudioProcessor::LayerType::analog);
        check(q->layerType(0)==ClassicPlayerAudioProcessor::LayerType::analog,"switch source");
        auto analog=q->analogSynthConfig(0);
        analog.browserCompatible=true;analog.ampDecayMs=3900;analog.lfoToFilter=25;
        q->setAnalogSynthConfig(0,analog);
        juce::MemoryBlock analogState;q->getStateInformation(analogState);
        p->setStateInformation(analogState.getData(),(int)analogState.getSize());
        check(p->analogSynthConfig(0).browserCompatible
              &&p->analogSynthConfig(0).ampDecayMs==3900
              &&p->analogSynthConfig(0).lfoToFilter==25,"browser Analog state roundtrip");
        std::cout<<"MIDI learn/channel, 33 presets, state roundtrip, add/remove/switch layers and browser Analog persistence passed\n";
        if(argc>1){
            auto panel=createHammondEditorContent(*q,last-1);
            for(int i=0;i<panel->getNumChildComponents();++i){
                auto* child=panel->getChildComponent(i);
                check(dynamic_cast<juce::MidiKeyboardComponent*>(child)==nullptr,"unexpected keyboard");
                check(panel->getLocalBounds().contains(child->getBounds()),"editor child clipped");
                for(int j=0;j<child->getNumChildComponents();++j)
                    check(child->getLocalBounds().contains(child->getChildComponent(j)->getBounds()),"control clipped");
            }
            const auto image=panel->createComponentSnapshot(panel->getLocalBounds());
            juce::FileOutputStream stream{juce::File(argv[1])};juce::PNGImageFormat png;
            check(stream.openedOk()&&png.writeImageToStream(image,stream),"panel screenshot");
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
