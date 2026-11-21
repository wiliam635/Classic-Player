// Direct USB audio output: isochronous OUT transfers through usbdevfs, bypassing Android's
// audio mixer (AudioFlinger / AAudio shared path), whose USB burst is 20 ms on class-compliant
// converters. The Java side (UsbDirectAudio) obtains permission, claims the interface and
// selects the alternate setting; this file only keeps a few small URBs in flight and refills
// each one with freshly rendered audio when the previous one completes.
//
// Scope of this first driver: UAC1-style 16-bit stereo, engine rate (44.1 kHz) only, adaptive or
// synchronous endpoints (no explicit feedback endpoint). Everything is behind a Backend so the
// scheduling and refill logic is exercised by an emulator test with a fake device.
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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

bool renderClassicPlayerPcm(int16_t* output, int frames, bool realtime);

namespace {
constexpr int kEngineRate = 44100;
constexpr int kBytesPerFrame = 4;   // stereo, 16-bit

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

/** Exact number of frames per packet, carrying the fractional remainder (44100/1000 -> 44,44,...,45). */
struct PacketSchedule {
    PacketSchedule(int rate, int packetsPerSecond) : numerator(rate), denominator(packetsPerSecond) {}
    int next() {
        remainder += numerator;
        const int frames = static_cast<int>(remainder / denominator);
        remainder -= static_cast<int64_t>(frames) * denominator;
        return frames;
    }
    int64_t numerator, denominator, remainder = 0;
};

void raisePriority() {
    setpriority(PRIO_PROCESS, 0, -19);
    sched_param param{};
    param.sched_priority = 2;
    sched_setscheduler(0, SCHED_FIFO, &param);   // usually refused for apps; the nice value is the fallback
}

struct Stream {
    Stream(std::unique_ptr<Backend> b, int endpoint, int maxPacket, int packetsPerSecond, int bufferMs)
        : backend(std::move(b)), endpointAddress(endpoint), maxPacketBytes(maxPacket),
          schedule(kEngineRate, packetsPerSecond) {
        packetsPerUrb = std::max(1, packetsPerSecond / 500);                 // ~2 ms per URB
        const int urbMs = std::max(1, packetsPerUrb * 1000 / packetsPerSecond);
        urbCount = std::clamp((bufferMs + urbMs - 1) / urbMs, 3, 16);
        const size_t descriptorBytes = sizeof(usbdevfs_iso_packet_desc) * static_cast<size_t>(packetsPerUrb);
        for (int i = 0; i < urbCount; ++i) {
            auto* urb = static_cast<usbdevfs_urb*>(calloc(1, sizeof(usbdevfs_urb) + descriptorBytes));
            urb->usercontext = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            urbs.push_back(urb);
            buffers.emplace_back(static_cast<size_t>(maxPacketBytes) * static_cast<size_t>(packetsPerUrb));
        }
    }
    ~Stream() { for (auto* urb : urbs) free(urb); }

    /** Renders the next stretch of audio into URB `index` and prepares its packets. */
    void fill(int index) {
        auto* urb = urbs[static_cast<size_t>(index)];
        auto& buffer = buffers[static_cast<size_t>(index)];
        int offsetBytes = 0;
        for (int p = 0; p < packetsPerUrb; ++p) {
            int frames = schedule.next();
            frames = std::min(frames, maxPacketBytes / kBytesPerFrame);
            auto* destination = reinterpret_cast<int16_t*>(buffer.data() + offsetBytes);
            int done = 0;
            while (done < frames) {
                const int count = std::min(256, frames - done);
                if (!renderClassicPlayerPcm(destination + done * 2, count, true))
                    std::memset(destination + done * 2, 0, static_cast<size_t>(count) * kBytesPerFrame);
                done += count;
            }
            urb->iso_frame_desc[p].length = static_cast<unsigned>(frames * kBytesPerFrame);
            urb->iso_frame_desc[p].actual_length = 0;
            urb->iso_frame_desc[p].status = 0;
            offsetBytes += frames * kBytesPerFrame;
            framesRendered.fetch_add(frames, std::memory_order_relaxed);
        }
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = static_cast<unsigned char>(endpointAddress);
        urb->flags = USBDEVFS_URB_ISO_ASAP;
        urb->buffer = buffer.data();
        urb->buffer_length = offsetBytes;
        urb->number_of_packets = packetsPerUrb;
        urb->status = 0;
        urb->error_count = 0;
    }

    void run() {
        raisePriority();
        for (int i = 0; i < urbCount && running.load(); ++i) {
            fill(i);
            if (!backend->submit(urbs[static_cast<size_t>(i)])) { lastErrno = errno; errors.fetch_add(1); running = false; return; }
            submitted.fetch_add(1, std::memory_order_relaxed);
        }
        while (running.load(std::memory_order_acquire)) {
            usbdevfs_urb* done = backend->reap();
            if (done == nullptr) { if (running.load()) { lastErrno = errno; errors.fetch_add(1); } break; }
            if (!running.load(std::memory_order_acquire)) break;
            completed.fetch_add(1, std::memory_order_relaxed);
            if (done->status != 0 && done->status != -ENOENT) errors.fetch_add(1, std::memory_order_relaxed);
            if (done->error_count > 0) errors.fetch_add(done->error_count, std::memory_order_relaxed);
            const int index = static_cast<int>(reinterpret_cast<intptr_t>(done->usercontext));
            fill(index);
            if (!backend->submit(done)) { lastErrno = errno; errors.fetch_add(1); break; }
            submitted.fetch_add(1, std::memory_order_relaxed);
        }
        running = false;
    }

