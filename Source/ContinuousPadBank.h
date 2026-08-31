#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>

// File decoding is performed by the caller outside the audio callback. The
// callback only reads the published buffers and uses fixed-size voice storage.
class ContinuousPadBank
{
public:
    static constexpr int count=12;
    ContinuousPadBank() { for(auto& p:pads) p.cc=-1; }
    void prepare(double rate)
    {
        const juce::ScopedLock guard(lock);
        sampleRate=juce::jmax(1.0,rate); volume.reset(sampleRate,0.02);
        volume.setCurrentAndTargetValue(0.f); stopImmediately();
    }
    juce::Result load(int index,const juce::File& file)
    {
        if(!juce::isPositiveAndBelow(index,count))return juce::Result::fail("Pad inválido");
        juce::AudioFormatManager formats;formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if(!reader || reader->lengthInSamples<4 || reader->sampleRate<=0)
            return juce::Result::fail("Não foi possível ler este áudio");
        // This is a time limit, not a byte limit: a 44.1 kHz source gets the
        // same ten minutes as a 48 kHz source. Audio is held as stereo float
        // internally. Keep a deliberately generous per-layer working budget:
        // several ten-minute MP3 pads can coexist without rejecting the second
        // pad solely because the first one was decoded to PCM.
        constexpr double maxSeconds = 10.0 * 60.0;
        const auto maxFrames = static_cast<juce::int64>(std::floor(reader->sampleRate * maxSeconds));
        if(reader->lengthInSamples>maxFrames)
            return juce::Result::fail("Áudio muito grande. Cada pad contínuo aceita até 10 minutos.");
        juce::AudioBuffer<float> audio(2,(int)reader->lengthInSamples);
        if(!reader->read(&audio,0,audio.getNumSamples(),0,true,true))
            return juce::Result::fail("Falha ao ler o áudio");
        if(reader->numChannels==1)audio.copyFrom(1,0,audio,0,0,audio.getNumSamples());
        const juce::ScopedLock guard(lock);
        juce::int64 totalFrames=audio.getNumSamples();
        for(int i=0;i<count;++i)if(i!=index)totalFrames+=pads[(size_t)i].audio.getNumSamples();
        constexpr juce::int64 maximumBankBytes=4ll*1024ll*1024ll*1024ll; // 4 GiB decoded stereo PCM
        constexpr juce::int64 bytesPerStereoFrame=(juce::int64)sizeof(float)*2;
        if(totalFrames*bytesPerStereoFrame>maximumBankBytes)
            return juce::Result::fail("Limite de memória dos pads contínuos atingido (4 GB). Remova um pad ou use arquivos menores.");
        auto& p=pads[(size_t)index];std::swap(p.audio,audio);p.rate=reader->sampleRate;
        p.path=file.getFullPathName();p.position=0;p.level=0;
        if(active.load()==index){active=-1;command=-2;}
        return juce::Result::ok();
    }
    juce::String path(int i) const {const juce::ScopedLock g(lock);return pads[(size_t)i].path;}
    int mapping(int i) const {const juce::ScopedLock g(lock);return i==count?stopCC:pads[(size_t)i].cc;}
    int learningTarget() const {return learning.load();}
    void learn(int i){learning=i;}
    void trigger(int i){if(juce::isPositiveAndBelow(i,count))command=i;}
    void stop(){command=-1;}
    int selected() const{return active.load();}
    float peak() const{return meter.load();}
    double fadeSeconds() const{return fade.load();}
    void setFadeSeconds(double value){fade=juce::jlimit(.02,10.0,value);}
    void stopImmediately()
    {
        const juce::ScopedLock g(lock);
        command=-2;active=-1;meter=0;learning=-1;
        for(auto& p:pads){p.level=0;p.position=0;}
        for(auto& c:ccDown)c.fill(false);
    }
    void midi(const juce::MidiMessage& message)
    {
        if(!message.isController())return;
        const int cc=message.getControllerNumber(),channel=message.getChannel()-1;
        if(cc==64 || cc>=120)return;
        const juce::ScopedTryLock g(lock);if(!g.isLocked())return;
        const bool down=message.getControllerValue()>=64;
        const bool edge=down&&!ccDown[(size_t)channel][(size_t)cc];
        ccDown[(size_t)channel][(size_t)cc]=down;
        if(!edge)return;
        const int target=learning.exchange(-1);
        if(target>=0)
        {
            for(auto& p:pads)if(p.cc==cc)p.cc=-1;
            if(stopCC==cc)stopCC=-1;
            if(target==count)stopCC=cc;else if(target<count)pads[(size_t)target].cc=cc;
            return; // Learning must not start audio unexpectedly.
        }
        if(stopCC==cc){stop();return;}
        for(int i=0;i<count;++i)if(pads[(size_t)i].cc==cc){trigger(i);break;}
    }
    void render(juce::AudioBuffer<float>& output,float gain)
    {
        const juce::ScopedTryLock g(lock);if(!g.isLocked()){meter=0;return;}
        const int next=command.exchange(-2);
        if(next==-1)active=-1;
        else if(next>=0 && next!=active.load() && pads[(size_t)next].audio.getNumSamples()>0)
        {
            if(pads[(size_t)next].level==0)pads[(size_t)next].position=0;
            active=next;
        }
        const double seconds=fade.load();
        const float step=(float)(1.0/(sampleRate*seconds));
        volume.setTargetValue(gain);
        float blockPeak=0;
        for(int sample=0;sample<output.getNumSamples();++sample)
        {
            float sum[2]{};
            for(int i=0;i<count;++i)
            {
                auto& p=pads[(size_t)i];const bool on=i==active.load();
                p.level=juce::jlimit(0.f,1.f,p.level+(on?step:-step));
                const int length=p.audio.getNumSamples();
                if(p.level<=0 || length<4)continue;
                const double overlap=juce::jlimit(1.0,length*.5,seconds*p.rate);
                const double seam=length-overlap;
                for(int ch=0;ch<2;++ch)
                {
                    float value=read(p,ch,p.position);
                    if(p.position>=seam)
                    {
                        const float mix=(float)((p.position-seam)/overlap);
                        value=value*(1-mix)+read(p,ch,p.position-seam)*mix;
                    }
                    sum[ch]+=value*p.level;
                }
                p.position+=p.rate/sampleRate;
                if(p.position>=length)p.position=overlap+std::fmod(p.position-length,length-overlap);
            }
            const float v=volume.getNextValue();
            for(int ch=0;ch<juce::jmin(2,output.getNumChannels());++ch)
            {const float value=sum[ch]*v;output.addSample(ch,sample,value);blockPeak=juce::jmax(blockPeak,std::abs(value));}
        }
        meter=blockPeak;
    }
    juce::ValueTree save() const
    {
        const juce::ScopedLock g(lock);juce::ValueTree tree("ContinuousPads");
        tree.setProperty("fade",fade.load(),nullptr);tree.setProperty("stopCC",stopCC,nullptr);
        for(int i=0;i<count;++i)
        {juce::ValueTree child("Pad");child.setProperty("path",pads[(size_t)i].path,nullptr);
         child.setProperty("cc",pads[(size_t)i].cc,nullptr);tree.addChild(child,-1,nullptr);}
        return tree;
    }
    void restore(const juce::ValueTree& tree)
    {
        stopImmediately();
        setFadeSeconds((double)tree.getProperty("fade",1.0));
        {const juce::ScopedLock g(lock);stopCC=validCC((int)tree.getProperty("stopCC",-1));
         for(auto& p:pads){p.audio.setSize(0,0);p.path.clear();p.cc=-1;}}
        for(int i=0;i<count;++i)
        {
            const auto child=tree.getChild(i);const auto filename=child.getProperty("path").toString();
            if(filename.isNotEmpty())load(i,juce::File(filename));
            const juce::ScopedLock g(lock);
            pads[(size_t)i].path=filename;pads[(size_t)i].cc=validCC((int)child.getProperty("cc",-1));
        }
        stopImmediately();
    }
private:
    struct Pad {juce::AudioBuffer<float> audio;juce::String path;double rate=48000,position=0;float level=0;int cc=-1;};
    static int validCC(int cc){return cc>=0&&cc<120&&cc!=64?cc:-1;}
    static float read(const Pad& p,int ch,double pos)
    {const int i=juce::jlimit(0,p.audio.getNumSamples()-1,(int)pos);
     const int j=juce::jmin(i+1,p.audio.getNumSamples()-1);
     return p.audio.getSample(ch,i)+(p.audio.getSample(ch,j)-p.audio.getSample(ch,i))*(float)(pos-i);}
    mutable juce::CriticalSection lock;
    std::array<Pad,count> pads;
    std::array<std::array<bool,128>,16> ccDown{};
    std::atomic<int> command{-2},active{-1},learning{-1};
    std::atomic<float> meter{0};std::atomic<double> fade{1.0};
    int stopCC=-1;double sampleRate=48000;
    juce::SmoothedValue<float> volume;
};
