// Direct USB audio output: isochronous OUT transfers through usbfs, bypassing Android's audio
// mixer (AudioFlinger / AAudio shared path), whose USB burst is 20 ms on class-compliant
// converters. The Java side (UsbDirectAudio) obtains permission, parses the descriptors, claims
// the interface, selects the alternate setting and programs the sample rate; this file keeps a few
// small URBs in flight and refills each one with freshly rendered audio when the previous one
// completes.
//
// What it handles: any integer PCM layout the descriptors can describe (16/24/32-bit containers,
// mono / stereo / multichannel: the stereo mix goes to channels 1-2), any device sample rate (the
// engine renders at 44.1 kHz; other rates go through a 4-point Hermite resampler, 44.1 kHz is
// passed through untouched), full speed and high speed packet timing, adaptive and synchronous
// endpoints, and asynchronous endpoints paced by the explicit feedback endpoint. Everything sits
// behind a Backend so the scheduling and refill logic is exercised by emulator tests with a fake
// device.
#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef USBDEVFS_GET_SPEED
#define USBDEVFS_GET_SPEED _IO('U', 31)
#endif

bool renderClassicPlayerPcm(int16_t* output, int frames, bool realtime);

namespace {
constexpr int kEngineRate = 44100;
constexpr int kFeedbackIndexBase = 1000;

struct Backend {
    virtual ~Backend() = default;
    virtual bool submit(usbdevfs_urb* urb) = 0;
    /** Blocks until a URB completes. nullptr on a fatal error or after discardAll(). */
    virtual usbdevfs_urb* reap() = 0;
    virtual void discardAll(const std::vector<usbdevfs_urb*>& urbs) = 0;
};

struct UsbfsBackend final : Backend {
    explicit UsbfsBackend(int duplicated) : fd(duplicated) {}
    ~UsbfsBackend() override { if (fd >= 0) close(fd); }
    bool submit(usbdevfs_urb* urb) override { return ioctl(fd, USBDEVFS_SUBMITURB, urb) == 0; }
    usbdevfs_urb* reap() override {
        for (;;) {
            void* completed = nullptr;
            if (ioctl(fd, USBDEVFS_REAPURB, &completed) == 0) return static_cast<usbdevfs_urb*>(completed);
            if (errno == EINTR) continue;
            return nullptr;
        }
    }
    void discardAll(const std::vector<usbdevfs_urb*>& urbs) override {
        for (auto* urb : urbs) ioctl(fd, USBDEVFS_DISCARDURB, urb);
    }
    int fd;
};

/** Frames per packet in 16.16 fixed point with the fractional remainder carried over. */
struct PacketSchedule {
    void setRate(double framesPerPacket) { stepQ16.store(static_cast<int64_t>(std::llround(framesPerPacket * 65536.0))); }
    int next() {
        accumulator += stepQ16.load(std::memory_order_relaxed);
        const int frames = static_cast<int>(accumulator >> 16);
        accumulator &= 0xffff;
        return frames;
    }
    std::atomic<int64_t> stepQ16{0};
    int64_t accumulator = 0;
};

void raisePriority() {
    setpriority(PRIO_PROCESS, 0, -19);
    sched_param param{};
    param.sched_priority = 2;
    sched_setscheduler(0, SCHED_FIFO, &param);   // usually refused for apps; the nice value is the fallback
}

struct Config {
    int endpoint = 0, maxPacketBytes = 0;
    int unitsPerSecond = 1000;      // 1000 full speed, 8000 high speed (frames or microframes per second)
    int packetsPerSecond = 1000;    // data packets per second (service interval)
    int bufferMs = 12;
    int deviceRate = kEngineRate;
    int channels = 2, subframeBytes = 2;
    int feedbackEndpoint = 0, feedbackMaxPacket = 0;
};

struct Stream {
    Stream(std::unique_ptr<Backend> b, const Config& c) : backend(std::move(b)), cfg(c) {
        frameBytes = cfg.channels * cfg.subframeBytes;
        schedule.setRate(static_cast<double>(cfg.deviceRate) / cfg.packetsPerSecond);
        nominalFramesPerPacket = static_cast<double>(cfg.deviceRate) / cfg.packetsPerSecond;
        step = static_cast<double>(kEngineRate) / cfg.deviceRate;
        passthrough = cfg.deviceRate == kEngineRate;
        input.assign(2, 0.f);       // one frame of history so the interpolator can look behind
        inputRead = 1;
        packetsPerUrb = std::max(1, cfg.packetsPerSecond / 500);                 // ~2 ms per URB
        const int urbMs = std::max(1, packetsPerUrb * 1000 / cfg.packetsPerSecond);
        urbCount = std::clamp((cfg.bufferMs + urbMs - 1) / urbMs, 3, 16);
        const size_t descriptorBytes = sizeof(usbdevfs_iso_packet_desc) * static_cast<size_t>(packetsPerUrb);
        for (int i = 0; i < urbCount; ++i) {
            auto* urb = static_cast<usbdevfs_urb*>(calloc(1, sizeof(usbdevfs_urb) + descriptorBytes));
            urb->usercontext = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            urbs.push_back(urb);
            buffers.emplace_back(static_cast<size_t>(cfg.maxPacketBytes) * static_cast<size_t>(packetsPerUrb));
        }
        if (cfg.feedbackEndpoint != 0 && cfg.feedbackMaxPacket >= 3) {
            const size_t fbDescriptors = sizeof(usbdevfs_iso_packet_desc) * kFeedbackPackets;
            for (int i = 0; i < 2; ++i) {
                auto* urb = static_cast<usbdevfs_urb*>(calloc(1, sizeof(usbdevfs_urb) + fbDescriptors));
                urb->usercontext = reinterpret_cast<void*>(static_cast<intptr_t>(kFeedbackIndexBase + i));
                feedbackUrbs.push_back(urb);
                feedbackBuffers.emplace_back(static_cast<size_t>(cfg.feedbackMaxPacket) * kFeedbackPackets);
            }
        }
    }
    ~Stream() {
        for (auto* urb : urbs) free(urb);
        for (auto* urb : feedbackUrbs) free(urb);
    }

