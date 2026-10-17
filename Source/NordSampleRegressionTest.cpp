#include "NordSampleDecoder.h"
#include "NordSampleEngine.h"
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace {
using Bytes=std::vector<std::uint8_t>;
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void be(Bytes& b,size_t at,std::uint32_t v){for(int i=0;i<4;++i)b[at+size_t(i)]=std::uint8_t(v>>(24-i*8));}
void le(Bytes& b,size_t at,std::uint32_t v){for(int i=0;i<4;++i)b[at+size_t(i)]=std::uint8_t(v>>(i*8));}
void chunk(Bytes& b,const char* tag,int version,const Bytes& data){const auto n=b.size();b.resize(n+12);std::memcpy(b.data()+n,tag,4);be(b,n+4,std::uint32_t(version));be(b,n+8,std::uint32_t(data.size()));b.insert(b.end(),data.begin(),data.end());}
Bytes fixture(bool stereo=false){
    Bytes body,head(76),map(1336),stroke(stereo?204:140);
    chunk(body,"NSMP",40,Bytes{0,2,0,5});std::memcpy(head.data()+10,"Synthetic",9);chunk(body,"\0hdr",11,head);
    map[0]=0x10;for(int n=0;n<128;++n)map[6+size_t(n)*10]=0x10;
    map[1317]=1;map[1318]=60;map[1319]=127;map[1320]=0;be(map,1326,1);map[1333]=127;
    chunk(body,"\0map",21,map);
    const auto payloadAt=body.size()+12;
    be(stroke,0,1);stroke[5]=60;stroke[12]=11;stroke[14]=7;stroke[15]=255;
    const auto first=(payloadAt+68)/4,last=first+(stereo?33:17);
    for(int offset:{20,29,38}){stroke[size_t(offset)]=std::uint8_t(first>>8);stroke[size_t(offset+1)]=std::uint8_t(first);}
    stroke[47]=std::uint8_t(last>>8);stroke[48]=std::uint8_t(last);
    be(stroke,68,stereo?0xf80040:0xf80020);
    for(int ch=0;ch<(stereo?2:1);++ch)for(int i=0;i<32;++i){const auto v=std::int16_t(std::sin(i*juce::MathConstants<double>::twoPi/32)*1023*(ch==0?1.0:-.5));const auto at=stereo?72+size_t(i/2)*8+size_t(ch)*4+size_t(i%2)*2:72+size_t(i)*2;stroke[at]=std::uint8_t(std::uint16_t(v)>>8);stroke[at+1]=std::uint8_t(v);}
    be(stroke,stereo?200:136,stereo?0x800040:0x800020);chunk(body,"\0stk",1,stroke);
    std::uint32_t crc=0xffffffffu;for(auto byte:body){crc^=byte;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}crc^=0xffffffffu;
    Bytes b(44);std::memcpy(b.data(),"CBIN",4);b[4]=1;std::memcpy(b.data()+8,"nsmp",4);le(b,20,410);le(b,24,crc);b.insert(b.end(),body.begin(),body.end());return b;
}
}
int runNordSampleRegressionTests(){
    try{
        juce::TemporaryFile tmp(".nsmp4");auto b=fixture();check(tmp.getFile().replaceWithData(b.data(),b.size()),"write synthetic NSMP4");
        auto instrument=std::make_shared<NordSampleDecoder::Instrument>();
        auto r=NordSampleDecoder::decode(tmp.getFile(),*instrument);check(r.wasOk(),r.getErrorMessage().toRawUTF8());
        check(instrument->name=="Synthetic"&&instrument->zones.size()==1,"metadata");
        const auto& z=instrument->zones[0];check(z.frames()==32&&z.channels==1&&z.loopStart==0&&z.loopEnd==32,"PCM/loop");
        check(std::abs(z.pcm[8]-1023.0f/32768)<1e-6f,"signed field/predictor scale");
        for(double rate:{44100.,48000.}){
            NordSampleEngine engine;engine.prepare(rate);engine.set(0,instrument);
            NordProgramDecoder::SynthPart source;source.attackMs=.5f;source.decayMs=-1;source.releaseMs=3;
            Sf2Engine::LayerConfig route;route.release=30;
            juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer events;
            const auto render=[&]{audio.clear();engine.process(0,audio,events,source,route);events.clear();check(std::isfinite(audio.getMagnitude(0,128)),"nonfinite sample audio");};
            render();check(audio.getMagnitude(0,128)==0,"idle noise");
            events.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);events.addEvent(juce::MidiMessage::controllerEvent(1,64,127),0);render();
            for(int block=0;block<20;++block)render();
            events.addEvent(juce::MidiMessage::controllerEvent(1,64,0),0);render();
            check(audio.getMagnitude(0,128)>.001f,"pedal-up killed physically held sample");
            events.addEvent(juce::MidiMessage::noteOff(1,60),0);render();for(int k=0;k<10;++k)render();check(audio.getMagnitude(0,128)==0,"sample release stuck");
            events.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);render();engine.stop();render();check(audio.getMagnitude(0,128)==0,"panic did not stop sample");
        }
        b=fixture(true);tmp.getFile().replaceWithData(b.data(),b.size());
        r=NordSampleDecoder::decode(tmp.getFile(),*instrument);check(r.wasOk(),r.getErrorMessage().toRawUTF8());
        check(instrument->zones[0].channels==2&&instrument->zones[0].frames()==32,"stereo frame count");
        check(std::abs(instrument->zones[0].pcm[16]-1023.f/32768)<1e-6f&&std::abs(instrument->zones[0].pcm[17]+511.f/32768)<1e-6f,"V4 alternating channel words mixed");
        b.back()^=1;tmp.getFile().replaceWithData(b.data(),b.size());check(NordSampleDecoder::decode(tmp.getFile(),*instrument).failed(),"corrupt NSMP4 accepted");
        b.resize(32);tmp.getFile().replaceWithData(b.data(),b.size());check(NordSampleDecoder::decode(tmp.getFile(),*instrument).failed(),"truncated NSMP4 accepted");
        std::cout<<"Nord NSMP4 decoder/sustain/loop regression passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#ifndef NORD_SAMPLE_TEST_EMBEDDED
int main(){return runNordSampleRegressionTests();}
#endif
