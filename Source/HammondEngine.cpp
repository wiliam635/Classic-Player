#include "HammondEngine.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
float unit(float x, float fallback = 0) { return std::isfinite(x) ? juce::jlimit(0.f,1.f,x) : fallback; }
double hz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }
}
juce::StringArray HammondEngine::presetNames() { return { "Jimmy Gospel","Jazz Ballad","Rock Organ","Percussive B3","Full Drawbar","Jazz Green Onions","The House Is Rockin","The Way It Is","Whiter Shade","Gospel Fullness","Blues 888000000","Cinema Swell","Cathedral Soft","The Ballad","Soul 888","Jazz Rotary","Deep Warmth","Bright Gospel","Soft Strings","Power Chords","Blues Lead","The Boxer","Nights In White","Church Full","Gritty Rock","Funky Clav Organ","Late Night Jazz","Slow Leslie","Fast Leslie","Percussive Jazz","Vintage Combo","Big Theatre","Minimalist" }; }
HammondEngine::Config HammondEngine::preset(int index)
{
    static const std::array<std::array<int,9>,33> bars {{ {8,8,7,6,8,5,4,3,2},{8,6,8,7,5,4,3,2,1},{8,8,8,8,6,5,4,3,2},{8,7,6,8,7,3,2,1,0},{8,8,8,8,8,8,8,8,8},{8,8,5,6,0,0,0,0,0},{8,8,8,8,8,8,4,0,0},{8,6,8,6,5,0,0,0,0},{8,6,8,7,6,4,3,2,1},{8,8,8,7,8,6,5,4,3},{8,8,8,0,0,0,0,0,0},{8,5,8,7,4,3,2,1,0},{8,4,7,6,4,2,1,0,0},{8,5,8,6,5,3,2,1,0},{8,8,8,0,0,0,0,0,0},{8,6,8,7,5,4,0,0,0},{8,3,7,5,3,2,1,0,0},{8,8,8,8,6,4,3,2,1},{8,4,6,7,5,4,3,2,1},{8,8,8,8,8,6,4,2,0},{8,8,6,0,0,0,0,0,0},{8,6,8,6,5,4,3,2,0},{8,5,8,7,6,4,3,2,1},{8,8,8,8,8,8,6,4,2},{8,8,8,8,7,5,3,1,0},{8,8,6,5,4,2,1,0,0},{8,4,7,6,4,3,2,1,0},{8,6,8,7,5,3,2,1,0},{8,8,8,7,6,4,3,2,1},{8,7,6,8,7,4,2,0,0},{8,5,7,6,4,3,1,0,0},{8,8,8,8,8,7,5,3,1},{8,0,8,0,0,0,0,0,0} }};
    Config c; c.preset = juce::jlimit(0,32,index); c.bars = bars[(size_t)c.preset];
    c.leslie = c.preset == 28 ? 2 : 1;
    c.percussion = c.preset == 3 || c.preset == 29 ? 3 : 0;
    return c;
}
HammondEngine::Config HammondEngine::validated(Config c)
{
    for(auto& bar:c.bars) bar=juce::jlimit(0,8,bar);
    c.leslie=juce::jlimit(0,2,c.leslie);
    if(c.percussion!=2 && c.percussion!=3)c.percussion=0;
    c.click=unit(c.click);c.leakage=unit(c.leakage);c.drive=unit(c.drive);c.level=unit(c.level,.75f);
    c.preset=juce::jlimit(-1,32,c.preset);
    for(size_t i=0;i<c.cc.size();++i){
        if(c.cc[i]<0||c.cc[i]>=120||c.cc[i]==64)c.cc[i]=-1;
        c.channel[i]=juce::jlimit(0,16,c.channel[i]);
    }
    c.learning=juce::jlimit(-1,10,c.learning);
    return c;
}
juce::ValueTree HammondEngine::save(const Config& source, int layer)
{
    const auto c=validated(source);
    juce::ValueTree t("Hammond");t.setProperty("layer",layer,nullptr);
    for(int i=0;i<9;++i)t.setProperty("bar"+juce::String(i),c.bars[(size_t)i],nullptr);
    t.setProperty("leslie",c.leslie,nullptr);t.setProperty("percussion",c.percussion,nullptr);
    t.setProperty("click",c.click,nullptr);t.setProperty("leakage",c.leakage,nullptr);
    t.setProperty("drive",c.drive,nullptr);t.setProperty("level",c.level,nullptr);t.setProperty("preset",c.preset,nullptr);
    for(int i=0;i<11;++i){t.setProperty("cc"+juce::String(i),c.cc[(size_t)i],nullptr);
        t.setProperty("channel"+juce::String(i),c.channel[(size_t)i],nullptr);}
    return t;
}
HammondEngine::Config HammondEngine::restore(const juce::ValueTree& t)
{
    Config c;
    for(int i=0;i<9;++i)c.bars[(size_t)i]=(int)t.getProperty("bar"+juce::String(i),c.bars[(size_t)i]);
    c.leslie=t.getProperty("leslie",c.leslie);c.percussion=t.getProperty("percussion",c.percussion);
    c.click=t.getProperty("click",c.click);c.leakage=t.getProperty("leakage",c.leakage);
    c.drive=t.getProperty("drive",c.drive);c.level=t.getProperty("level",c.level);c.preset=t.getProperty("preset",c.preset);
    for(int i=0;i<11;++i){c.cc[(size_t)i]=t.getProperty("cc"+juce::String(i),-1);
        c.channel[(size_t)i]=t.getProperty("channel"+juce::String(i),0);}
    return validated(c);
}
void HammondEngine::prepare(double rate,int block)
{
    sampleRate=rate>0?rate:48000;
    for(size_t i=0;i<sineTable.size();++i)sineTable[i]=(float)std::sin(2*pi*(double)i/4096);
    scratch.setSize(2,std::max(1,block));
    for(int i=0;i<layerCount;++i){
        auto& l=layers[(size_t)i];
        for(auto& delay:l.delay)delay.assign((size_t)std::ceil(sampleRate*.025)+4,0.f);
        l.reverb.setSampleRate(sampleRate);
        unload(i);
    }
}
void HammondEngine::unload(int index)
{
    if(!juce::isPositiveAndBelow(index,layerCount))return;
    auto& l=layers[(size_t)index];
    for(auto& v:l.voices)v=Voice{};
    l.sustain.fill(false);l.volume.fill(1);l.expression.fill(1);
    for(auto& g:l.channelGain){g.reset(sampleRate,.015);g.setCurrentAndTargetValue(1);}
    for(auto& g:l.bars){g.reset(sampleRate,.015);g.setCurrentAndTargetValue(0);}
    for(auto* g:{&l.gain,&l.drive,&l.leakage,&l.level,&l.pan,&l.rotaryDepth}){
        g->reset(sampleRate,.02);g->setCurrentAndTargetValue(0);}
    for(int r=0;r<2;++r){l.speed[(size_t)r].reset(sampleRate,r==0?2.5:1.0);
        l.speed[(size_t)r].setCurrentAndTargetValue(0);
        std::fill(l.delay[(size_t)r].begin(),l.delay[(size_t)r].end(),0.f);}
    l.rotorPhase={};l.delayPosition=0;l.crossover=0;l.lowpass={};l.compressor={};
    l.reverb.reset();l.enabled=false;l.clock=0;peaks[(size_t)index].store(0);
}
void HammondEngine::stopAllSounds(){for(int i=0;i<layerCount;++i)unload(i);}
float HammondEngine::sine(double phase) const
{
    const auto x=phase*4096;const auto i=juce::jlimit(0,4095,(int)x);
    return sineTable[(size_t)i]+(float)(x-i)*(sineTable[(size_t)i+1]-sineTable[(size_t)i]);
}
void HammondEngine::release(Voice& v)
{
    if(!v.active||v.releasing)return;
    v.down=false;v.releasing=true;v.releaseAge=0;v.releaseStart=v.envelope;
}
void HammondEngine::message(int index,const juce::MidiMessage& m,Config& c)
{
    if(c.routing.midiChannel!=0 && !m.isForChannel(c.routing.midiChannel))return;
    auto& l=layers[(size_t)index];const int ch=juce::jlimit(0,15,m.getChannel()-1);
    if(m.isController()){
        const int cc=m.getControllerNumber(), value=m.getControllerValue();
        if(cc==64){
            l.sustain[(size_t)ch]=c.routing.sustainEnabled&&value>=64;
            if(!l.sustain[(size_t)ch])for(auto& v:l.voices)if(v.channel==ch&&!v.down)release(v);
            return;
        }
        if(cc>=120){
            if(cc==120||cc==123)for(auto& v:l.voices)if(v.channel==ch)release(v);
            if(cc==121){l.sustain[(size_t)ch]=false;l.volume[(size_t)ch]=l.expression[(size_t)ch]=1;
                l.channelGain[(size_t)ch].setTargetValue(1);
                for(auto& v:l.voices)if(v.channel==ch&&!v.down)release(v);}
            return;
        }
        if(c.learning>=0){
            for(size_t i=0;i<c.cc.size();++i)if(c.cc[i]==cc&&c.channel[i]==ch+1)c.cc[i]=-1;
            c.cc[(size_t)c.learning]=cc;c.channel[(size_t)c.learning]=ch+1;c.learning=-1;
        }
        bool mapped=false;
        for(size_t i=0;i<c.cc.size();++i)if(c.cc[i]==cc&&(c.channel[i]==0||c.channel[i]==ch+1)){
            if(i<9)c.bars[i]=(int)std::round(value*8.f/127);
            else if(i==9)c.leslie=value<43?0:value<85?1:2;
            else c.level=value/127.f;
            c.preset=-1;mapped=true;
        }
        if(!mapped){
            if(cc==1)c.leslie=value>=64?2:1;
            if(cc==7)l.volume[(size_t)ch]=value/127.f;
            if(cc==11)l.expression[(size_t)ch]=value/127.f;
            l.channelGain[(size_t)ch].setTargetValue(l.volume[(size_t)ch]*l.expression[(size_t)ch]);
        }
        return;
    }
    if(m.isNoteOff()){
        for(auto& v:l.voices)if(v.active&&v.note==m.getNoteNumber()&&v.channel==ch&&v.down){
            v.down=false;if(!l.sustain[(size_t)ch])release(v);}
        return;
    }
    if(!m.isNoteOn()||m.getNoteNumber()<c.routing.lowNote||m.getNoteNumber()>c.routing.highNote)return;
    for(auto& v:l.voices)if(v.active&&v.note==m.getNoteNumber()&&v.channel==ch)release(v);
    bool first=true;int held=0;
    for(const auto& v:l.voices)if(v.active&&v.down){first=false;++held;}
    if(held>=64)for(auto& v:l.voices)if(v.active&&v.down){release(v);break;}
    auto* chosen=&l.voices[0];
    for(auto& v:l.voices)if(!v.active){chosen=&v;break;}
    if(chosen->active)for(auto& v:l.voices)if(v.envelope<chosen->envelope)chosen=&v;
    auto transition=chosen->transition;
    if(chosen->active)transition.retarget(sampleRate);else transition={};
    *chosen=Voice{};auto& v=*chosen;v.transition=transition;
    v.active=v.down=true;v.note=m.getNoteNumber();v.channel=ch;
    const double base=hz(juce::jlimit(0,127,v.note+c.routing.octave*12));
    for(size_t i=0;i<ratios.size();++i){
        double f=base*ratios[i];const double high=std::min(hz(114),sampleRate*.45);
        while(f>high)f*=.5;while(f<hz(24))f*=2;
        v.step[i]=f/sampleRate;v.phase[i]=std::fmod((double)l.clock*v.step[i],1.0);
    }
    v.percussion=first&&c.percussion ? .12f:0;
    v.perStep=std::min(base*c.percussion,sampleRate*.45)/sampleRate;
    v.click=c.click*.06f;
}
float HammondEngine::renderVoice(Voice& v,Layer& l,const std::array<float,9>& bars,float leak)
{
    if(!v.active)return 0;
    const int attack=std::max(1,(int)(sampleRate*.005)), releaseSamples=std::max(1,(int)(sampleRate*.02));
    if(v.releasing){
        if(v.releaseAge>=releaseSamples){v.active=false;v.envelope=0;return 0;}
        v.envelope=v.releaseStart*.5f*(1.f+(float)std::cos(pi*(double)++v.releaseAge/releaseSamples));
    }else v.envelope=.24f*std::min(1.f,(float)++v.age/attack);
    float sum=0;
    for(size_t i=0;i<ratios.size();++i){
        sum+=sine(v.phase[i])*(i<9?bars[i]:leak*.002f);
        v.phase[i]+=v.step[i];v.phase[i]-=std::floor(v.phase[i]);
    }
    if(v.percussion>1e-6f){
        sum+=sine(v.perPhase)*v.percussion;
        v.perPhase+=v.perStep;v.perPhase-=std::floor(v.perPhase);
        v.percussion*=(float)std::exp(std::log(.0001/.12)/(sampleRate*.08));
    }
    if(v.click>1e-7f){
        l.noise=l.noise*1664525u+1013904223u;
        sum+=((float)(l.noise>>8)/8388608.f-1)*v.click;
        v.click*=(float)std::exp(-8.0/(sampleRate*.015));
    }
    return v.transition.process(sum*v.envelope);
}
void HammondEngine::process(juce::AudioBuffer<float>& out,const juce::MidiBuffer& host,
    std::array<Config,layerCount>& configs,const std::array<juce::MidiBuffer,layerCount>* routed)
{
    juce::ScopedNoDenormals noDenormals;
    for(int index=0;index<layerCount;++index){
        auto& c=configs[(size_t)index];auto& l=layers[(size_t)index];
        if(!c.routing.enabled){if(l.enabled)unload(index);continue;}
        l.enabled=true;
        const juce::MidiBuffer empty;
        const auto& local=routed?(*routed)[(size_t)index]:empty;
        auto hi=host.cbegin(),li=local.cbegin();
        float peak=0;
        const auto setTargets=[&]{
            for(size_t i=0;i<9;++i){
                const int b=i==8&&c.percussion?0:c.bars[i];
                l.bars[i].setTargetValue(b?.085f*std::pow(10.f,(b-8)*3.f/20):0.f);
            }
            l.gain.setTargetValue(c.routing.gain);l.level.setTargetValue(c.level);
            l.leakage.setTargetValue(c.leakage);l.drive.setTargetValue(c.drive);
            l.pan.setTargetValue(c.routing.pan);
            l.rotaryDepth.setTargetValue(c.leslie?1.f:0.f);
            l.speed[0].setTargetValue(c.leslie==0?0.f:c.leslie==1?.65f:5.4f);
            l.speed[1].setTargetValue(c.leslie==0?0.f:c.leslie==1?.8f:6.2f);
        };
        setTargets();
        if(!c.routing.sustainEnabled)for(int ch=0;ch<16;++ch)if(l.sustain[(size_t)ch]){
            l.sustain[(size_t)ch]=false;for(auto& v:l.voices)if(v.channel==ch&&!v.down)release(v);}
        const float cross=1.f-(float)std::exp(-2*pi*800/sampleRate);
        const float cutoffHz=40.f*std::pow(450.f,juce::jlimit(0.f,100.f,c.routing.cutoff)/100.f);
        const float lowpass=1.f-(float)std::exp(-2*pi*std::min((double)cutoffHz,sampleRate*.45)/sampleRate);
        juce::Reverb::Parameters rp;
        rp.roomSize=c.routing.reverbSize/100.f;rp.damping=c.routing.reverbDamping/100.f;
        rp.width=c.routing.reverbWidth/100.f;rp.wetLevel=c.routing.reverb/100.f*.4f;
        rp.dryLevel=1.f-c.routing.reverb/100.f*.2f;l.reverb.setParameters(rp);
        for(int s=0;s<out.getNumSamples();++s){
            bool event=false;
            while(hi!=host.cend()&&(*hi).samplePosition<=s){message(index,(*hi).getMessage(),c);++hi;event=true;}
            while(li!=local.cend()&&(*li).samplePosition<=s){message(index,(*li).getMessage(),c);++li;event=true;}
            if(event)setTargets();
            std::array<float,9> bars;for(size_t i=0;i<9;++i)bars[i]=l.bars[i].getNextValue();
            std::array<float,16> channel;for(size_t i=0;i<16;++i)channel[i]=l.channelGain[i].getNextValue();
            const float leak=l.leakage.getNextValue();float sample=0;
            for(auto& v:l.voices)sample+=renderVoice(v,l,bars,leak)*channel[(size_t)v.channel];
            const float drive=l.drive.getNextValue();
            sample=sample*(1-drive)+std::tanh(2*sample*(1+drive*8))/std::tanh(2.f)*drive/(1+drive*2);
            l.crossover+=cross*(sample-l.crossover);
            const std::array<float,2> bands {l.crossover,sample-l.crossover};
            float left=0,right=0;const float depth=l.rotaryDepth.getNextValue();
            for(size_t r=0;r<2;++r){
                l.rotorPhase[r]+=l.speed[r].getNextValue()/sampleRate;
                l.rotorPhase[r]-=std::floor(l.rotorPhase[r]);
                const float mod=sine(l.rotorPhase[r])*depth;
                auto& delay=l.delay[r];delay[(size_t)l.delayPosition]=bands[r];
                // Wrap integer indices, not a floating-point position. At
                // 48 kHz a tiny negative rounding error can otherwise round
                // up to delay.size() after wrapping and read past the buffer.
                const double offset=(.008+mod*(r?.00045:.00015))*sampleRate;
                const int whole=(int)std::floor(offset), size=(int)delay.size();
                const int a=(l.delayPosition-whole+size)%size,b=(a+size-1)%size;
                const float fraction=(float)(offset-whole);
                const float value=(delay[(size_t)a]+fraction*(delay[(size_t)b]-delay[(size_t)a]))
                                  *(.7f+mod*(r?.12f:.08f));
                const float pan=mod*(r?.65f:.4f);
                left+=value*std::sqrt(.5f*(1-pan));right+=value*std::sqrt(.5f*(1+pan));
            }
            l.delayPosition=(l.delayPosition+1)%(int)l.delay[0].size();
            l.reverb.processStereo(&left,&right,1);
            const float gain=l.gain.getNextValue()*l.level.getNextValue();
            const float pan=juce::jlimit(-1.f,1.f,l.pan.getNextValue());
            float* values[2]={&left,&right};
            for(int ch=0;ch<2;++ch){
                auto& value=*values[ch];auto& env=l.compressor[(size_t)ch];
                l.lowpass[(size_t)ch]+=lowpass*(value-l.lowpass[(size_t)ch]);value=l.lowpass[(size_t)ch];
                const float coefficient=(float)std::exp(-1/(sampleRate*(std::abs(value)>env?.003:.1)));
                env=coefficient*env+(1-coefficient)*std::abs(value);
                const float threshold=juce::Decibels::decibelsToGain(c.routing.compressorThreshold);
                if(env>threshold&&c.routing.compressor>0)
                    value*=1-c.routing.compressor/100.f+c.routing.compressor/100.f*
                        std::pow(threshold/env,1-1/std::max(1.f,c.routing.compressorRatio));
                value*=gain*(ch==0?std::sqrt(1-pan):std::sqrt(1+pan));
            }
            if(out.getNumChannels()>1){out.addSample(0,s,left);out.addSample(1,s,right);}
            else if(out.getNumChannels()==1)out.addSample(0,s,(left+right)*.5f);
            peak=std::max(peak,std::max(std::abs(left),std::abs(right)));++l.clock;
        }
        peaks[(size_t)index].store(peak,std::memory_order_relaxed);
    }
}
