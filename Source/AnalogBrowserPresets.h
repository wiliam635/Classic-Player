#pragma once
#include "AnalogSynthEngine.h"

// Exact parameter bank from the user-approved analog-browser-lab/app.js.
// Browser fallback uses a 12 dB/oct Web Audio lowpass, not the optional ladder.
namespace AnalogBrowserPresets
{
struct Preset {
    const char* category;
    const char* name;
    std::array<AnalogSynthEngine::Waveform, 3> waves;
    std::array<float, 3> levels, tunes;
    float cutoffHz, resonanceDb, attack, decay, sustain, release, lfo, modulation;
    bool mono;
};
using W = AnalogSynthEngine::Waveform;
inline constexpr std::array<Preset, 32> bank {{
    { "Lead", "Solo Lead", {W::saw,W::triangle,W::saw}, {0.66f,0.36f,0.0f}, {0.0f,0.0f,0.0f}, 2350.0f,0.2f,0.034f,3.9f,0.76f,0.048f,4.0f,0.25f, true },
    { "Lead", "Modern Lead", {W::square,W::saw,W::saw}, {0.57f,0.38f,0.28f}, {0.0f,12.0f,19.0f}, 6100.0f,2.4f,0.006f,0.18f,0.62f,0.09f,5.8f,0.38f, true },
    { "Lead", "Classic Minimoog Lead", {W::saw,W::saw,W::square}, {0.76f,0.41f,0.13f}, {0.0f,0.0f,-12.0f}, 760.0f,12.0f,0.013f,0.28f,0.58f,0.1f,4.5f,0.16f, true },
    { "Lead", "Lucky Man", {W::triangle,W::saw,W::sine}, {0.55f,0.52f,0.12f}, {-12.0f,12.0f,0.0f}, 1450.0f,4.5f,0.022f,0.55f,0.68f,0.12f,3.2f,0.12f, true },
    { "Lead", "Unison Lead", {W::saw,W::saw,W::square}, {0.62f,0.48f,0.18f}, {0.0f,12.0f,12.0f}, 4100.0f,3.2f,0.009f,0.24f,0.64f,0.1f,5.1f,0.28f, true },
    { "Lead", "Vintage Lead", {W::triangle,W::saw,W::triangle}, {0.72f,0.27f,0.12f}, {0.0f,0.0f,-12.0f}, 1120.0f,5.2f,0.025f,0.48f,0.66f,0.14f,3.6f,0.11f, true },
    { "Lead", "Easy Lead", {W::square,W::triangle,W::saw}, {0.62f,0.26f,0.19f}, {0.0f,7.0f,12.0f}, 2500.0f,4.0f,0.015f,0.34f,0.7f,0.13f,2.2f,0.34f, true },
    { "Lead", "Screaming Lead", {W::saw,W::saw,W::square}, {0.7f,0.42f,0.15f}, {0.0f,12.0f,24.0f}, 3800.0f,11.0f,0.004f,0.14f,0.57f,0.07f,6.5f,0.42f, true },
    { "Lead", "Waterfall Lead", {W::saw,W::sine,W::triangle}, {0.49f,0.31f,0.28f}, {0.0f,12.0f,19.0f}, 2050.0f,2.6f,0.03f,0.7f,0.72f,0.2f,1.1f,0.46f, true },
    { "Lead", "Brass Lead", {W::saw,W::square,W::triangle}, {0.63f,0.31f,0.14f}, {0.0f,0.0f,12.0f}, 1700.0f,7.4f,0.012f,0.31f,0.52f,0.09f,4.0f,0.14f, true },
    { "Bass", "Taurus Bass", {W::saw,W::square,W::triangle}, {0.8f,0.35f,0.15f}, {-12.0f,-12.0f,-24.0f}, 380.0f,13.0f,0.008f,0.25f,0.7f,0.18f,2.0f,0.05f, true },
    { "Bass", "Funk Bass", {W::saw,W::square,W::triangle}, {0.62f,0.45f,0.1f}, {-12.0f,0.0f,-12.0f}, 720.0f,6.0f,0.004f,0.13f,0.45f,0.1f,3.0f,0.08f, false },
    { "Bass", "Authentic Minimoog Bass", {W::saw,W::saw,W::triangle}, {0.75f,0.32f,0.09f}, {-12.0f,-12.0f,-24.0f}, 460.0f,11.0f,0.008f,0.22f,0.55f,0.16f,2.5f,0.08f, true },
    { "Bass", "Thick Bass", {W::saw,W::saw,W::square}, {0.7f,0.4f,0.13f}, {-12.0f,-24.0f,-12.0f}, 540.0f,8.0f,0.006f,0.2f,0.69f,0.13f,2.2f,0.06f, true },
    { "Bass", "Analog Bass", {W::square,W::triangle,W::saw}, {0.48f,0.46f,0.18f}, {-12.0f,-24.0f,0.0f}, 650.0f,5.5f,0.01f,0.29f,0.61f,0.17f,1.8f,0.05f, true },
    { "Bass", "Percussive Bass", {W::square,W::saw,W::triangle}, {0.62f,0.37f,0.11f}, {-12.0f,-12.0f,-24.0f}, 980.0f,8.6f,0.004f,0.09f,0.34f,0.055f,3.0f,0.04f, true },
    { "Bass", "Dub Bass", {W::sine,W::square,W::triangle}, {0.52f,0.32f,0.28f}, {-24.0f,-12.0f,-12.0f}, 390.0f,4.2f,0.018f,0.45f,0.74f,0.27f,0.75f,0.09f, true },
    { "Bass", "Pulse Bass", {W::square,W::square,W::triangle}, {0.54f,0.31f,0.17f}, {-12.0f,0.0f,-24.0f}, 830.0f,6.8f,0.008f,0.18f,0.54f,0.12f,4.8f,0.22f, false },
    { "Pad", "Crystal Pad", {W::triangle,W::sine,W::saw}, {0.5f,0.33f,0.11f}, {0.0f,12.0f,19.0f}, 2800.0f,2.0f,0.38f,0.6f,0.72f,1.5f,0.33f,0.38f, false },
    { "Pad", "Dream Pad", {W::triangle,W::triangle,W::sine}, {0.62f,0.36f,0.12f}, {0.0f,7.0f,12.0f}, 1250.0f,2.0f,0.5f,0.7f,0.78f,1.9f,0.23f,0.46f, false },
    { "Pad", "Space Mod", {W::saw,W::triangle,W::sine}, {0.34f,0.52f,0.16f}, {0.0f,12.0f,19.0f}, 1100.0f,3.0f,0.65f,0.9f,0.7f,2.3f,0.15f,0.6f, false },
    { "Pad", "Atmospheric Pad", {W::sine,W::triangle,W::saw}, {0.36f,0.44f,0.15f}, {0.0f,12.0f,24.0f}, 820.0f,2.1f,0.9f,1.1f,0.76f,3.1f,0.12f,0.52f, false },
    { "Pad", "Warm Pad", {W::triangle,W::triangle,W::sine}, {0.55f,0.34f,0.16f}, {0.0f,7.0f,12.0f}, 920.0f,1.5f,0.56f,0.72f,0.8f,2.4f,0.28f,0.31f, false },
    { "Pad", "Vintage Minimoog Pad", {W::saw,W::triangle,W::square}, {0.31f,0.44f,0.09f}, {0.0f,12.0f,19.0f}, 1030.0f,3.8f,0.48f,0.8f,0.73f,2.1f,0.22f,0.48f, false },
    { "Pad", "Shimmer Pad", {W::sine,W::triangle,W::sine}, {0.41f,0.34f,0.24f}, {0.0f,12.0f,19.0f}, 3600.0f,1.2f,0.42f,0.62f,0.7f,2.6f,0.38f,0.28f, false },
    { "Pad", "Velvet Cloud", {W::triangle,W::sine,W::triangle}, {0.59f,0.3f,0.13f}, {0.0f,7.0f,14.0f}, 1270.0f,1.8f,0.72f,1.2f,0.78f,3.4f,0.18f,0.4f, false },
    { "Experimental", "Alien Landscape", {W::sine,W::triangle,W::square}, {0.4f,0.27f,0.09f}, {0.0f,19.0f,24.0f}, 900.0f,5.0f,0.7f,1.1f,0.65f,2.6f,0.12f,0.72f, false },
    { "Experimental", "Digital Rain", {W::square,W::sine,W::triangle}, {0.33f,0.4f,0.19f}, {0.0f,12.0f,24.0f}, 2300.0f,4.0f,0.01f,0.12f,0.25f,0.45f,7.0f,0.3f, false },
    { "Experimental", "Submarine Sonar", {W::sine,W::sine,W::triangle}, {0.8f,0.25f,0.08f}, {0.0f,12.0f,-12.0f}, 650.0f,12.0f,0.01f,0.4f,0.1f,2.8f,0.18f,0.18f, true },
    { "Experimental", "Thunder Storm", {W::square,W::saw,W::sine}, {0.18f,0.26f,0.33f}, {-12.0f,0.0f,19.0f}, 720.0f,8.0f,0.3f,0.9f,0.55f,3.2f,0.14f,0.78f, false },
    { "Experimental", "Glass Harmonica", {W::sine,W::triangle,W::sine}, {0.52f,0.24f,0.18f}, {0.0f,12.0f,19.0f}, 3150.0f,2.8f,0.16f,0.75f,0.62f,2.2f,0.42f,0.34f, false },
    { "Experimental", "Cosmic Drone", {W::saw,W::sine,W::triangle}, {0.27f,0.43f,0.25f}, {-12.0f,0.0f,7.0f}, 1250.0f,4.5f,0.18f,1.2f,0.83f,4.0f,0.09f,0.7f, false },
}};
inline AnalogSynthEngine::Config config(size_t index, const Sf2Engine::LayerConfig& routing)
{
    const auto& p = bank.at(index);
    AnalogSynthEngine::Config c;
    c.routing = routing;
    c.browserCompatible = true;
    c.oscillator1Wave = p.waves[0]; c.oscillator2Wave = p.waves[1]; c.oscillator3Wave = p.waves[2];
    c.oscillator1Level = p.levels[0]; c.oscillator2Level = p.levels[1]; c.oscillator3Level = p.levels[2];
    c.oscillator1Semitones = p.tunes[0]; c.oscillator2Semitones = p.tunes[1]; c.oscillator3Semitones = p.tunes[2];
    c.cutoff = 100.0f * std::log(p.cutoffHz / 25.0f) / std::log(700.0f);
    c.routing.cutoff = c.cutoff;
    c.resonance = p.resonanceDb / 20.0f;
    c.filterEnvelopeAmount = 0;
    c.ampAttackMs = p.attack * 1000; c.ampDecayMs = p.decay * 1000;
    c.ampSustain = p.sustain; c.ampReleaseMs = p.release * 1000;
    c.lfoRateHz = p.lfo;
    // In browser mode this knob is a percent of the original 420 Hz depth.
    c.lfoToFilter = p.modulation * 100;
    c.lfoToPitch = 0;
    c.noiseLevel = 0; // Browser UI had a noise parameter but no noise generator.
    c.monophonic = p.mono; c.routing.mono = p.mono; c.routing.portamento = false;
    return c;
}
}