    void start() { running = true; worker = std::thread([this] { run(); }); }
    void stop() {
        running = false;
        backend->discardAll(urbs);   // completes every pending URB so the blocked reap() returns
        if (worker.joinable()) worker.join();
    }

    std::unique_ptr<Backend> backend;
    int endpointAddress, maxPacketBytes, packetsPerUrb = 2, urbCount = 6;
    PacketSchedule schedule;
    std::vector<usbdevfs_urb*> urbs;
    std::vector<std::vector<unsigned char>> buffers;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int64_t> framesRendered{0}, submitted{0}, completed{0}, errors{0};
    int lastErrno = 0;
};

std::mutex streamMutex;
std::unique_ptr<Stream> activeStream;

/** Test double: completes URBs in order, each after the time its audio would take to play. */
struct FakeBackend final : Backend {
    bool submit(usbdevfs_urb* urb) override {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(urb);
        int frames = 0, longest = 0;
        bool silent = true;
        for (int p = 0; p < urb->number_of_packets; ++p) {
            const int length = static_cast<int>(urb->iso_frame_desc[p].length);
            frames += length / kBytesPerFrame;
            longest = std::max(longest, length);
        }
        const auto* samples = static_cast<const int16_t*>(urb->buffer);
        for (int i = 0; i < urb->buffer_length / 2; ++i) if (samples[i] != 0) { silent = false; break; }
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
                if (!queue.empty()) {
                    urb = queue.front();
                    for (int p = 0; p < urb->number_of_packets; ++p)
                        frames += static_cast<int>(urb->iso_frame_desc[p].length) / kBytesPerFrame;
                }
            }
            if (urb != nullptr) {
                // Real time passes while the URB "plays".
                timespec ts{0, static_cast<long>(static_cast<int64_t>(frames) * 1000000000LL / kEngineRate)};
                nanosleep(&ts, nullptr);
                std::lock_guard<std::mutex> lock(mutex);
                if (!queue.empty() && queue.front() == urb) queue.erase(queue.begin());
                return urb;
            }
            usleep(200);
        }
    }
    void discardAll(const std::vector<usbdevfs_urb*>&) override { stopped = true; }
    std::mutex mutex;
    std::vector<usbdevfs_urb*> queue;
    std::atomic<bool> stopped{false};
    int64_t framesSeen = 0;
    int longestPacket = 0, nonSilentUrbs = 0;
};
}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeStart(JNIEnv*, jclass, jint fd, jint endpoint,
                                                              jint maxPacket, jint packetsPerSecond, jint bufferMs) {
    std::lock_guard<std::mutex> lock(streamMutex);
    if (activeStream) { activeStream->stop(); activeStream.reset(); }
    if (fd < 0 || maxPacket < kBytesPerFrame * 40 || packetsPerSecond < 1) return JNI_FALSE;
    const int duplicated = dup(fd);   // the Java UsbDeviceConnection keeps its own descriptor
    if (duplicated < 0) return JNI_FALSE;
    activeStream = std::make_unique<Stream>(std::make_unique<UsbfsBackend>(duplicated), endpoint, maxPacket,
                                            packetsPerSecond, std::max(2, bufferMs));
    activeStream->start();
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeStop(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(streamMutex);
    if (activeStream) { activeStream->stop(); activeStream.reset(); }
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeInfo(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(streamMutex);
    jlong values[8] = {};
    if (activeStream) {
        values[0] = activeStream->running.load() ? 1 : 0;
        values[1] = activeStream->submitted.load();
        values[2] = activeStream->completed.load();
        values[3] = activeStream->errors.load();
        values[4] = activeStream->lastErrno;
        values[5] = activeStream->urbCount;
        values[6] = activeStream->packetsPerUrb;
        values[7] = activeStream->framesRendered.load();
    }
    jlongArray result = env->NewLongArray(8);
    env->SetLongArrayRegion(result, 0, 8, values);
    return result;
}

// {frames the fake device received, longest packet in bytes, URBs with audio, errors, URBs completed, packets submitted}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_classickeys_classicplayer_UsbDirectAudio_nativeSelfTest(JNIEnv* env, jclass, jint milliseconds) {
    auto backendOwner = std::make_unique<FakeBackend>();
    auto* fake = backendOwner.get();
    Stream stream(std::move(backendOwner), 1, 196, 1000, 12);
    stream.start();
    usleep(static_cast<useconds_t>(std::max(100, static_cast<int>(milliseconds))) * 1000);
    stream.stop();
    jlong values[6] = {fake->framesSeen, fake->longestPacket, fake->nonSilentUrbs, stream.errors.load(), stream.completed.load(),
                       stream.submitted.load() * stream.packetsPerUrb};
    jlongArray result = env->NewLongArray(6);
    env->SetLongArrayRegion(result, 0, 6, values);
    return result;
}
