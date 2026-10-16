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
    std::vector<int16_t> pcm;
};

struct Instrument {
    std::string name;
    int channels = 0, gainTenthsDb = 0, velocityScale = 31;
    bool signedTrim = false;
    std::array<uint8_t, 128> keyMap {};
    std::array<int8_t, 128> fineTune {};
    std::vector<Stroke> strokes;
};

// Decoding and loop preparation run only on the import worker, never on the
// audio callback. A failed import leaves the previously published layer intact.
bool load(const std::string& path, size_t decodedBudgetBytes,
          Instrument& destination, std::string& error);

struct Voice {
    bool active = false, released = false, waitingForSustain = false;
    int note = -1, channel = 0, stroke = -1;
    uint64_t serial = 0, age = 0;
    double position = 0, increment = 1;
    float level = 0, attackGain = 1, releaseGain = 1;
};

struct Layer {
    std::shared_ptr<Instrument> instrument;
    std::array<Voice, maxVoices> voices {};
    uint64_t serial = 0;
    bool finitePiano = true;
    int eqMode = 1;
    std::array<float,2> eqLowState {}, eqHighState {};
};

void noteOn(Layer& layer, int note, int velocity, int channel, bool sustain,
            int voiceBudget, float attackSeconds);
void noteOff(Layer& layer, int note, int channel, bool sustain);
void pedalUp(Layer& layer, int channel);
void render(Layer& layer, float* stereo, int frames, int outputRate,
            int voiceBudget, float attackSeconds, float releaseSeconds);
int activeVoices(const Layer& layer);
}
