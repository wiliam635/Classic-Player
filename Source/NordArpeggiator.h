#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Fixed-memory scheduler for the arpeggiator fields imported from NS4P.
// Audio processing uses no locks, allocations, or per-sample key-table scans.
struct NordArpSettings
{
    bool enabled = false;
    bool syncToClock = false;
    bool keyboardSync = false;
    bool zigZag = false;
    int mode = 0;       // 0 Arp, 1 Poly, 2 Gate
    int direction = 0;  // 0 Up, 1 Down, 2 Up/Down, 3 Random
    int rangeSemitones = 0; // 0..48
    float rate = 0.0f;  // normalized 0..1
    float bpm = 120.0f;
    float gate = 0.75f;
    bool patternEnabled = false;
    int patternLength = 16;
    std::array<std::uint8_t, 16> patternDuration {};
    std::array<std::uint8_t, 16> patternAccent {};
    std::array<std::int8_t, 16> patternPan {};
};

class NordArpeggiator
{
public:
    struct Event
    {
        bool noteOn = false;
        int channel = 1, note = 60, sourceNote = 60, velocity = 100, pan = 0;
        bool gateEvent = false, gateOpen = false;
    };

    void prepare(double rate) noexcept
    {
        sampleRate = std::max(1.0, rate);
        reset();
    }

    void reset() noexcept
    {
        held = {};
        sustain.fill(false);
        baseSequenceSize = sequenceSize = activeSize = heldKeyCount = 0;
        dirty = true;
        builtRange = builtMode = -1;
        countdown = gateCountdown = stepSerial = 0;
        stepIndex = 0;
        directionSign = 1;
        gateOpen = false;
        randomState = 0x6d2b79f5u;
    }

    template <typename Emit>
    void stop(Emit&& emit) noexcept
    {
        releaseActive(emit);
        // Always send a close target: the gate smoother can still be moving
        // even when the scheduler's logical state has already been reset.
        emit(Event { false, 1, 60, 60, 100, 0, true, false });
        reset();
    }

    template <typename Emit>
    void input(int channel, int note, int velocity, bool isOn, Emit&& emit,
               bool keyboardSync = false) noexcept
    {
        if (channel < 1 || channel > 16 || note < 0 || note > 127) return;
        auto& key = held[(size_t) (channel - 1) * 128u + (size_t) note];
        if (isOn)
        {
            const bool wasActive = key.count != 0 || key.sustained;
            if (!wasActive && keyboardSync)
            {
                countdown = gateCountdown = stepSerial = 0;
                stepIndex = 0;
                directionSign = 1;
                releaseActive(emit);
                if (gateOpen)
                {
                    gateOpen = false;
                    emit(Event { false, 1, 60, 60, 100, 0, true, false });
                }
            }
            key.sustained = false;
            if (!wasActive) ++heldKeyCount;
            if (key.count < 255) ++key.count;
            key.velocity = (std::uint8_t) std::clamp(velocity, 1, 127);
            dirty = true;
            if (baseSequenceSize == 0) countdown = 0;
            return;
        }

        if (key.count == 0) return;
        --key.count;
        if (key.count == 0)
        {
            key.sustained = sustain[(size_t) (channel - 1)];
            if (!key.sustained && heldKeyCount != 0) --heldKeyCount;
            if (!key.sustained) dirty = true;
        }
        if (heldKeyCount == 0)
        {
            releaseActive(emit);
            if (builtMode == 2 && gateOpen)
            {
                gateOpen = false;
                emit(Event { false, 1, 60, 60, 100, 0, true, false });
            }
        }
    }

    template <typename Emit>
    void controller(int channel, int cc, int value, Emit&& emit) noexcept
    {
        if (channel < 1 || channel > 16) return;
        if (cc == 120 || cc == 123)
        {
            const auto first = (size_t) (channel - 1) * 128u;
            for (size_t i = first; i < first + 128; ++i)
            {
                if (held[i].count != 0 || held[i].sustained)
                {
                    held[i] = {};
                    if (heldKeyCount != 0) --heldKeyCount;
                    dirty = true;
                }
            }
            sustain[(size_t) (channel - 1)] = false;
            releaseActiveChannel(channel, emit);
            if (heldKeyCount == 0 && builtMode == 2 && gateOpen)
            {
                gateOpen = false;
                emit(Event { false, 1, 60, 60, 100, 0, true, false });
            }
            return;
        }
        if (cc != 64) return;

        auto& down = sustain[(size_t) (channel - 1)];
        const bool next = value >= 64;
        if (down && !next)
            for (int note = 0; note < 128; ++note)
            {
                auto& key = held[(size_t) (channel - 1) * 128u + (size_t) note];
                if (key.sustained)
                {
                    key.sustained = false;
                    if (key.count == 0 && heldKeyCount != 0) --heldKeyCount;
                    dirty = true;
                }
            }
        down = next;
        if (heldKeyCount == 0) releaseActive(emit);
    }

