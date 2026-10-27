#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "LayerEffects.h"
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#if JUCE_MAC || JUCE_LINUX
 #include <dlfcn.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

// File decoding is performed by the caller outside the audio callback. The
// callback only reads the published buffers and uses fixed-size voice storage.
class ContinuousPadBank
{
    struct SampleData
    {
        // Interleaved signed 16-bit PCM halves the resident footprint versus
        // float buffers. Factory data is shared between continuous-pad layers.
        std::vector<int16_t> stereo;
        int frames = 0;
        double rate = 48000.0;
    };

public:
    static constexpr int count=12;
    struct EqSettings
    {
        float low=0.f,mid=0.f,high=0.f;
        float lowFrequency=220.f,midFrequency=1200.f,highFrequency=4200.f;
        float lowQ=0.707f,midQ=1.f,highQ=0.707f;
    };
    ContinuousPadBank() { for(auto& p:pads) p.cc=-1; }
    void prepare(double rate)
    {
        const juce::ScopedLock guard(lock);
        sampleRate=juce::jmax(1.0,rate); volume.reset(sampleRate,0.02);
        volume.setCurrentAndTargetValue(0.f); stopImmediately();
    }
    juce::Result load(int index,const juce::File& file)
    {
        if(!juce::isPositiveAndBelow(index,count))return juce::Result::fail(juce::String::fromUTF8("Pad inválido"));
        std::shared_ptr<SampleData> audio;
        const auto decoded = decodeFile(file, 10.0 * 60.0, audio);
        if (decoded.failed()) return decoded;
        const juce::ScopedLock guard(lock);
        juce::int64 totalFrames=audio->frames;
        for(int i=0;i<count;++i)
            if(i!=index && pads[(size_t)i].audio != nullptr)
                totalFrames+=pads[(size_t)i].audio->frames;
        constexpr juce::int64 maximumBankBytes=4ll*1024ll*1024ll*1024ll;
        constexpr juce::int64 bytesPerStereoFrame=(juce::int64)sizeof(int16_t)*2;
        if(totalFrames*bytesPerStereoFrame>maximumBankBytes)
            return juce::Result::fail(juce::String::fromUTF8("Limite de memória dos pads contínuos atingido (4 GB). Remova um pad ou use arquivos menores."));
        auto& p=pads[(size_t)index];p.audio=std::move(audio);
        p.path=file.getFullPathName();p.position=0;p.level=0;
        if(active.load()==index){active=-1;command=-2;}
        return juce::Result::ok();
    }
    juce::String path(int i) const {const juce::ScopedLock g(lock);return pads[(size_t)i].path;}
    static juce::String noteName(int i)
    {
        static constexpr const char* names[count] { "C", "C#", "D", "D#", "E", "F",
                                                     "F#", "G", "G#", "A", "A#", "B" };
        return juce::isPositiveAndBelow(i, count) ? juce::String(names[i]) : juce::String{};
    }
    int mapping(int i) const {const juce::ScopedLock g(lock);return i==count?stopCC.load():pads[(size_t)i].cc;}
    int learningTarget() const {return learning.load();}
    void learn(int i){learning = learning.load() == i ? -1 : i;}
    void clearMapping(int i)
    {
        const juce::ScopedLock guard(lock);
        if (i == count) stopCC = -1;
        else if (juce::isPositiveAndBelow(i, count)) pads[(size_t) i].cc = -1;
        if (learning.load() == i) learning = -1;
    }
    // Standalone MIDI callbacks run outside the audio thread. Capture here
    // before device/channel routing so Learn behaves like the other layer
    // controls, including for controllers whose current value is below 64.
    bool captureLearnedCC(const juce::MidiMessage& message)
    {
        if (!message.isController()) return false;
        const int cc = message.getControllerNumber();
        if (cc == 64 || cc >= 120 || learning.load() < 0) return false;
        const juce::ScopedLock guard(lock);
        const auto channel = (size_t) (message.getChannel() - 1);
        ccDown[channel][(size_t) cc] = message.getControllerValue() >= 64;
        return assignLearnedCC(cc);
    }
    void trigger(int i){if(juce::isPositiveAndBelow(i,count))command=i;}
    void stop(){command=-1;}
    bool stopFromMidi(const juce::MidiMessage& message)
    {
        if (!message.isController() || message.getControllerValue() < 64
            || message.getControllerNumber() != stopCC.load(std::memory_order_acquire))
            return false;
        stop();
        return true;
    }
    int selected() const{return active.load();}
    float peak() const{return meter.load();}
    double fadeSeconds() const{return fade.load();}
    void setFadeSeconds(double value){fade=juce::jlimit(.02,10.0,value);}
    void stopImmediately()
    {
        const juce::ScopedLock g(lock);
        command=-2;active=-1;meter=0;learning=-1;
        for(auto& p:pads){p.level=0;p.position=0;}highPassInput={};highPassOutput={};lowPassState={};eq.reset();
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
        if(assignLearnedCC(cc))return;
        // STOP is idempotent. Some controller buttons send only 127 and no
        // release, so an edge latch would leave a learned STOP inert.
        if(stopCC.load()==cc){if(down)stop();return;}
        if(!edge)return;
        for(int i=0;i<count;++i)if(pads[(size_t)i].cc==cc){trigger(i);break;}
    }
    void render(juce::AudioBuffer<float>& output,float gain,float highPassHz,float lowPassHz,
                const EqSettings& eqSettings)
    {
        const juce::ScopedTryLock g(lock);if(!g.isLocked()){meter=0;return;}
        const int next=command.exchange(-2);
        if(next==-1)active=-1;
        else if(next>=0 && next!=active.load() && pads[(size_t)next].audio != nullptr
                && pads[(size_t)next].audio->frames>0)
        {
            if(pads[(size_t)next].level==0)pads[(size_t)next].position=0;
            active=next;
        }
        const double seconds=fade.load();
        const float step=(float)(1.0/(sampleRate*seconds));
        volume.setTargetValue(gain);
        float blockPeak=0;
        highPassHz=juce::jlimit(20.f,1000.f,highPassHz);lowPassHz=juce::jlimit(1000.f,20000.f,lowPassHz);
        const bool highPassEnabled=highPassHz>20.5f,lowPassEnabled=lowPassHz<19950.f;
        const float highPassCoefficient=std::exp(-juce::MathConstants<float>::twoPi*highPassHz/(float)sampleRate);
        const float lowPassCoefficient=std::exp(-juce::MathConstants<float>::twoPi*lowPassHz/(float)sampleRate);
        eq.setParameters(eqSettings.low, eqSettings.mid, eqSettings.high, sampleRate,
                         eqSettings.lowFrequency, eqSettings.midFrequency,
                         eqSettings.highFrequency, eqSettings.lowQ,
                         eqSettings.midQ, eqSettings.highQ);
        for(int sample=0;sample<output.getNumSamples();++sample)
        {
            float sum[2]{};
            for(int i=0;i<count;++i)
            {
                auto& p=pads[(size_t)i];const bool on=i==active.load();
                p.level=juce::jlimit(0.f,1.f,p.level+(on?step:-step));
                const int length=p.audio != nullptr ? p.audio->frames : 0;
                if(p.level<=0 || length<4)continue;
                const double overlap=juce::jlimit(1.0,length*.5,seconds*p.audio->rate);
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
                p.position+=p.audio->rate/sampleRate;
                if(p.position>=length)p.position=overlap+std::fmod(p.position-length,length-overlap);
            }
            const float v=volume.getNextValue();
            for(int ch=0;ch<juce::jmin(2,output.getNumChannels());++ch)
            {float value=sum[ch]*v;
             if(highPassEnabled){const float filtered=highPassCoefficient*(highPassOutput[(size_t)ch]+value-highPassInput[(size_t)ch]);highPassInput[(size_t)ch]=value;highPassOutput[(size_t)ch]=filtered;value=filtered;}
             if(lowPassEnabled){lowPassState[(size_t)ch]=(1.f-lowPassCoefficient)*value+lowPassCoefficient*lowPassState[(size_t)ch];value=lowPassState[(size_t)ch];}
             value=eq.process(value,ch);
             output.addSample(ch,sample,value);blockPeak=juce::jmax(blockPeak,std::abs(value));}
        }
        meter=blockPeak;
    }
    juce::ValueTree save() const
    {
        const juce::ScopedLock g(lock);juce::ValueTree tree("ContinuousPads");
        tree.setProperty("fade",fade.load(),nullptr);tree.setProperty("stopCC",stopCC.load(),nullptr);
        for(int i=0;i<count;++i)
        {juce::ValueTree child("Pad");child.setProperty("path",pads[(size_t)i].path,nullptr);
         child.setProperty("cc",pads[(size_t)i].cc,nullptr);tree.addChild(child,-1,nullptr);}
        return tree;
    }
    void restore(const juce::ValueTree& tree, bool provideFactoryPad = false)
    {
        stopImmediately();
        setFadeSeconds((double)tree.getProperty("fade",1.0));
        {const juce::ScopedLock g(lock);stopCC=validCC((int)tree.getProperty("stopCC",-1));
         for(auto& p:pads){p.audio.reset();p.path.clear();p.cc=-1;}}
        for(int i=0;i<count;++i)
        {
            const auto child=tree.getChild(i);const auto filename=child.getProperty("path").toString();
            const bool isFactory = filename.startsWith(factoryPathPrefix())
                                || filename == "Built-in Warm Pad";
            bool loaded = false;
            if (isFactory)
                loaded = loadFactoryPad(i);
            else if(filename.isNotEmpty())
                loaded = load(i,juce::File(filename)).wasOk();
            if (!loaded && provideFactoryPad && filename.isEmpty())
                loaded = loadFactoryPad(i);
            const juce::ScopedLock g(lock);
            if (!isFactory && filename.isNotEmpty()) pads[(size_t)i].path=filename;
            pads[(size_t)i].cc=validCC((int)child.getProperty("cc",-1));
        }
        stopImmediately();
    }
    bool loadFactoryPad(int index)
    {
        if (!juce::isPositiveAndBelow(index, count)) return false;
        auto audio = getFactorySample(index);
        if (audio == nullptr) return false;
        const juce::ScopedLock guard(lock);
        auto& pad = pads[(size_t)index];
        pad.audio = std::move(audio);
        pad.path = factoryPath(index);
        pad.position = 0.0;
        pad.level = 0.0f;
        return true;
    }
private:
    static juce::Result decodeFile(const juce::File& file, double maxSeconds,
                                   std::shared_ptr<SampleData>& result)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader || reader->lengthInSamples < 4 || reader->sampleRate <= 0.0)
            return juce::Result::fail(juce::String::fromUTF8("Não foi possível ler este áudio"));

        const auto maxFrames = static_cast<juce::int64>(std::floor(reader->sampleRate * maxSeconds));
        if (reader->lengthInSamples > maxFrames)
            return juce::Result::fail(juce::String::fromUTF8("Áudio muito grande. Cada pad contínuo aceita até 10 minutos."));

        const auto frames = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> decoded(2, frames);
        if (!reader->read(&decoded, 0, frames, 0, true, true))
            return juce::Result::fail(juce::String::fromUTF8("Falha ao ler o áudio"));
        if (reader->numChannels == 1)
            decoded.copyFrom(1, 0, decoded, 0, 0, frames);

        auto packed = std::make_shared<SampleData>();
        packed->frames = frames;
        packed->rate = reader->sampleRate;
        packed->stereo.resize((size_t)frames * 2);
        for (int frame = 0; frame < frames; ++frame)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto value = juce::jlimit(-1.0f, 0.999969f, decoded.getSample(channel, frame));
                packed->stereo[(size_t)frame * 2 + (size_t)channel]
                    = static_cast<int16_t>(std::lrint(value * 32768.0f));
            }
        }
        result = std::move(packed);
        return juce::Result::ok();
    }

    static juce::String factoryPathPrefix() { return "@factory-pad:"; }
    static juce::String factoryPath(int index) { return factoryPathPrefix() + noteName(index); }

    static juce::File factoryResourceDirectory()
    {
        juce::File moduleFile;
#if JUCE_MAC || JUCE_LINUX
        Dl_info info{};
        const auto address = reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(&ContinuousPadBank::factoryResourceDirectory));
        if (dladdr(address, &info) != 0 && info.dli_fname != nullptr)
            moduleFile = juce::File(juce::String::fromUTF8(info.dli_fname));
