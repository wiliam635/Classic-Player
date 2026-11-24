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
constexpr float restrikeFadeSeconds = 0.2f;   // pedal-held voice of a re-struck key fades over this time

struct Stroke {
    uint8_t root = 60, bank = 0, velocityLayer = 0;
    uint16_t trim = 0;
    uint32_t loopStart = 0, loopEnd = 0;
    float peakDb = -120.f;   // peak of the first 0.3 s in dBFS, measured after loading
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
    bool active = false, keyDown = false, released = false, waitingForSustain = false, sostenutoHeld = false;
    int note = -1, channel = 0, stroke = -1;
    uint64_t serial = 0, age = 0;
    double position = 0, increment = 1;
    float toneHz = 0, toneLeft = 0, toneRight = 0, airLeft = 0, airRight = 0;   // reference voicing: velocity-dependent low-pass
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
    bool longRelease = false;   // doubles the release
    bool stringRes = false;     // sympathetic string resonance (follows the sustain pedal)
    std::array<bool,16> pedal {}, sostenutoDown {}, softDown {};
    std::array<float,16> bend {};   // -1..1, +-2 semitones
    std::array<float,16> ratioNow = [] { std::array<float,16> a; a.fill(1.f); return a; }();
    std::array<float,16> cc7 = [] { std::array<float,16> a; a.fill(1.f); return a; }();
    std::array<float,16> cc11 = [] { std::array<float,16> a; a.fill(1.f); return a; }();
    std::array<float,16> gainNow = [] { std::array<float,16> a; a.fill(1.f); return a; }();
    static constexpr int stringResCount = 56;   // A0..E5 string fundamentals
    struct StringRes {
        std::array<std::array<float,4>,stringResCount> y1 {}, y2 {}, level {};
        bool active = false;
    } res;
    std::vector<float> resIn, resOut;           // preallocated off the audio thread
};

/** Peak level of every stroke (first 0.3 s); needed by the reference velocity law. */
void measureLevels(Instrument& piano);
/** CC 7, 11 (squared law), 66 sostenuto and 67 soft pedal. sustainDown is the current CC64 state. */
void control(Layer& layer, int channel, int controller, int value);
void pitchBend(Layer& layer, int channel, int value14);
void resetControllers(Layer& layer);
void prepareResonance(Layer& layer);

void noteOn(Layer& layer, int note, int velocity, int channel, bool sustain,
            int voiceBudget, float attackSeconds, int timbreFloor = 1);
void noteOff(Layer& layer, int note, int channel, bool sustain,
             float releaseSeconds = 0.04f);
void pedalUp(Layer& layer, int channel, float releaseSeconds = 0.04f);
void render(Layer& layer, float* stereo, int frames, int outputRate,
            int voiceBudget, float attackSeconds, float decayMs = 0.0f, float sustainPercent = 100.0f);
int activeVoices(const Layer& layer);
}
