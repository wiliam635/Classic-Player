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
#include <map>
#include <mutex>

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
        const auto decoded = decodeFileCached(file, 10.0 * 60.0, audio);
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
    // Returns the decoded file when it is already in the cache (never decodes).
    static std::shared_ptr<SampleData> cachedOnly(const juce::File& file)
    {
        std::shared_ptr<SampleData> audio;
        return lookupCache(file, audio) ? audio : nullptr;
    }
    // Decodes, ahead of time and off the audio lock, the user pad files of a saved bank (PadStrip /
    // ContinuousPads node) so the program change that restores it finds them in the cache.
    static void prefetch(const juce::ValueTree& tree)
    {
        juce::Array<juce::File> files;
        for (int i = 0; i < tree.getNumChildren() && i < count; ++i)
        {
            const auto filename = tree.getChild(i).getProperty("path").toString();
            if (filename.isEmpty() || filename.startsWith(factoryPathPrefix()) || filename == "Built-in Warm Pad")
                continue;
            const juce::File file(filename);
            if (file.existsAsFile()) files.addIfNotAlreadyThere(file);
        }
        if (files.isEmpty()) return;
        juce::ThreadPool decoders(juce::jlimit(1, 4, juce::SystemStats::getNumCpus() - 1));
        for (const auto& file : files)
            decoders.addJob([file] { std::shared_ptr<SampleData> ignored; decodeFileCached(file, 10.0 * 60.0, ignored); });
        while (decoders.getNumJobs() > 0) juce::Thread::sleep(20);
    }
    bool loadPendingUserPad(int i)
    {
        juce::String path;
        {
            const juce::ScopedLock g(lock);
            if (!juce::isPositiveAndBelow(i, count) || pads[(size_t)i].audio != nullptr
                || pads[(size_t)i].path.isEmpty() || pads[(size_t)i].path.startsWith(factoryPathPrefix()))
                return false;
            path = pads[(size_t)i].path;
        }
        const juce::File file(path);
        std::shared_ptr<SampleData> audio;
        if (!file.existsAsFile() || decodeFileCached(file, 10.0 * 60.0, audio).failed()) return false;
        const juce::ScopedLock g(lock);
        if (pads[(size_t)i].path != path || pads[(size_t)i].audio != nullptr) return false; // replaced meanwhile
        pads[(size_t)i].audio = std::move(audio);
        pads[(size_t)i].position = 0.0;
        pads[(size_t)i].level = 0.0f;
        return true;
    }
    bool hasAudio(int i) const {const juce::ScopedLock g(lock);return juce::isPositiveAndBelow(i,count)&&pads[(size_t)i].audio!=nullptr;}
    juce::String path(int i) const {const juce::ScopedLock g(lock);return pads[(size_t)i].path;}
    static juce::String noteName(int i)
    {
        static constexpr const char* names[count] { "C", "C#", "D", "D#", "E", "F",
                                                     "F#", "G", "G#", "A", "A#", "B" };
        return juce::isPositiveAndBelow(i, count) ? juce::String(names[i]) : juce::String{};
    }
    // Mappings 0..119 are CCs; 128..255 are MIDI notes (128 + note number).
    static juce::String mappingText(int value) {return value>=128?"NOTA "+juce::String(value-128):"CC "+juce::String(value);}
    int mapping(int i) const {const juce::ScopedLock g(lock);return i==count?stopCC.load():pads[(size_t)i].cc;}
    int learningTarget() const {return learning.load();}
    void learn(int i){learning = learning.load() == i ? -1 : i;}
    void clearMapping(int i)
    {
        const juce::ScopedLock guard(lock);
        if (i == count) stopCC = -1;
        else if (juce::isPositiveAndBelow(i, count))
        {
            pads[(size_t) i].cc = -1;
            pads[(size_t) i].ccChannel = 0;
        }
        if (learning.load() == i) learning = -1;
    }
    // Standalone MIDI callbacks run outside the audio thread. Capture here
    // before device/channel routing so Learn behaves like the other layer
    // controls, including for controllers whose current value is below 64.
    // 0 = every MIDI channel; 1..16 = only that channel (Learn, pads and STOP alike).
    int midiChannel() const {return channelFilter.load();}
    void setMidiChannel(int channel){channelFilter=juce::jlimit(0,16,channel);}
    bool acceptsChannel(const juce::MidiMessage& message) const
    {const int f=channelFilter.load();return f==0||message.getChannel()==f;}
    bool captureLearnedCC(const juce::MidiMessage& message)
    {
        if (!message.isController() || !acceptsChannel(message)) return false;
        const int cc = message.getControllerNumber();
        if (cc == 64 || cc >= 120 || learning.load() < 0) return false;
        const juce::ScopedLock guard(lock);
        const auto channel = (size_t) (message.getChannel() - 1);
        ccDown[channel][(size_t) cc] = message.getControllerValue() >= 64;
        return assignLearnedCC(cc, message.getChannel());
    }
    void trigger(int i){if(juce::isPositiveAndBelow(i,count))command=i;}
    void stop(){command=-1;}
    bool stopFromMidi(const juce::MidiMessage& message)
    {
        if (!message.isController() || !acceptsChannel(message) || message.getControllerValue() < 64
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
        if (!acceptsChannel(message)) return;
        // Hardware pads commonly send Note On rather than CC. Store notes as
        // 128 + note number so existing CC mappings and saved banks stay valid.
        if (message.isNoteOn())
        {
            const juce::ScopedTryLock guard(lock);
            if (!guard.isLocked()) return;
            const int key = 128 + message.getNoteNumber();
            if (assignLearnedCC(key, message.getChannel())) return;
            if (stopCC.load() == key) { stop(); return; }
            for (int i = 0; i < count; ++i)
                if (pads[(size_t)i].cc == key
                    && (pads[(size_t)i].ccChannel == 0
                        || pads[(size_t)i].ccChannel == message.getChannel()))
                { trigger(i); break; }
            return;
        }
        if(!message.isController())return;
        const int cc=message.getControllerNumber(),channel=message.getChannel()-1;
        if(cc==64 || cc>=120)return;
        const juce::ScopedTryLock g(lock);if(!g.isLocked())return;
        const bool down=message.getControllerValue()>=64;
        const bool edge=down&&!ccDown[(size_t)channel][(size_t)cc];
        ccDown[(size_t)channel][(size_t)cc]=down;
        if(assignLearnedCC(cc, message.getChannel()))return;
        // STOP is idempotent. Some controller buttons send only 127 and no
        // release, so an edge latch would leave a learned STOP inert.
        if(stopCC.load()==cc){if(down)stop();return;}
        if(!edge)return;
        for(int i=0;i<count;++i)
            if(pads[(size_t)i].cc==cc
                && (pads[(size_t)i].ccChannel == 0
                    || pads[(size_t)i].ccChannel == message.getChannel()))
            {trigger(i);break;}
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
        // The bank lock fixes selection/audio buffers for this block. Build a
        // small ordered list of playing/fading pads and their loop geometry.
        const int selectedPad=active.load();
        std::array<int,count> renderingPads {};
        std::array<double,count> overlaps {}, seams {}, increments {};
        int renderingCount=0;
        for(int i=0;i<count;++i)
        {
            const auto& p=pads[(size_t)i];
            if(p.level<=0 && i!=selectedPad)continue;
            renderingPads[(size_t)renderingCount++]=i;
            const int length=p.audio != nullptr?p.audio->frames:0;
            if(length>=4)
            {
                overlaps[(size_t)i]=juce::jlimit(1.0,length*.5,seconds*p.audio->rate);
                seams[(size_t)i]=length-overlaps[(size_t)i];
                increments[(size_t)i]=p.audio->rate/sampleRate;
            }
        }
        if(renderingCount==0)
        {
            // A fully faded/stopped bank has no source audio or effect tail
            // worth processing. Drain the volume ramp and clear filter state
            // once so the next trigger starts cleanly without a silent block
            // scanning every sample through the EQ/filter chain.
            volume.skip(output.getNumSamples());
            highPassInput={};highPassOutput={};lowPassState={};eq.reset();
            meter=0.0f;
            return;
        }
        float blockPeak=0;
        highPassHz=juce::jlimit(20.f,1000.f,highPassHz);lowPassHz=juce::jlimit(80.f,20000.f,lowPassHz);
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
            for(int slot=0;slot<renderingCount;++slot)
            {
                const int i=renderingPads[(size_t)slot];
                auto& p=pads[(size_t)i];const bool on=i==selectedPad;
                p.level=juce::jlimit(0.f,1.f,p.level+(on?step:-step));
                const int length=p.audio != nullptr ? p.audio->frames : 0;
                if(p.level<=0 || length<4)continue;
                const double overlap=overlaps[(size_t)i];
                const double seam=seams[(size_t)i];
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
                p.position+=increments[(size_t)i];
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
        tree.setProperty("fade",fade.load(),nullptr);tree.setProperty("midiChannel",channelFilter.load(),nullptr);tree.setProperty("stopCC",stopCC.load(),nullptr);
        for(int i=0;i<count;++i)
        {juce::ValueTree child("Pad");child.setProperty("path",pads[(size_t)i].path,nullptr);
         child.setProperty("cc",pads[(size_t)i].cc,nullptr);
         child.setProperty("ccChannel",pads[(size_t)i].ccChannel,nullptr);
         tree.addChild(child,-1,nullptr);}
        return tree;
    }
    // deferFactoryPads: factory pads are only registered here (path, no audio); the caller loads them in the
    // background (loadFactoryPad), so a program change never waits for ~12 FLAC decodes.
    void restore(const juce::ValueTree& tree, bool provideFactoryPad = false, bool deferFactoryPads = false,
                 bool deferUserPads = false)
    {
        stopImmediately();
        setFadeSeconds((double)tree.getProperty("fade",1.0));setMidiChannel((int)tree.getProperty("midiChannel",0));
        // Pads already holding the very same file are kept as they are: decoding them again on every
        // program change (inside the audio lock) cost many seconds, e.g. on a laptop with a slow CPU.
        std::array<std::shared_ptr<SampleData>,count> keptAudio;
        std::array<juce::String,count> keptPath;
        {const juce::ScopedLock g(lock);stopCC=validCC((int)tree.getProperty("stopCC",-1));
         for(int i=0;i<count;++i)
         {auto& p=pads[(size_t)i];keptAudio[(size_t)i]=std::move(p.audio);keptPath[(size_t)i]=p.path;
          p.audio.reset();p.path.clear();p.cc=-1;p.ccChannel=0;}}
        for(int i=0;i<count;++i)
        {
            const auto child=tree.getChild(i);const auto filename=child.getProperty("path").toString();
            const bool isFactory = filename.startsWith(factoryPathPrefix())
                                || filename == "Built-in Warm Pad";
            bool loaded = false;
            const auto wantedPath = isFactory ? factoryPath(i) : filename;
            if (wantedPath.isNotEmpty() && keptAudio[(size_t)i] != nullptr && keptPath[(size_t)i] == wantedPath)
            {
                const juce::ScopedLock g(lock);
                pads[(size_t)i].audio = std::move(keptAudio[(size_t)i]);
                pads[(size_t)i].path = wantedPath;
                pads[(size_t)i].position = 0.0;
                pads[(size_t)i].level = 0.0f;
                loaded = true;
            }
            else if (isFactory && deferFactoryPads)
            {
                const juce::ScopedLock g(lock);
                pads[(size_t)i].path = wantedPath;
                loaded = true;
            }
            else if (isFactory)
                loaded = loadFactoryPad(i);
            else if (filename.isNotEmpty() && deferUserPads)
            {
                // Cached decodes install at once; the rest is only registered and decoded in the background.
                if (auto cached = cachedOnly(juce::File(filename)))
                {
                    const juce::ScopedLock g(lock);
                    pads[(size_t)i].audio = std::move(cached);
                    pads[(size_t)i].position = 0.0;
                    pads[(size_t)i].level = 0.0f;
                }
                const juce::ScopedLock g(lock);
                pads[(size_t)i].path = filename;
                loaded = true;
            }
            else if(filename.isNotEmpty())
                loaded = load(i,juce::File(filename)).wasOk();
            if (!loaded && provideFactoryPad && filename.isEmpty())
            {
                if (deferFactoryPads)
                {
                    const juce::ScopedLock g(lock);
                    pads[(size_t)i].path = factoryPath(i);
                    loaded = true;
                }
                else
                    loaded = loadFactoryPad(i);
            }
            const juce::ScopedLock g(lock);
            if (!isFactory && filename.isNotEmpty()) pads[(size_t)i].path=filename;
            pads[(size_t)i].cc=validCC((int)child.getProperty("cc",-1));
            pads[(size_t)i].ccChannel=juce::jlimit(0,16,(int)child.getProperty("ccChannel",0));
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
    // Decoded user pads are kept in a small least-recently-used cache (by path, size and date) shared by
    // every bank: switching between programs whose pad banks differ no longer decodes the files again
    // (~1.4 s per switch on a laptop CPU, inside the audio lock). The bank still shares the same read-only data.
    struct CacheEntry { juce::String key; std::shared_ptr<SampleData> audio; juce::int64 stamp; };
    static juce::CriticalSection& cacheMutex() { static juce::CriticalSection m; return m; }
    static std::vector<CacheEntry>& cacheStore() { static std::vector<CacheEntry> v; return v; }
    static juce::int64& cacheCounter() { static juce::int64 n = 0; return n; }
    static juce::String cacheKey(const juce::File& file)
    {
        return file.getFullPathName() + "|" + juce::String(file.getSize()) + "|"
             + juce::String(file.getLastModificationTime().toMilliseconds());
    }
    static bool lookupCache(const juce::File& file, std::shared_ptr<SampleData>& result)
    {
        const auto key = cacheKey(file);
        const juce::ScopedLock guard(cacheMutex());
        for (auto& entry : cacheStore())
            if (entry.key == key) { entry.stamp = ++cacheCounter(); result = entry.audio; return true; }
        return false;
    }
    static juce::Result decodeFileCached(const juce::File& file, double maxSeconds,
                                         std::shared_ptr<SampleData>& result)
    {
        using Entry = CacheEntry;
        auto& cacheLock = cacheMutex();
        auto& cache = cacheStore();
        auto& counter = cacheCounter();
        const auto key = cacheKey(file);
        {
            const juce::ScopedLock guard(cacheLock);
            for (auto& entry : cache)
                if (entry.key == key) { entry.stamp = ++counter; result = entry.audio; return juce::Result::ok(); }
        }
        // One decode per file at a time: a second caller (staging prefetch vs. restore) waits and reuses it.
        static juce::CriticalSection inFlightLock;
        static std::map<juce::String, std::shared_ptr<std::mutex>> inFlight;
        std::shared_ptr<std::mutex> keyMutex;
        {
            const juce::ScopedLock guard(inFlightLock);
            auto& slot = inFlight[key];
            if (slot == nullptr) slot = std::make_shared<std::mutex>();
            keyMutex = slot;
        }
        std::lock_guard<std::mutex> decodeGuard(*keyMutex);
        {
            const juce::ScopedLock guard(cacheLock);
            for (auto& entry : cache)
                if (entry.key == key) { entry.stamp = ++counter; result = entry.audio; return juce::Result::ok(); }
        }
        const auto decoded = decodeFile(file, maxSeconds, result);
        if (decoded.failed()) return decoded;
        const juce::ScopedLock guard(cacheLock);
        cache.push_back({ key, result, ++counter });
        constexpr juce::int64 budgetBytes = 192ll * 1024ll * 1024ll;
        const auto totalBytes = [&]
        {
            juce::int64 total = 0;
            for (const auto& entry : cache) total += (juce::int64) entry.audio->frames * 4;
            return total;
        };
        while (cache.size() > 1 && totalBytes() > budgetBytes)
        {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end() - 1; ++it)
                if (it->stamp < oldest->stamp) oldest = it;
            cache.erase(oldest);
        }
        return juce::Result::ok();
    }

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
        auto packed = std::make_shared<SampleData>();
        packed->frames = frames;
        packed->rate = reader->sampleRate;
        packed->stereo.resize((size_t)frames * 2);
        constexpr int chunkFrames = 1 << 16;
        if (!reader->usesFloatingPointData)
        {
            // Integer formats (WAV/AIFF/FLAC/MP3 decoders): read left-justified 32-bit samples in chunks and
            // round straight to 16 bit. This avoids a float copy of the whole file and a per-sample getSample().
            std::vector<int32_t> left((size_t) chunkFrames), right((size_t) chunkFrames);
            int* channels[2] { left.data(), right.data() };
            for (int start = 0; start < frames; start += chunkFrames)
            {
                const int n = juce::jmin(chunkFrames, frames - start);
                if (!reader->read(channels, 2, start, n, true))
                    return juce::Result::fail(juce::String::fromUTF8("Falha ao ler o áudio"));
                auto* out = packed->stereo.data() + (size_t) start * 2;
                for (int i = 0; i < n; ++i)
                {
                    const auto l = (juce::int64) left[(size_t) i] + 0x8000, r = (juce::int64) right[(size_t) i] + 0x8000;
                    out[2 * i] = (int16_t) juce::jlimit<juce::int64>(-32768, 32767, l >> 16);
                    out[2 * i + 1] = (int16_t) juce::jlimit<juce::int64>(-32768, 32767, r >> 16);
                }
            }
            result = std::move(packed);
            return juce::Result::ok();
        }
        juce::AudioBuffer<float> decoded(2, frames);
        if (!reader->read(&decoded, 0, frames, 0, true, true))
            return juce::Result::fail(juce::String::fromUTF8("Falha ao ler o áudio"));
        if (reader->numChannels == 1)
            decoded.copyFrom(1, 0, decoded, 0, 0, frames);
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
                moduleFile = juce::File(juce::String(path.data()));
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

    bool assignLearnedCC(int cc, int channel)
    {
        const int target = learning.exchange(-1);
        if (target < 0) return false;
        for (auto& pad : pads)
            if (pad.cc == cc && (pad.ccChannel == 0 || pad.ccChannel == channel)) pad.cc = -1;
        if (stopCC.load() == cc) stopCC = -1;
        if (target == count) stopCC = cc;
        else if (target < count)
        {
            pads[(size_t) target].cc = cc;
            pads[(size_t) target].ccChannel = channel;
        }
        return true; // Learning must not start/stop audio unexpectedly.
    }

    struct Pad {std::shared_ptr<SampleData> audio;juce::String path;double position=0;float level=0;int cc=-1;int ccChannel=0;};
    static int validCC(int cc){return ((cc>=0&&cc<120&&cc!=64)||(cc>=128&&cc<=255))?cc:-1;}
    static float read(const Pad& p,int ch,double pos)
    {const int i=juce::jlimit(0,p.audio->frames-1,(int)pos);
     const int j=juce::jmin(i+1,p.audio->frames-1);
     const auto a=(float)p.audio->stereo[(size_t)i*2+(size_t)ch]/32768.0f;
     const auto b=(float)p.audio->stereo[(size_t)j*2+(size_t)ch]/32768.0f;
     return a+(b-a)*(float)(pos-i);}
    mutable juce::CriticalSection lock;
    std::array<Pad,count> pads;
    std::array<std::array<bool,128>,16> ccDown{};
    std::atomic<int> command{-2},active{-1},learning{-1},channelFilter{0};
    std::atomic<float> meter{0};std::atomic<double> fade{1.0};
    std::atomic<int> stopCC{-1};double sampleRate=48000;
    std::array<float,2> highPassInput{},highPassOutput{},lowPassState{};
    LayerEqState eq;
    juce::SmoothedValue<float> volume;
};
