#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "NordSampleDecoder.h"

// Nord sample (.nsmp / .nsmp3 / .nsmp4) playback for Android. Decoding is the desktop decoder
// (nsmp/NordSampleDecoder, nsmp/NordLegacySampleDecoder); voice handling mirrors the desktop
// NordSampleEngine: zone by key and velocity, cubic interpolation with the baked loop splice,
// attack / decay-sustain / release, and a pedal-held voice of a re-struck key fading out quickly.
namespace nsmp_android {
constexpr int maxVoices = 256;
constexpr float restrikeFadeSeconds = 0.2f;

using Instrument = NordSampleDecoder::Instrument;

// Reads the file on the caller's thread (never the audio thread). Returns false and an error text.
bool load(const std::string& path, std::uint64_t decodedBudgetBytes,
          std::shared_ptr<Instrument>& destination, std::string& error);
std::string lastLoadInfo();
// Generated looping sine instrument, used only by the instrumented regression test.
std::shared_ptr<Instrument> makeTestToneInstrument();

struct Voice {
    bool active = false, released = false, keyDown = false, waitingForSustain = false;
    int note = -1, channel = 0, zone = -1;
    std::uint64_t serial = 0;
    double position = 0.0, increment = 1.0;
    float amplitude = 0.f, attack = 1.f, attackStep = 1.f, decayGain = 1.f;
    float releaseGain = 1.f, releaseCoefficient = 0.f;
};

struct Layer {
    std::shared_ptr<Instrument> instrument;
    std::array<Voice, maxVoices> voices {};
    std::uint64_t serial = 0;
};

void noteOn(Layer& layer, int note, int velocity, int channel, bool sustain, int voiceBudget,
            float attackSeconds, float releaseSeconds);
void noteOff(Layer& layer, int note, int channel, bool sustain, float releaseSeconds);
void pedalUp(Layer& layer, int channel, float releaseSeconds);
// Adds the layer's voices to `stereo` (interleaved L/R, `frames` frames).
void render(Layer& layer, float* stereo, int frames, int outputRate, int voiceBudget,
            float decayMs, float sustainPercent);
int activeVoices(const Layer& layer);
}
