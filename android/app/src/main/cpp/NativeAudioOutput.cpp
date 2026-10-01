#include <jni.h>
#include <oboe/Oboe.h>
#include <oboe/LatencyTuner.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <time.h>

// Shared by the offline regression renderer and the actual device callback.
bool renderClassicPlayerPcm(int16_t* output, int frames,bool realtime);

namespace {
class OutputCallback final : public oboe::AudioStreamDataCallback,
                             public oboe::AudioStreamErrorCallback {
public:
    explicit OutputCallback(int mode) : sampleRateMode(mode) {
        if(sampleRateMode==2)for(int phase=0;phase<kPhases;++phase){
            double sum=0.0;
            for(int tap=0;tap<kTaps;++tap){
                const double x=(tap-kHalfTaps+1)-static_cast<double>(phase)/kPhases;
                const double sinc=std::abs(x)<1.0e-9?1.0:std::sin(kPi*x)/(kPi*x);
                const double window=0.5-0.5*std::cos(2.0*kPi*(tap+0.5)/kTaps);
                coefficients[phase][tap]=static_cast<float>(sinc*window);
                sum+=coefficients[phase][tap];
            }
            for(float& coefficient:coefficients[phase])coefficient=static_cast<float>(coefficient/sum);
        }
    }
    static constexpr int kTaps=32,kHalfTaps=kTaps/2,kPhases=512,kSourceRing=1024;
    static constexpr double kPi=3.14159265358979323846;
    const int sampleRateMode;
    std::array<std::array<float,kTaps>,kPhases> coefficients{};
    std::array<int16_t,kSourceRing*2> sourceRing{};
    int64_t sourceFrames=0;
    double sourcePosition=0.0;
    std::atomic<int> error{0};
    std::atomic<int> contentions{0};
    std::atomic<int> callbackMicros{0};
    std::atomic<int> maxCallbackMicros{0};
    std::atomic<int> callbackFrames{0};
    // Owned for the full stream lifetime. Oboe recommends tuning from the
    // data callback; this avoids a Java/JNI hop or a control-thread race.
    std::unique_ptr<oboe::LatencyTuner> latencyTuner;
    int16_t lastLeft=0,lastRight=0;
    bool faded=false;
    void renderResampled(int16_t* output,int32_t frames){
        constexpr int kRenderQuantum=256;
        std::array<int16_t,kRenderQuantum*2> block{};
        for(int frame=0;frame<frames;++frame){
            const int64_t center=static_cast<int64_t>(sourcePosition);
            while(sourceFrames<=center+kHalfTaps){
                const int count=std::min<int64_t>(kRenderQuantum,center+kHalfTaps+1-sourceFrames);
                if(!renderClassicPlayerPcm(block.data(),count,true)){
                    ++contentions;
                    block.fill(0);
                }
                for(int i=0;i<count;++i){
                    const size_t destination=static_cast<size_t>((sourceFrames+i)%kSourceRing)*2;
                    sourceRing[destination]=block[(size_t)i*2];
                    sourceRing[destination+1]=block[(size_t)i*2+1];
                }
                sourceFrames+=count;
            }
            const double fraction=sourcePosition-center;
            const int phase=std::min(kPhases-1,static_cast<int>(fraction*kPhases));
            for(int channel=0;channel<2;++channel){
                double value=0.0;
                for(int tap=0;tap<kTaps;++tap){
                    const int64_t index=center+tap-kHalfTaps+1;
                    if(index>=0)value+=coefficients[phase][tap]*sourceRing[static_cast<size_t>(index%kSourceRing)*2+channel];
                }
                output[(size_t)frame*2+channel]=static_cast<int16_t>(std::clamp(std::lround(value),-32768L,32767L));
            }
            sourcePosition+=44100.0/48000.0;
        }
        lastLeft=output[(size_t)(frames-1)*2];lastRight=output[(size_t)(frames-1)*2+1];
    }
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t frames) override {
        timespec started{};
        clock_gettime(CLOCK_MONOTONIC, &started);
        auto* output=static_cast<int16_t*>(data);
        if(sampleRateMode==2){
            renderResampled(output,frames);
        }else{
        // Keep MIDI response within 256 frames (5.8 ms at 44.1 kHz), while
        // avoiding repeated DSP setup for every 128 frames. In particular,
        // USB callbacks can contain 882/960 frames and were doing 7/8 setups.
        constexpr int kRenderQuantum=256;
        for(int offset=0;offset<frames;offset+=kRenderQuantum){
            const int count=std::min(kRenderQuantum,frames-offset);
            auto* block=output+offset*2;
            if(!renderClassicPlayerPcm(block,count,true)){
                ++contentions;
                for(int i=0;i<count;++i){
                    const float gain=1.0f-static_cast<float>(i+1)/count;
                    block[i*2]=static_cast<int16_t>(lastLeft*gain);
                    block[i*2+1]=static_cast<int16_t>(lastRight*gain);
                }
                faded=true;
            }else if(faded){
                for(int i=0;i<std::min(16,count);++i){block[i*2]=block[i*2]*(i+1)/16;block[i*2+1]=block[i*2+1]*(i+1)/16;}
                faded=false;
            }
            lastLeft=block[(count-1)*2];lastRight=block[(count-1)*2+1];
        }
        }
        if(latencyTuner)latencyTuner->tune();
        timespec finished{};
        clock_gettime(CLOCK_MONOTONIC, &finished);
        const int64_t elapsedNanos=(finished.tv_sec-started.tv_sec)*1000000000LL+
                (finished.tv_nsec-started.tv_nsec);
        const int elapsedMicros=static_cast<int>(std::max<int64_t>(0,elapsedNanos/1000));
        callbackMicros.store(elapsedMicros,std::memory_order_relaxed);
        int maxMicros=maxCallbackMicros.load(std::memory_order_relaxed);
        while(elapsedMicros>maxMicros &&
              !maxCallbackMicros.compare_exchange_weak(maxMicros,elapsedMicros,
                                                       std::memory_order_relaxed)) { }
        callbackFrames.store(frames,std::memory_order_relaxed);
        return oboe::DataCallbackResult::Continue;
    }
    void onErrorAfterClose(oboe::AudioStream*,oboe::Result result) override {
        error.store(static_cast<int>(result));
    }
};
std::mutex outputMutex;
std::shared_ptr<oboe::AudioStream> stream;
std::shared_ptr<OutputCallback> callback;