    // ----- audio production -------------------------------------------------------------------
    void renderChunk() {
        int16_t chunk[256 * 2];
        if (!renderClassicPlayerPcm(chunk, 256, true)) std::memset(chunk, 0, sizeof(chunk));
        if (inputRead > 4096) {     // drop consumed history, keep one frame behind the read position
            const size_t drop = inputRead - 1;
            input.erase(input.begin(), input.begin() + static_cast<long>(drop * 2));
            inputRead -= drop;
        }
        for (int i = 0; i < 512; ++i) input.push_back(static_cast<float>(chunk[i]) * (1.0f / 32768.0f));
    }
    size_t availableFrames() const { return input.size() / 2 - inputRead; }

    /** Next device-rate stereo frame as floats. */
    void nextFrame(float& left, float& right) {
        if (passthrough) {
            while (availableFrames() < 1) renderChunk();
            left = input[inputRead * 2]; right = input[inputRead * 2 + 1];
            ++inputRead;
            return;
        }
        const size_t whole = static_cast<size_t>(phase);
        while (availableFrames() < whole + 3) renderChunk();
        const double t = phase - static_cast<double>(whole);
        const size_t i1 = inputRead + whole;
        for (int ch = 0; ch < 2; ++ch) {
            const double y0 = input[(i1 - 1) * 2 + ch], y1 = input[i1 * 2 + ch];
            const double y2 = input[(i1 + 1) * 2 + ch], y3 = input[(i1 + 2) * 2 + ch];
            const double c0 = y1, c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            const float value = static_cast<float>(((c3 * t + c2) * t + c1) * t + c0);
            (ch == 0 ? left : right) = value;
        }
        phase += step;
        const size_t moved = static_cast<size_t>(phase);
        inputRead += moved;
        phase -= static_cast<double>(moved);
    }

    void writeSample(unsigned char* out, float value) const {
        double scaled = static_cast<double>(value) * 2147483648.0;
        scaled = std::clamp(scaled, -2147483648.0, 2147483647.0);
        const int32_t v = static_cast<int32_t>(std::llround(scaled));
        const int shift = 32 - 8 * cfg.subframeBytes;
        const int32_t top = v >> shift;
        for (int b = 0; b < cfg.subframeBytes; ++b) out[b] = static_cast<unsigned char>((top >> (8 * b)) & 0xff);
    }

