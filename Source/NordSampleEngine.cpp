#include "NordSampleEngine.h"
#include <cmath>
#include <algorithm>
#include <limits>

void NordSampleEngine::stop() { for(auto& s:slots){for(auto& v:s.voices)v={};s.sustain.fill(false);s.eq.reset();s.reverb.reset();s.compressor={};} }
void NordSampleEngine::process(int part,juce::AudioBuffer<float>& out,const juce::MidiBuffer& events,
                              const NordProgramDecoder::SynthPart& source,const Sf2Engine::LayerConfig& route)
{
    auto& s=slots[size_t(part)];if(!s.instrument)return;const auto& inst=*s.instrument;
    const auto releaseMs=juce::jlimit(3.0f,45000.0f,source.releaseMs*std::pow(2.f,(route.release-30)/10));
    const auto release=[&](Voice& v){if(!v.release){v.release=true;v.releaseStep=v.envelope/float(sampleRate*releaseMs*.001);}};
    auto event=events.cbegin();
    const auto attackMs=juce::jlimit(.5f,45000.f,source.attackMs+route.attack-5);
    const float attackStep=1.0f/float(sampleRate*attackMs*.001);
    const float decay=source.decayMs<0?1.0f:float(std::exp(-4.605170186/(sampleRate*std::max(3.0f,source.decayMs)*.001)));
    const auto maxHz=40*std::pow(450.0f,route.cutoff/100.0f);
    const float filter=1-float(std::exp(-juce::MathConstants<double>::twoPi*std::min({double(source.filterHz),double(maxHz),sampleRate*.45})/sampleRate));
    s.eq.setParameters(route.eqLow,route.eqMid,route.eqHigh,sampleRate,route.eqLowFrequency,route.eqMidFrequency,route.eqHighFrequency,route.eqLowQ,route.eqMidQ,route.eqHighQ);
    juce::Reverb::Parameters reverb;reverb.roomSize=route.reverbSize/100;reverb.damping=route.reverbDamping/100;reverb.width=route.reverbWidth/100;reverb.wetLevel=route.reverb/100*.72f;reverb.dryLevel=1;
    s.reverb.setParameters(reverb);s.gain.setTargetValue(route.gain);
    const auto comp=juce::jlimit(0.f,1.f,route.compressor/100), threshold=juce::Decibels::decibelsToGain(route.compressorThreshold);
    const auto compAttack=float(std::exp(-1.0/(sampleRate*std::max(.1f,route.compressorAttack)*.001)));
    const auto compRelease=float(std::exp(-1.0/(sampleRate*std::max(5.f,route.compressorRelease)*.001)));
    for(int frame=0;frame<out.getNumSamples();++frame) {
        while(event!=events.cend() && (*event).samplePosition<=frame) {
            const auto m=(*event).getMessage();++event;
            if(route.midiChannel>0 && m.getChannel()!=route.midiChannel)continue;
            const auto channel=juce::jlimit(1,16,m.getChannel());
            if(m.isController()&&m.getControllerNumber()==64){
                const auto down=route.sustainEnabled&&m.getControllerValue()>=64;s.sustain[size_t(channel-1)]=down;
                if(!down)for(auto& v:s.voices)if(v.active&&v.channel==channel&&!v.down)release(v);
            }
            if(m.isAllSoundOff()||m.isAllNotesOff())for(auto& v:s.voices)if(v.channel==channel){if(m.isAllSoundOff())v={};else release(v);}
            if(m.isNoteOff())for(auto& v:s.voices)if(v.active&&v.note==m.getNoteNumber()&&v.channel==channel){v.down=false;if(!s.sustain[size_t(channel-1)]||!route.sustainEnabled)release(v);}
            if(!m.isNoteOn() || !route.enabled)continue;
            const auto note=m.getNoteNumber();int zone=-1;
            for(size_t k=0;k<inst.zones.size();++k){const auto& z=inst.zones[k];if(note>=z.low&&note<=z.high&&m.getVelocity()>=z.velocityLow&&m.getVelocity()<=z.velocityHigh){zone=int(k);break;}}
            if(zone<0)continue;
            if(source.mono)for(auto& v:s.voices)if(v.active)release(v);
            Voice* chosen=nullptr;
            for(auto& v:s.voices)if(!v.active){chosen=&v;break;}
            if(!chosen)chosen=&*std::min_element(s.voices.begin(),s.voices.end(),[](const auto& a,const auto& b){return a.envelope*a.velocity<b.envelope*b.velocity;});
            *chosen={};auto& v=*chosen;v.active=v.down=true;v.zone=zone;v.note=note;v.channel=channel;
            auto velocity=m.getFloatVelocity();if(route.velocityCurve==1)velocity=std::sqrt(velocity);if(route.velocityCurve==2)velocity*=velocity;
            v.velocity=velocity*inst.gain[size_t(note)]*inst.zones[size_t(zone)].gain;
            v.step=NordSampleDecoder::sampleRate/sampleRate*std::pow(2.0,(note+route.octave*12+source.coarse+source.fine*.01+inst.detune[size_t(note)]-inst.zones[size_t(zone)].root)/12.0);
        }
        float sum[2] {};
        for(auto& v:s.voices)if(v.active) {
            const auto& z=inst.zones[size_t(v.zone)];const auto loop=z.loopEnd>z.loopStart;
            const auto cross=loop?std::min(size_t(NordSampleDecoder::sampleRate*.02),(z.loopEnd-z.loopStart)/8):0;
            if(loop&&v.position>=double(z.loopEnd))v.position=z.loopStart+cross+std::fmod(v.position-z.loopEnd,double(z.loopEnd-z.loopStart-cross));
            if(!loop && v.position>=double(z.frames()-1)){v.active=false;continue;}
            if(v.release){v.envelope=std::max(0.0f,v.envelope-v.releaseStep);if(v.envelope<=0){v.active=false;continue;}}
            else if(double(v.age)<sampleRate*attackMs*.001)v.envelope=std::min(1.0f,v.envelope+attackStep);
            else v.envelope*=decay;
            if(v.envelope<1e-7f && v.age>std::uint64_t(sampleRate*attackMs*.001)){v.active=false;continue;}
            const auto read=[&](double position,int ch){const auto i=size_t(position);const auto next=std::min(i+1,z.frames()-1);const auto a=z.pcm[i*size_t(z.channels)+size_t(ch)];return a+float(position-i)*(z.pcm[next*size_t(z.channels)+size_t(ch)]-a);};
            for(int ch=0;ch<2;++ch) {
                const auto actual=std::min(ch,z.channels-1);auto value=read(v.position,actual);
                if(cross && v.position>=double(z.loopEnd-cross)){const auto mix=float((v.position-(z.loopEnd-cross))/cross);value=value*(1-mix)+read(z.loopStart+v.position-(z.loopEnd-cross),actual)*mix;}
                for(auto& pole:v.filter[size_t(ch)]){pole+=filter*(value-pole);value=pole;}
                sum[ch]+=value*v.velocity*v.envelope;
            }
            v.position+=v.step;++v.age;
        }
        const auto pan=juce::jlimit(-1.0f,1.0f,route.pan+source.pan);
        for(int ch=0;ch<2;++ch)sum[ch]=s.eq.process(sum[ch],ch);
        if(route.reverb>0)s.reverb.processStereo(sum,sum+1,1);
        for(int ch=0;ch<2;++ch)if(comp>0){auto& env=s.compressor[size_t(ch)];const auto coeff=std::abs(sum[ch])>env?compAttack:compRelease;env=coeff*env+(1-coeff)*std::abs(sum[ch]);const auto reduction=env>threshold?std::pow(threshold/env,1-1/std::max(1.f,route.compressorRatio)):1;sum[ch]*=1-comp+comp*reduction*juce::Decibels::decibelsToGain(route.compressorMakeup);}
        const auto gain=s.gain.getNextValue();
        for(int ch=0;ch<out.getNumChannels();++ch)out.addSample(ch,frame,sum[std::min(ch,1)]*gain*(ch==0?std::min(1.0f,1-pan):std::min(1.0f,1+pan)));
    }
}
