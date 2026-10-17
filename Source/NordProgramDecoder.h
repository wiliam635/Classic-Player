#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Static NS4 program data only. Playback is a separate, explicitly adapted engine.
class NordProgramDecoder
{
public:
    struct Part
    {
        std::array<bool, 2> enabled {};
        float gain = 0, reverb = 0, eqLow = 0, eqMid = 0, eqHigh = 0;
        int octave = 0, zones = 15, timbre = -1, touch = 0, model = 0;
        bool sustain = true;
        std::uint32_t pianoId = 0;
        std::string pianoName;
        std::array<int, 9> drawbars {};
        int percussion = 0;
    };
    struct SynthPart : Part {
        bool sample = false, mono = false, arp = false;
        int synthType = 0, category = 0, wave = 0;
        float control = 0, coarse = 0, fine = 0, pan = 0;
        float attackMs = 5, decayMs = -1, releaseMs = 100, filterHz = 20000;
        float resonance = 0, filterEnv = 0, filterAttackMs = 5, filterDecayMs = 1000;
        float lfoHz = 5, lfoAmount = 0, glideMs = 0;
        std::uint32_t sampleId = 0;
        std::string sampleName;
    };
    struct Program
    {
        std::array<Part, 2> pianos, organs;
        std::array<SynthPart, 3> synths;
        std::array<std::array<bool, 3>, 2> sections {};
        std::array<int, 3> splitNotes { 36, 60, 77 };
        std::array<bool, 3> splitEnabled {};
        bool split = false;
        int scene = 0, transpose = 0, leslie = 0, version = 0;
        float organDrive = 0;
        std::vector<std::string> warnings;
        bool playable(int selectedScene, std::string& reason) const;
        bool accepts(const Part&, int note) const;
    };
    static bool decode(const std::vector<std::uint8_t>&, Program&, std::string& error);
};