    void writeFrame(unsigned char* out, float left, float right) const {
        if (cfg.channels == 1) { writeSample(out, 0.5f * (left + right)); return; }
        writeSample(out, left);
        writeSample(out + cfg.subframeBytes, right);
        if (cfg.channels > 2) std::memset(out + 2 * cfg.subframeBytes, 0, static_cast<size_t>(cfg.channels - 2) * cfg.subframeBytes);
    }

    /** Renders the next stretch of audio into URB `index` and prepares its packets. */
    void fill(int index) {
        auto* urb = urbs[static_cast<size_t>(index)];
        auto& buffer = buffers[static_cast<size_t>(index)];
        const int maxFrames = cfg.maxPacketBytes / frameBytes;
        int offsetBytes = 0;
        for (int p = 0; p < packetsPerUrb; ++p) {
            const int frames = std::min(schedule.next(), maxFrames);
            for (int f = 0; f < frames; ++f) {
                float left = 0, right = 0;
                nextFrame(left, right);
                writeFrame(buffer.data() + offsetBytes + static_cast<size_t>(f) * frameBytes, left, right);
            }
            urb->iso_frame_desc[p].length = static_cast<unsigned>(frames * frameBytes);
            urb->iso_frame_desc[p].actual_length = 0;
            urb->iso_frame_desc[p].status = 0;
            offsetBytes += frames * frameBytes;
            framesRendered.fetch_add(frames, std::memory_order_relaxed);
        }
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = static_cast<unsigned char>(cfg.endpoint);
        urb->flags = USBDEVFS_URB_ISO_ASAP;
        urb->buffer = buffer.data();
        urb->buffer_length = offsetBytes;
        urb->number_of_packets = packetsPerUrb;
        urb->status = 0;
        urb->error_count = 0;
    }

    // ----- explicit feedback (asynchronous endpoints) -----------------------------------------
    static constexpr int kFeedbackPackets = 8;

    bool submitFeedback(int slot) {
        auto* urb = feedbackUrbs[static_cast<size_t>(slot)];
        auto& buffer = feedbackBuffers[static_cast<size_t>(slot)];
        for (int p = 0; p < kFeedbackPackets; ++p) {
            urb->iso_frame_desc[p].length = static_cast<unsigned>(cfg.feedbackMaxPacket);
            urb->iso_frame_desc[p].actual_length = 0;
            urb->iso_frame_desc[p].status = 0;
        }
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = static_cast<unsigned char>(cfg.feedbackEndpoint);
        urb->flags = USBDEVFS_URB_ISO_ASAP;
        urb->buffer = buffer.data();
        urb->buffer_length = cfg.feedbackMaxPacket * kFeedbackPackets;
        urb->number_of_packets = kFeedbackPackets;
        urb->status = 0;
        urb->error_count = 0;
        return backend->submit(urb);
    }