void closeOutput() {
    if(stream){stream->requestStop();stream->close();stream.reset();}
    callback.reset();
}

oboe::Result openOutputStream(jint deviceId, oboe::SharingMode sharingMode, int sampleRateMode,
                              const std::shared_ptr<OutputCallback>& dataCallback,
                              std::shared_ptr<oboe::AudioStream>& openedStream) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(2);
    // Reserve enough capacity for the 4,800-frame setting used by Numa Player.
    // AAudio's capacity is fixed at open; setBufferSizeInFrames() cannot grow
    // beyond it later, which previously capped this USB route at 1,792 frames.
    builder.setBufferCapacityInFrames(4800);
    // The synth always renders at 44.1 kHz. Native mode lets Oboe convert to
    // the endpoint rate; explicit 48 kHz uses the callback's band-limited SRC
    // before handing samples to Oboe. Never reinterpret 44.1 kHz PCM as 48 kHz.
    builder.setSampleRate(sampleRateMode==2?48000:44100);
    builder.setSampleRateConversionQuality(sampleRateMode!=1
        ? oboe::SampleRateConversionQuality::High
        : oboe::SampleRateConversionQuality::None);
    builder.setFormatConversionAllowed(true);
    builder.setChannelConversionAllowed(true);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(sharingMode);
    // Treat the synth as media/music. Some Android vendor policies route GAME
    // usage to the built-in speaker even when a USB output device was requested.
    builder.setUsage(oboe::Usage::Media);
    builder.setContentType(oboe::ContentType::Music);
    if(deviceId>0)builder.setDeviceId(deviceId);
    builder.setDataCallback(dataCallback);
    builder.setErrorCallback(dataCallback);
    return builder.openStream(openedStream);
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeStartOutput(JNIEnv*,jclass,jint deviceId,jint bufferFrames,jint sampleRateMode) {
    std::lock_guard<std::mutex> lock(outputMutex);
    closeOutput();
    const int mode=std::clamp(static_cast<int>(sampleRateMode),0,2);
    auto nextCallback=std::make_shared<OutputCallback>(mode);
    // Try exclusive low-latency first. Some Android routes silently downgrade
    // a successful exclusive request to Shared/None instead of failing open;
    // detect that and explicitly retry Shared/LowLatency before accepting the
    // downgraded stream. This matters for both built-in and USB outputs.
    auto result=openOutputStream(deviceId,oboe::SharingMode::Exclusive,mode,nextCallback,stream);
    bool retryShared=result!=oboe::Result::OK;
    if(result==oboe::Result::OK &&
       (stream->getPerformanceMode()!=oboe::PerformanceMode::LowLatency ||
        (deviceId>0&&stream->getDeviceId()!=deviceId))){
        // Some USB endpoints only accept explicit routing in shared mode.
        retryShared=true;
        stream->close();stream.reset();
    }
    if(retryShared){
        if(stream){stream->close();stream.reset();}
        result=openOutputStream(deviceId,oboe::SharingMode::Shared,mode,nextCallback,stream);
    }
    if(result!=oboe::Result::OK){
        if(stream){stream->close();stream.reset();}
        return JNI_FALSE;
    }
    // Never report a successful explicit selection if Android silently opened
    // the default output instead of the requested USB endpoint.
    if(deviceId>0&&stream->getDeviceId()!=deviceId){closeOutput();return JNI_FALSE;}
    // OpenSL ES cannot select an explicit USB device. Let the Java fallback
    // preserve that selection rather than silently routing to the speaker.
    if(deviceId!=0&&stream->getAudioApi()!=oboe::AudioApi::AAudio){closeOutput();return JNI_FALSE;}
    callback=nextCallback;
    // Start at the selected size, rounded to the route's burst granularity.
    // Never impose an extra two-burst floor just because Android selected its
    // standard path: that made the user's 128/256/512 choices indistinguishable.
    // LatencyTuner raises the buffer by one burst only if xruns are observed.
    const int32_t burst=stream->getFramesPerBurst();
    const int32_t capacity=stream->getBufferCapacityInFrames();
    int64_t target=std::max<int64_t>(1,bufferFrames);
    if(burst>0){
        target=std::max<int64_t>(target,static_cast<int64_t>(burst));
        target=((target+burst-1)/burst)*burst;
    }
    if(capacity>0)target=std::min<int64_t>(target,capacity);
    if(burst>0&&capacity>0&&stream->getAudioApi()==oboe::AudioApi::AAudio){
        nextCallback->latencyTuner=std::make_unique<oboe::LatencyTuner>(*stream,capacity);
        nextCallback->latencyTuner->setMinimumBufferSize(static_cast<int32_t>(target));
        nextCallback->latencyTuner->setBufferSizeIncrement(burst);
        nextCallback->latencyTuner->requestReset();
    }
    stream->setBufferSizeInFrames(static_cast<int32_t>(target));
    result=stream->requestStart();
    if(result!=oboe::Result::OK){closeOutput();return JNI_FALSE;}
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeSetOutputDefaults(JNIEnv*,jclass,jint rate,jint burst) {
    // Keep Oboe's defaults aligned with the actual native renderer, rather than
    // Android's hardware-preferred rate (often 48 kHz).
    if(rate>0)oboe::DefaultStreamValues::SampleRate=rate;
    if(burst>0)oboe::DefaultStreamValues::FramesPerBurst=burst;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeStopOutput(JNIEnv*,jclass) {
    std::lock_guard<std::mutex> lock(outputMutex);closeOutput();
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeOutputInfo(JNIEnv* env,jclass) {
    std::lock_guard<std::mutex> lock(outputMutex);
    // rate, effective buffer, burst, device, API, performance, sharing, underruns, error,
    // lock contentions, buffer capacity, latest callback duration in micros,
    // callback frame count, and peak callback duration since this stream opened.
    jint values[14]={};
    if(stream){
        const auto xruns=stream->getXRunCount();
        values[0]=stream->getSampleRate();values[1]=stream->getBufferSizeInFrames();
        values[2]=stream->getFramesPerBurst();values[3]=stream->getDeviceId();
        values[4]=static_cast<int>(stream->getAudioApi());
        values[5]=static_cast<int>(stream->getPerformanceMode());
        values[6]=static_cast<int>(stream->getSharingMode());
        values[7]=xruns?xruns.value():-1;values[8]=callback?callback->error.load():0;
        values[9]=callback?callback->contentions.load():0;
        values[10]=stream->getBufferCapacityInFrames();
        if(callback){
            values[11]=callback->callbackMicros.load(std::memory_order_relaxed);
            values[12]=callback->callbackFrames.load(std::memory_order_relaxed);
            values[13]=callback->maxCallbackMicros.load(std::memory_order_relaxed);
        }
    }
    auto result=env->NewIntArray(14);if(result)env->SetIntArrayRegion(result,0,14,values);return result;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeOutputLatency(JNIEnv*,jclass) {
    std::lock_guard<std::mutex> lock(outputMutex);
    if(!stream||!callback||callback->error.load()!=0)return -1;
    auto result=stream->calculateLatencyMillis();return result?result.value():-1;
}
