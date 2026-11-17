#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nord_android {
constexpr int sampleRate = 35002;
constexpr int maxVoices = 256;

struct Stroke {
    uint8_t root = 60, bank = 0, velocityLayer = 0;
    uint16_t trim = 0;
    uint32_t loopStart = 0, loopEnd = 0;
    // PCM is either owned (freshly decoded) or points into a read-only mapping
    // of the fast-load cache; the audio thread only ever reads `data`.
    std::vector<int16_t> owned;
    const int16_t* data = nullptr;
    size_t samples = 0;
};

struct Instrument {
    std::string name;
    int channels = 0, gainTenthsDb = 0, velocityScale = 31;
    bool signedTrim = false;
    std::array<uint8_t, 128> keyMap {};
    std::array<int8_t, 128> fineTune {};
    std::vector<Stroke> strokes;
    std::shared_ptr<void> backing;   // keeps the cache mapping alive
    bool fromCache = false;
    double loadMs = 0;
};

// Human readable summary of the last load ("cache 120 ms" / "decodificado 8200 ms").
std::string lastLoadInfo();

// Decoding and loop preparation run only on the import worker, never on the
// audio callback. A failed import leaves the previously published layer intact.
bool load(const std::string& path, size_t decodedBudgetBytes,
          Instrument& destination, std::string& error);

struct Voice {
    bool active = false, keyDown = false, released = false, waitingForSustain = false;
    int note = -1, channel = 0, stroke = -1;
    uint64_t serial = 0, age = 0;
    double position = 0, increment = 1;
    float level = 0, attackGain = 1, releaseGain = 1, releaseSeconds = 0.04f, decayGain = 1, decaySeconds = 15.f;
};

struct Layer {
    std::shared_ptr<Instrument> instrument;
    std::array<Voice, maxVoices> voices {};
    uint64_t serial = 0;
    bool finitePiano = true;
    int eqMode = -1;           // -1 = OFF (bypass, default), 0 SOFT, 1 MID, 2 BRIGHT
    int dynamicCompression = 0; // 0..3, lifts quiet keystrokes like the desktop
    bool softRelease = false;
    bool reflections = false;   // pedal-down reflection bank, opt-in like the desktop
    float decaySeconds = 15.f;  // CAUDA NPNO 3..15 s
    std::array<float,2> eqLowState {}, eqHighState {};
};

void noteOn(Layer& layer, int note, int velocity, int channel, bool sustain,
            int voiceBudget, float attackSeconds, int timbreFloor = 1);
void noteOff(Layer& layer, int note, int channel, bool sustain,
             float releaseSeconds = 0.04f);
void pedalUp(Layer& layer, int channel, float releaseSeconds = 0.04f);
void render(Layer& layer, float* stereo, int frames, int outputRate,
            int voiceBudget, float attackSeconds, float decayMs = 0.0f, float sustainPercent = 100.0f);
int activeVoices(const Layer& layer);
}