    void handleFeedback(usbdevfs_urb* urb, int slot) {
        const auto& buffer = feedbackBuffers[static_cast<size_t>(slot)];
        for (int p = 0; p < urb->number_of_packets; ++p) {
            const unsigned length = urb->iso_frame_desc[p].actual_length;
            if (urb->iso_frame_desc[p].status != 0 || length < 3) continue;
            const unsigned char* data = buffer.data() + static_cast<size_t>(p) * cfg.feedbackMaxPacket;
            double framesPerUnit;   // device frames per bus frame (full speed) or microframe (high speed)
            if (length >= 4) framesPerUnit = static_cast<double>(data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24)) / 65536.0;
            else framesPerUnit = static_cast<double>(data[0] | (data[1] << 8) | (data[2] << 16)) / 16384.0;
            const double rate = framesPerUnit * cfg.unitsPerSecond;
            if (rate < cfg.deviceRate * 0.9 || rate > cfg.deviceRate * 1.1) continue;   // implausible: ignore
            estimatedRate = estimatedRate == 0 ? rate : estimatedRate + 0.5 * (rate - estimatedRate);
            schedule.setRate(estimatedRate / cfg.packetsPerSecond);
            feedbackUpdates.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void run() {
        raisePriority();
        for (int i = 0; i < urbCount && running.load(); ++i) {
            fill(i);
            if (!backend->submit(urbs[static_cast<size_t>(i)])) { lastErrno = errno; errors.fetch_add(1); running = false; return; }
            submitted.fetch_add(1, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < feedbackUrbs.size(); ++i) submitFeedback(static_cast<int>(i));
        while (running.load(std::memory_order_acquire)) {
            usbdevfs_urb* done = backend->reap();
            if (done == nullptr) { if (running.load()) { lastErrno = errno; errors.fetch_add(1); } break; }
            if (!running.load(std::memory_order_acquire)) break;
            const int index = static_cast<int>(reinterpret_cast<intptr_t>(done->usercontext));
            if (index >= kFeedbackIndexBase) {
                const int slot = index - kFeedbackIndexBase;
                if (done->status == 0 || done->status == -EXDEV) handleFeedback(done, slot);
                if (running.load() && done->status != -ENOENT) submitFeedback(slot);   // best effort, never fatal
                continue;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            if (done->status != 0 && done->status != -ENOENT) errors.fetch_add(1, std::memory_order_relaxed);
            if (done->error_count > 0) errors.fetch_add(done->error_count, std::memory_order_relaxed);
            fill(index);
            if (!backend->submit(done)) { lastErrno = errno; errors.fetch_add(1); break; }
            submitted.fetch_add(1, std::memory_order_relaxed);
        }
        running = false;
    }

    void start() { running = true; worker = std::thread([this] { run(); }); }
    void stop() {
        running = false;
        std::vector<usbdevfs_urb*> all = urbs;
        all.insert(all.end(), feedbackUrbs.begin(), feedbackUrbs.end());
        backend->discardAll(all);   // completes every pending URB so the blocked reap() returns
        if (worker.joinable()) worker.join();
    }

    std::unique_ptr<Backend> backend;
    Config cfg;
    int frameBytes = 4, packetsPerUrb = 2, urbCount = 6;
    double nominalFramesPerPacket = 44.1, step = 1.0, phase = 0.0, estimatedRate = 0.0;
    bool passthrough = true;
    PacketSchedule schedule;
    std::vector<float> input;       // interleaved stereo at the engine rate
    size_t inputRead = 1;           // frame index of the current position (frame 0 is history)
    std::vector<usbdevfs_urb*> urbs, feedbackUrbs;
    std::vector<std::vector<unsigned char>> buffers, feedbackBuffers;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int64_t> framesRendered{0}, submitted{0}, completed{0}, errors{0}, feedbackUpdates{0};
    int lastErrno = 0;
};

std::mutex streamMutex;
std::unique_ptr<Stream> activeStream;

/** Test double: completes URBs in order, each after the time its audio would take to play. */
struct FakeBackend final : Backend {
    FakeBackend(int rate, int bytesPerFrame, int channelBytes, double feedbackFramesPerUnit, int unitsPerSecond, bool feedbackFourBytes)
        : deviceRate(rate), frameBytes(bytesPerFrame), subframeBytes(channelBytes), feedbackValue(feedbackFramesPerUnit),
          units(unitsPerSecond), fourBytes(feedbackFourBytes) {}
    bool submit(usbdevfs_urb* urb) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (urb->endpoint & 0x80) {
            feedbackQueue.push_back({urb, std::chrono::steady_clock::now() + std::chrono::milliseconds(8)});
            return true;
        }
        queue.push_back(urb);
        int frames = 0, longest = 0;
        for (int p = 0; p < urb->number_of_packets; ++p) {
            const int length = static_cast<int>(urb->iso_frame_desc[p].length);
            frames += length / frameBytes;
            longest = std::max(longest, length);
        }
        const auto* bytes = static_cast<const unsigned char*>(urb->buffer);
        bool silent = true;
        const int total = urb->buffer_length / frameBytes;
        for (int f = 0; f < total; ++f) {
            for (int b = 0; b < frameBytes; ++b) {
                if (bytes[f * frameBytes + b] == 0) continue;
                if (b < 2 * subframeBytes) silent = false; else extraChannelBytes++;
            }
        }
        framesSeen += frames;
        longestPacket = std::max(longestPacket, longest);
        if (!silent) nonSilentUrbs++;
        return true;
    }
    usbdevfs_urb* reap() override {
        for (;;) {
            if (stopped.load()) return nullptr;
            usbdevfs_urb* urb = nullptr;
            int frames = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!feedbackQueue.empty() && feedbackQueue.front().second <= std::chrono::steady_clock::now()) {
                    urb = feedbackQueue.front().first;
                    feedbackQueue.erase(feedbackQueue.begin());
                    auto* data = static_cast<unsigned char*>(urb->buffer);
                    const int max = static_cast<int>(urb->iso_frame_desc[0].length);
                    for (int p = 0; p < urb->number_of_packets; ++p) {
                        unsigned char* d = data + static_cast<size_t>(p) * max;
                        if (fourBytes) {
                            const uint32_t v = static_cast<uint32_t>(std::llround(feedbackValue * 65536.0));
                            d[0] = v & 0xff; d[1] = (v >> 8) & 0xff; d[2] = (v >> 16) & 0xff; d[3] = (v >> 24) & 0xff;
                            urb->iso_frame_desc[p].actual_length = 4;
                        } else {
                            const uint32_t v = static_cast<uint32_t>(std::llround(feedbackValue * 16384.0));
                            d[0] = v & 0xff; d[1] = (v >> 8) & 0xff; d[2] = (v >> 16) & 0xff;
                            urb->iso_frame_desc[p].actual_length = 3;
                        }
                        urb->iso_frame_desc[p].status = 0;
                    }
                    urb->status = 0;
                    feedbackCompleted++;
                    return urb;
                }
                if (!queue.empty()) {
                    urb = queue.front();
                    for (int p = 0; p < urb->number_of_packets; ++p)
                        frames += static_cast<int>(urb->iso_frame_desc[p].length) / frameBytes;
                }
            }
            if (urb != nullptr) {
                // Real time passes while the URB "plays".
                timespec ts{0, static_cast<long>(static_cast<int64_t>(frames) * 1000000000LL / deviceRate)};
                nanosleep(&ts, nullptr);
                std::lock_guard<std::mutex> lock(mutex);
                if (!queue.empty() && queue.front() == urb) queue.erase(queue.begin());
                return urb;
            }
            usleep(200);
        }
    }
    void discardAll(const std::vector<usbdevfs_urb*>&) override { stopped = true; }
    int deviceRate, frameBytes, subframeBytes;
    double feedbackValue; int units; bool fourBytes;
    std::mutex mutex;
    std::vector<usbdevfs_urb*> queue;
    std::vector<std::pair<usbdevfs_urb*, std::chrono::steady_clock::time_point>> feedbackQueue;
    std::atomic<bool> stopped{false};
    int64_t framesSeen = 0, extraChannelBytes = 0, feedbackCompleted = 0;
    int longestPacket = 0, nonSilentUrbs = 0;
};
}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeStart(JNIEnv*, jclass, jint fd, jint endpoint, jint maxPacket,
                                                              jint packetsPerSecond, jint bufferMs, jint deviceRate,
                                                              jint channels, jint subframeBytes, jint feedbackEndpoint,
                                                              jint feedbackMaxPacket, jint unitsPerSecond) {
    std::lock_guard<std::mutex> lock(streamMutex);
    if (activeStream) { activeStream->stop(); activeStream.reset(); }
    if (fd < 0 || channels < 1 || channels > 32 || subframeBytes < 2 || subframeBytes > 4 || deviceRate < 4000 || deviceRate > 400000 ||
        packetsPerSecond < 1 || unitsPerSecond < 1)
        return JNI_FALSE;
    if (maxPacket < channels * subframeBytes) return JNI_FALSE;
    const int duplicated = dup(fd);   // the Java UsbDeviceConnection keeps its own descriptor
    if (duplicated < 0) return JNI_FALSE;
    Config cfg;
    cfg.endpoint = endpoint; cfg.maxPacketBytes = maxPacket; cfg.packetsPerSecond = packetsPerSecond;
    cfg.unitsPerSecond = unitsPerSecond; cfg.bufferMs = std::max(2, static_cast<int>(bufferMs)); cfg.deviceRate = deviceRate;
    cfg.channels = channels; cfg.subframeBytes = subframeBytes;
    cfg.feedbackEndpoint = feedbackEndpoint; cfg.feedbackMaxPacket = feedbackMaxPacket;
    activeStream = std::make_unique<Stream>(std::make_unique<UsbfsBackend>(duplicated), cfg);
    activeStream->start();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeSpeed(JNIEnv*, jclass, jint fd) {
    return fd < 0 ? -1 : ioctl(fd, USBDEVFS_GET_SPEED, 0);   // 1 low, 2 full, 3 high, 5 super, 6 super+
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeStop(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(streamMutex);
    if (activeStream) { activeStream->stop(); activeStream.reset(); }
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeInfo(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(streamMutex);
    jlong values[9] = {};
    if (activeStream) {
        values[0] = activeStream->running.load() ? 1 : 0;
        values[1] = activeStream->submitted.load();
        values[2] = activeStream->completed.load();
        values[3] = activeStream->errors.load();
        values[4] = activeStream->lastErrno;
        values[5] = activeStream->urbCount;
        values[6] = activeStream->packetsPerUrb;
        values[7] = activeStream->framesRendered.load();
        values[8] = activeStream->feedbackUpdates.load();
    }
    jlongArray result = env->NewLongArray(9);
    env->SetLongArrayRegion(result, 0, 9, values);
    return result;
}

// {frames the fake device received, longest packet in bytes, URBs with audio, errors, URBs completed,
//  packets submitted, nonzero bytes in channels beyond the stereo pair, feedback updates applied}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeSelfTestEx(JNIEnv* env, jclass, jint milliseconds, jint rate, jint channels,
                                                                   jint subframeBytes, jdouble feedbackFramesPerUnit, jint unitsPerSecond,
                                                                   jint packetsPerSecond, jboolean feedbackFourBytes) {
    Config cfg;
    cfg.endpoint = 1; cfg.deviceRate = rate; cfg.channels = channels; cfg.subframeBytes = subframeBytes;
    cfg.unitsPerSecond = unitsPerSecond; cfg.packetsPerSecond = packetsPerSecond; cfg.bufferMs = 12;
    const int frameBytes = channels * subframeBytes;
    cfg.maxPacketBytes = ((rate + packetsPerSecond - 1) / packetsPerSecond + 2) * frameBytes;
    if (feedbackFramesPerUnit > 0) { cfg.feedbackEndpoint = 0x81; cfg.feedbackMaxPacket = 4; }
    auto backendOwner = std::make_unique<FakeBackend>(rate, frameBytes, subframeBytes, feedbackFramesPerUnit, unitsPerSecond, feedbackFourBytes == JNI_TRUE);
    auto* fake = backendOwner.get();
    Stream stream(std::move(backendOwner), cfg);
    stream.start();
    usleep(static_cast<useconds_t>(std::max(100, static_cast<int>(milliseconds))) * 1000);
    stream.stop();
    jlong values[8] = {fake->framesSeen, fake->longestPacket, fake->nonSilentUrbs, stream.errors.load(), stream.completed.load(),
                       stream.submitted.load() * stream.packetsPerUrb, fake->extraChannelBytes, stream.feedbackUpdates.load()};
    jlongArray result = env->NewLongArray(8);
    env->SetLongArrayRegion(result, 0, 8, values);
    return result;
}

// The original 44.1 kHz stereo 16-bit full-speed test, kept for the existing emulator test.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeSelfTest(JNIEnv* env, jclass cls, jint milliseconds) {
    jlongArray full = Java_com_classickeys_classicplayer_UsbDirectAudio_nativeSelfTestEx(env, cls, milliseconds, 44100, 2, 2, 0.0, 1000, 1000, JNI_FALSE);
    jlong values[8];
    env->GetLongArrayRegion(full, 0, 8, values);
    jlongArray result = env->NewLongArray(6);
    env->SetLongArrayRegion(result, 0, 6, values);
    return result;
}
