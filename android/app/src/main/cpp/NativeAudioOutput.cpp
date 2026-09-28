#include <jni.h>
#include <oboe/Oboe.h>
#include <oboe/LatencyTuner.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

// Shared by the offline regression renderer and the actual device callback.
bool renderClassicPlayerPcm(int16_t* output, int frames,bool realtime);

namespace {
class OutputCallback final : public oboe::AudioStreamDataCallback,
                             public oboe::AudioStreamErrorCallback {
public:
    std::atomic<int> error{0};
    std::atomic<int> contentions{0};
    // Owned for the full stream lifetime. Oboe recommends tuning from the
    // data callback; this avoids a Java/JNI hop or a control-thread race.
    std::unique_ptr<oboe::LatencyTuner> latencyTuner;
    int16_t lastLeft=0,lastRight=0;
    bool faded=false;
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t frames) override {
        auto* output=static_cast<int16_t*>(data);
        // Consume MIDI at least every 128 frames, without Java array copies or
        // a blocking write queue ahead of the hardware's own audio callback.
        for(int offset=0;offset<frames;offset+=128){
            const int count=std::min(128,frames-offset);
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
        if(latencyTuner)latencyTuner->tune();
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

oboe::Result openOutputStream(jint deviceId, oboe::SharingMode sharingMode,
                              const std::shared_ptr<OutputCallback>& dataCallback,
                              std::shared_ptr<oboe::AudioStream>& openedStream) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(2);
    // The synth and callback stay at 48 kHz. Oboe's medium-quality SRC handles
    // endpoints such as the CK61 that expose a 44.1 kHz USB stream.
    builder.setSampleRate(48000);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
    builder.setFormatConversionAllowed(true);
    builder.setChannelConversionAllowed(true);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(sharingMode);
    builder.setUsage(oboe::Usage::Game);
    builder.setContentType(oboe::ContentType::Music);
    if(deviceId>0)builder.setDeviceId(deviceId);
    builder.setDataCallback(dataCallback);
    builder.setErrorCallback(dataCallback);
    return builder.openStream(openedStream);
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeStartOutput(JNIEnv*,jclass,jint deviceId,jint bufferFrames) {
    std::lock_guard<std::mutex> lock(outputMutex);
    closeOutput();
    auto nextCallback=std::make_shared<OutputCallback>();
    // Try exclusive low-latency first. Some Android routes silently downgrade
    // a successful exclusive request to Shared/None instead of failing open;
    // detect that and explicitly retry Shared/LowLatency before accepting the
    // downgraded stream. This matters for both built-in and USB outputs.
    auto result=openOutputStream(deviceId,oboe::SharingMode::Exclusive,nextCallback,stream);
    if(result==oboe::Result::OK &&
       stream->getPerformanceMode()!=oboe::PerformanceMode::LowLatency){
        stream->close();stream.reset();
        result=openOutputStream(deviceId,oboe::SharingMode::Shared,nextCallback,stream);
    }else if(result!=oboe::Result::OK){
        if(stream){stream->close();stream.reset();}
        result=openOutputStream(deviceId,oboe::SharingMode::Shared,nextCallback,stream);
    }
    if(result!=oboe::Result::OK){
        if(stream){stream->close();stream.reset();}
        return JNI_FALSE;
    }
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
    // rate, buffer, burst, device, API, performance, sharing, underruns, error,
    // lock contentions, buffer capacity.
    jint values[11]={};
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
    }
    auto result=env->NewIntArray(11);if(result)env->SetIntArrayRegion(result,0,11,values);return result;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_classickeys_classicplayer_PolySynthEngine_nativeOutputLatency(JNIEnv*,jclass) {
    std::lock_guard<std::mutex> lock(outputMutex);
    if(!stream||!callback||callback->error.load()!=0)return -1;
    auto result=stream->calculateLatencyMillis();return result?result.value():-1;
}
