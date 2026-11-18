#pragma once

#include "NordPianoDecoder.h"
#include "Sf2Engine.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

// Keeps the pages of memory-mapped NPNO samples resident ahead of every sounding voice.
//
// A mapped sample is read straight from the cache file. When the machine is short of memory the operating
// system drops those pages, and the next read becomes a disk read inside the audio callback (an audible dropout).
// The audio thread therefore only publishes where each voice is; this background thread touches the pages the
// voice will need over the next ~1.2 s, so the audio thread finds them already in memory.
class PianoPrefetcher final : private juce::Thread
{
public:
    static constexpr int maxLayers = Sf2Engine::layerCount;
    static constexpr int maxVoices = Sf2Engine::maxPolyphonyPerLayer;

    struct Voice
    {
        int stroke = -1;
        double firstFrame = 0.0;  // current read position
        double framesAhead = 0.0; // how far to keep resident
        double loopStart = 0.0, loopEnd = 0.0;
        bool looped = false;
    };

    PianoPrefetcher() : juce::Thread("NPNO prefetch") {}
    ~PianoPrefetcher() override { stopThread(2000); }

    void start() { if (!isThreadRunning()) startThread(); }

    // Message thread: which instrument a layer plays (keeps its mapping alive while pages are touched).
    void setInstrument(int layer, std::shared_ptr<NordPianoDecoder::Instrument> instrument)
    {
        if (!juce::isPositiveAndBelow(layer, maxLayers)) return;
        std::shared_ptr<NordPianoDecoder::Instrument> previous;
        {
            const juce::SpinLock::ScopedLockType guard(spin);
            previous = std::move(slots[(size_t) layer].instrument);
            slots[(size_t) layer].instrument = std::move(instrument);
            slots[(size_t) layer].count = 0;
        }
    }

    // Audio thread: never blocks; returns false if the prefetch thread held the lock this time.
    bool publish(int layer, const Voice* voices, int count) noexcept
    {
        if (!juce::isPositiveAndBelow(layer, maxLayers)) return false;
        const juce::SpinLock::ScopedTryLockType guard(spin);
        if (!guard.isLocked()) return false;
        auto& slot = slots[(size_t) layer];
        slot.count = std::min(count, maxVoices);
        for (int i = 0; i < slot.count; ++i) slot.voices[(size_t) i] = voices[i];
        return true;
    }

private:
    struct Slot
    {
        std::shared_ptr<NordPianoDecoder::Instrument> instrument;
        std::array<Voice, (size_t) maxVoices> voices {};
        int count = 0;
    };

    static void touchRange(const char* base, size_t totalBytes, size_t frameBytes, double fromFrame, double toFrame)
    {
        if (!(toFrame > fromFrame)) return;
        const auto first = static_cast<size_t>(std::max(0.0, fromFrame)) * frameBytes;
        const auto last = std::min(totalBytes, static_cast<size_t>(std::max(0.0, toFrame)) * frameBytes);
        if (last <= first) return;
        volatile char sink = 0;
        for (size_t offset = first; offset < last; offset += 4096) sink = static_cast<char>(sink ^ base[offset]);
        sink = static_cast<char>(sink ^ base[last - 1]);
    }

    static void touch(const NordPianoDecoder::Instrument& instrument, const Voice& voice)
    {
        if (voice.stroke < 0 || voice.stroke >= static_cast<int>(instrument.strokes.size()) || instrument.channels < 1)
            return;
        const auto& pcm = instrument.strokes[(size_t) voice.stroke].interleavedPcm;
        if (!pcm.isMapped()) return;
        const auto* base = reinterpret_cast<const char*>(pcm.data());
        const auto totalBytes = pcm.size() * sizeof(std::int16_t);
        const auto frameBytes = static_cast<size_t>(instrument.channels) * sizeof(std::int16_t);
        const auto end = voice.firstFrame + voice.framesAhead;
        if (voice.looped && end > voice.loopEnd)
        {
            touchRange(base, totalBytes, frameBytes, voice.firstFrame, voice.loopEnd);
            touchRange(base, totalBytes, frameBytes, voice.loopStart,
                       std::min(voice.loopEnd, voice.loopStart + (end - voice.loopEnd)));
        }
        else
            touchRange(base, totalBytes, frameBytes, voice.firstFrame, end);
    }

    void run() override
    {
        std::vector<Voice> local;
        local.reserve((size_t) maxVoices);
        while (!threadShouldExit())
        {
            for (int layer = 0; layer < maxLayers && !threadShouldExit(); ++layer)
            {
                std::shared_ptr<NordPianoDecoder::Instrument> instrument;
                local.clear();
                {
                    const juce::SpinLock::ScopedLockType guard(spin);
                    const auto& slot = slots[(size_t) layer];
                    if (slot.instrument != nullptr && slot.count > 0)
                    {
                        instrument = slot.instrument;
                        local.assign(slot.voices.begin(), slot.voices.begin() + slot.count);
                    }
                }
                if (instrument != nullptr)
                    for (const auto& voice : local) touch(*instrument, voice);
            }
            wait(10);
        }
    }

    juce::SpinLock spin;
    std::array<Slot, (size_t) maxLayers> slots;
};