#elif JUCE_WINDOWS
        HMODULE module = nullptr;
        const auto address = reinterpret_cast<LPCWSTR>(
            reinterpret_cast<uintptr_t>(&ContinuousPadBank::factoryResourceDirectory));
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              address, &module) != 0)
        {
            std::array<wchar_t, 32768> path{};
            const auto length = GetModuleFileNameW(module, path.data(), (DWORD)path.size());
            if (length > 0 && length < path.size())
                moduleFile = juce::File(juce::String::fromWideCharPointer(path.data()));
        }
#endif

        auto directory = moduleFile.existsAsFile()
                       ? moduleFile.getParentDirectory()
                       : juce::File::getSpecialLocation(juce::File::currentApplicationFile);
        for (int level = 0; level < 7 && directory != directory.getParentDirectory(); ++level)
        {
            for (const auto& candidate : {
                    directory.getChildFile("Resources/ContinuousPads"),
                    directory.getChildFile("Contents/Resources/ContinuousPads"),
                    directory.getChildFile("ContinuousPads") })
                if (candidate.isDirectory()) return candidate;
            directory = directory.getParentDirectory();
        }
        return {};
    }

    static std::shared_ptr<SampleData> getFactorySample(int index)
    {
        static juce::CriticalSection cacheLock;
        static std::array<std::shared_ptr<SampleData>, count> cache{};
        const juce::ScopedLock guard(cacheLock);
        auto& sample = cache[(size_t)index];
        if (sample != nullptr) return sample;

        static constexpr const char* resourceNames[count] {
            "C", "C_sharp", "D", "D_sharp", "E", "F",
            "F_sharp", "G", "G_sharp", "A", "A_sharp", "B"
        };
        const auto file = factoryResourceDirectory().getChildFile(
            juce::String(resourceNames[index]) + ".flac");
        std::shared_ptr<SampleData> decoded;
        if (decodeFile(file, 121.0, decoded).failed())
        {
            juce::Logger::writeToLog(juce::String::fromUTF8("Pad de fábrica não encontrado ou inválido: ")
                                     + file.getFullPathName());
            return {};
        }
        sample = std::move(decoded);
        return sample;
    }

    bool assignLearnedCC(int cc)
    {
        const int target = learning.exchange(-1);
        if (target < 0) return false;
        for (auto& pad : pads)
            if (pad.cc == cc) pad.cc = -1;
        if (stopCC.load() == cc) stopCC = -1;
        if (target == count) stopCC = cc;
        else if (target < count) pads[(size_t) target].cc = cc;
        return true; // Learning must not start/stop audio unexpectedly.
    }

    struct Pad {std::shared_ptr<SampleData> audio;juce::String path;double position=0;float level=0;int cc=-1;};
    static int validCC(int cc){return cc>=0&&cc<120&&cc!=64?cc:-1;}
    static float read(const Pad& p,int ch,double pos)
    {const int i=juce::jlimit(0,p.audio->frames-1,(int)pos);
     const int j=juce::jmin(i+1,p.audio->frames-1);
     const auto a=(float)p.audio->stereo[(size_t)i*2+(size_t)ch]/32768.0f;
     const auto b=(float)p.audio->stereo[(size_t)j*2+(size_t)ch]/32768.0f;
     return a+(b-a)*(float)(pos-i);}
    mutable juce::CriticalSection lock;
    std::array<Pad,count> pads;
    std::array<std::array<bool,128>,16> ccDown{};
    std::atomic<int> command{-2},active{-1},learning{-1};
    std::atomic<float> meter{0};std::atomic<double> fade{1.0};
    std::atomic<int> stopCC{-1};double sampleRate=48000;
    std::array<float,2> highPassInput{},highPassOutput{},lowPassState{};
    LayerEqState eq;
    juce::SmoothedValue<float> volume;
};