    template <typename Emit>
    void tick(const NordArpSettings& settings, Emit&& emit) noexcept
    {
        advanceFrames(1, settings, [&](int, const Event& event) { emit(event); });
    }

    // Emits exact frame offsets, allowing sample engines to render between
    // note/gate events instead of rebuilding voices for every sample.
    template <typename Emit>
    void advanceFrames(int frames, const NordArpSettings& settings, Emit&& emit) noexcept
    {
        if (frames <= 0) return;
        if (!settings.enabled)
        {
            if (activeSize != 0 || heldKeyCount != 0 || gateOpen || builtMode == 2)
                stop([&](const Event& event) { emit(0, event); });
            return;
        }

        if (settings.mode != 2 && builtMode == 2)
        {
            gateOpen = false;
            emit(0, Event { false, 1, 60, 60, 100, 0, true, false });
        }

        if (dirty || builtRange != settings.rangeSemitones || builtMode != settings.mode)
            rebuild(settings);
        if (baseSequenceSize == 0)
        {
            releaseActive([&](const Event& event) { emit(0, event); });
            if (gateOpen)
            {
                gateOpen = false;
                emit(0, Event { false, 1, 60, 60, 100, 0, true, false });
            }
            countdown = gateCountdown = stepSerial = 0;
            builtMode = -1;
            return;
        }
        if (settings.mode == 2)
        {
            advanceGate(frames, settings, emit);
            return;
        }

        int offset = 0;
        while (offset < frames)
        {
            if (gateCountdown == 0 && activeSize != 0 && countdown != 0)
                releaseActiveAt(offset, emit);
            if (countdown == 0)
            {
                releaseActiveAt(offset, emit);
                const auto stepFrames = framesPerStep(settings);
                const auto step = settings.patternEnabled
                    ? (int) (stepSerial % (std::uint64_t) std::clamp(settings.patternLength, 1, 16)) : 0;
                const auto duration = settings.patternEnabled
                    ? durationForPattern(settings.patternDuration[(size_t) step])
                    : std::clamp((double) settings.gate, 0.0, 1.0);
                if (duration > 0.0)
                {
                    const bool accent = settings.patternEnabled
                        && settings.patternAccent[(size_t) step] != 0;
                    const int pan = settings.patternEnabled
                        ? settings.patternPan[(size_t) step] : 0;
                    if (settings.mode == 1)
                    {
                        const int inversionCount = std::max(1, sequenceSize - baseSequenceSize + 1);
                        playInversion(chooseIndex(settings, inversionCount), accent, pan, emit, offset);
                    }
                    else
                        play(sequence[(size_t) chooseIndex(settings, sequenceSize)], accent, pan, emit, offset);
                    gateCountdown = (std::uint64_t) std::max(1.0, std::round(stepFrames * duration));
                }
                else gateCountdown = 0;

                ++stepSerial;
                countdown = (std::uint64_t) std::max(1.0, std::ceil(stepFrames));
            }

            const auto remaining = (std::uint64_t) (frames - offset);
            auto elapsed = countdown;
            if (gateCountdown > 0) elapsed = std::min(elapsed, gateCountdown);
            if (elapsed >= remaining)
            {
                countdown = countdown > remaining ? countdown - remaining : 0;
                gateCountdown = gateCountdown > remaining ? gateCountdown - remaining : 0;
                break;
            }
            offset += (int) elapsed;
            countdown = countdown > elapsed ? countdown - elapsed : 0;
            gateCountdown = gateCountdown > elapsed ? gateCountdown - elapsed : 0;
            if (gateCountdown == 0 && activeSize != 0) releaseActiveAt(offset, emit);
        }
    }

    bool hasInputNotes() const noexcept { return heldKeyCount != 0; }
    int heldCount() const noexcept { return baseSequenceSize; }
    bool busy() const noexcept { return activeSize != 0 || hasInputNotes(); }

private:
    struct Key { std::uint8_t count = 0, velocity = 100; bool sustained = false; };
    struct StepNote { int channel = 1, note = 60, source = 60, velocity = 100, pan = 0; };
    static constexpr int maxHeldNotes = 2048;
    static constexpr int maxExpandedNotes = maxHeldNotes * 5;
    static constexpr int maxStepNotes = 256;

    std::array<Key, 2048> held {};
    std::array<bool, 16> sustain {};
    std::array<StepNote, maxHeldNotes> baseSequence {};
    std::array<StepNote, maxExpandedNotes> sequence {};
    std::array<StepNote, maxStepNotes> active {};
    int baseSequenceSize = 0, sequenceSize = 0, activeSize = 0;
    int heldKeyCount = 0, builtRange = -1, builtMode = -1;
    bool dirty = true, gateOpen = false;
    std::uint64_t countdown = 0, gateCountdown = 0, stepSerial = 0;
    int stepIndex = 0, directionSign = 1;
    std::uint32_t randomState = 0x6d2b79f5u;
    double sampleRate = 44100.0;

    double framesPerStep(const NordArpSettings& settings) const noexcept
    {
        const auto rate = std::clamp((double) settings.rate, 0.0, 1.0);
        double seconds = 1.5 / std::exp2(rate * 4.0);
        if (settings.syncToClock)
        {
            // Exact ArpDivisions table read from the WX Stage-IV binary.
            static constexpr double divisions[] { 2.0, 1.0, 2.0 / 3.0, 0.5,
                1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
            const auto index = std::clamp((int) std::floor(rate * 7.0 + 0.5), 0, 7);
            seconds = divisions[index] * 60.0 / std::clamp((double) settings.bpm, 30.0, 300.0);
        }
        return std::clamp(seconds * sampleRate, 1.0, sampleRate * 4.0);
    }

    void rebuild(const NordArpSettings& settings) noexcept
    {
        baseSequenceSize = sequenceSize = 0;
        for (int channel = 0; channel < 16; ++channel)
            for (int note = 0; note < 128; ++note)
            {
                const auto& key = held[(size_t) channel * 128u + (size_t) note];
                if (key.count == 0 && !key.sustained) continue;
                if (baseSequenceSize < maxHeldNotes)
                    baseSequence[(size_t) baseSequenceSize++] = { channel + 1, note, note, key.velocity };
            }
        std::sort(baseSequence.begin(), baseSequence.begin() + baseSequenceSize,
            [](const auto& a, const auto& b)
            { return a.note == b.note ? a.channel < b.channel : a.note < b.note; });

        if (baseSequenceSize > 0)
        {
            const int highestPitch = baseSequence[(size_t) baseSequenceSize - 1].note;
            const int rangeTop = highestPitch + std::clamp(settings.rangeSemitones, 0, 48);
            for (int octave = 0; octave <= 4; ++octave)
                for (int i = 0; i < baseSequenceSize; ++i)
                {
                    auto note = baseSequence[(size_t) i];
                    note.note += octave * 12;
                    if (note.note > rangeTop || note.note > 127) continue;
                    if (sequenceSize < maxExpandedNotes) sequence[(size_t) sequenceSize++] = note;
                }
        }
        std::sort(sequence.begin(), sequence.begin() + sequenceSize,
            [](const auto& a, const auto& b)
            { return a.note == b.note ? a.channel < b.channel : a.note < b.note; });
        if (sequenceSize == 0 || stepIndex >= std::max(1, settings.mode == 1
            ? sequenceSize - baseSequenceSize + 1 : sequenceSize))
        {
            stepIndex = 0;
            directionSign = 1;
        }
        builtRange = settings.rangeSemitones;
        builtMode = settings.mode;
        dirty = false;
    }

    int chooseIndex(const NordArpSettings& settings, int count) noexcept
    {
        count = std::max(1, count);
        if (count < 2) return 0;
        if (settings.direction == 3)
        {
            randomState = randomState * 1664525u + 1013904223u;
            return (int) (randomState % (std::uint32_t) count);
        }
        if (settings.direction == 1)
        {
            const int result = count - 1 - (stepIndex % count);
            ++stepIndex;
            return result;
        }
        if (settings.direction == 2)
        {
            const int result = stepIndex;
            stepIndex += directionSign;
            if (stepIndex >= count) { directionSign = -1; stepIndex = count - 2; }
            else if (stepIndex < 0) { directionSign = 1; stepIndex = 1; }
            return result;
        }
        const int result = stepIndex % count;
        if (settings.zigZag && count >= 3)
        {
            const int current = std::clamp(stepIndex, 0, count - 1);
            if (directionSign > 0 && current + 2 < count) stepIndex += 2;
            else if (directionSign > 0) { directionSign = -1; stepIndex = count - 2; }
            else if (current - 2 >= 0) stepIndex -= 2;
            else { directionSign = 1; stepIndex = 1; }
            return current;
        }
        ++stepIndex;
        return result;
    }

    static double durationForPattern(std::uint8_t value) noexcept
    {
        static constexpr double gates[] { 0.0, 0.25, 0.5, 0.75 };
        return gates[std::min<std::size_t>(value, 3)];
    }

    template <typename Emit>
    void advanceGate(int frames, const NordArpSettings& settings, Emit&& emit) noexcept
    {
        int offset = 0;
        while (offset < frames)
        {
            if (gateOpen && gateCountdown == 0)
            {
                gateOpen = false;
                emit(offset, Event { false, 1, 60, 60, 100, 0, true, false });
            }
            if (countdown == 0)
            {
                if (gateOpen)
                {
                    gateOpen = false;
                    emit(offset, Event { false, 1, 60, 60, 100, 0, true, false });
                }
                const auto stepFrames = framesPerStep(settings);
                const auto step = settings.patternEnabled
                    ? (int) (stepSerial % (std::uint64_t) std::clamp(settings.patternLength, 1, 16)) : 0;
                const auto duration = settings.patternEnabled
                    ? durationForPattern(settings.patternDuration[(size_t) step])
                    : std::clamp((double) settings.gate, 0.0, 1.0);
                if (duration > 0.0)
                {
                    gateOpen = true;
                    emit(offset, Event { false, 1, 60, 60, 100, 0, true, true });
                    gateCountdown = (std::uint64_t) std::max(1.0, std::ceil(stepFrames * duration));
                }
                else gateCountdown = 0;
                ++stepSerial;
                countdown = (std::uint64_t) std::max(1.0, std::ceil(stepFrames));
            }

            const auto remaining = (std::uint64_t) (frames - offset);
            auto elapsed = countdown;
            if (gateCountdown > 0) elapsed = std::min(elapsed, gateCountdown);
            if (elapsed >= remaining)
            {
                countdown = countdown > remaining ? countdown - remaining : 0;
                gateCountdown = gateCountdown > remaining ? gateCountdown - remaining : 0;
                break;
            }
            offset += (int) elapsed;
            countdown = countdown > elapsed ? countdown - elapsed : 0;
            gateCountdown = gateCountdown > elapsed ? gateCountdown - elapsed : 0;
            if (gateCountdown == 0 && gateOpen)
            {
                gateOpen = false;
                emit(offset, Event { false, 1, 60, 60, 100, 0, true, false });
            }
        }
    }

    template <typename Emit>
    void playInversion(int inversion, bool accent, int pan, Emit&& emit, int offset) noexcept
    {
        const auto maxOctaves = std::max(0, (builtRange + 11) / 12);
        for (int i = 0; i < baseSequenceSize && activeSize < maxStepNotes; ++i)
        {
            const auto shiftSteps = inversion + baseSequenceSize - 1 - i;
            const auto octave = std::clamp(shiftSteps / std::max(1, baseSequenceSize), 0, maxOctaves);
            auto note = baseSequence[(size_t) i];
            const auto rangeTop = baseSequence[(size_t) baseSequenceSize - 1].note + builtRange;
            note.note = std::min({ 127, rangeTop, note.note + octave * 12 });
            play(note, accent, pan, emit, offset);
        }
    }

    template <typename Emit>
    void play(const StepNote& note, bool accent, int pan, Emit&& emit, int offset) noexcept
    {
        if (activeSize >= maxStepNotes) return;
        const auto velocity = accent
            ? std::min(127, std::min(note.velocity, 96) + 30) : note.velocity;
        auto activeNote = note;
        activeNote.pan = std::clamp(pan, -1, 1);
        emit(offset, Event { true, note.channel, note.note, note.source, velocity, activeNote.pan });
        active[(size_t) activeSize++] = activeNote;
    }

    template <typename Emit>
    void releaseActive(Emit&& emit) noexcept
    {
        for (int i = 0; i < activeSize; ++i)
        {
            const auto& note = active[(size_t) i];
            emit(Event { false, note.channel, note.note, note.source, note.velocity, note.pan });
        }
        activeSize = 0;
    }

    template <typename Emit>
    void releaseActiveAt(int offset, Emit&& emit) noexcept
    {
        for (int i = 0; i < activeSize; ++i)
        {
            const auto& note = active[(size_t) i];
            emit(offset, Event { false, note.channel, note.note, note.source, note.velocity, note.pan });
        }
        activeSize = 0;
    }

    template <typename Emit>
    void releaseActiveChannel(int channel, Emit&& emit) noexcept
    {
        int kept = 0;
        for (int i = 0; i < activeSize; ++i)
        {
            const auto& note = active[(size_t) i];
            if (note.channel == channel)
                emit(Event { false, note.channel, note.note, note.source, note.velocity, note.pan });
            else active[(size_t) kept++] = note;
        }
        activeSize = kept;
    }
};
