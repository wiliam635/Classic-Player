#pragma once
#include "NordSampleDecoder.h"
#include "NordProgramDecoder.h"
#include "Sf2Engine.h"

// Bank-local rompler. All files/PCM are prepared outside the audio callback.
class NordSampleEngine {
public:
    void prepare(double rate) { sampleRate=rate;for(auto& s:slots){s.reverb.setSampleRate(rate);s.gain.reset(rate,.02);s.gain.setCurrentAndTargetValue(.8f);}stop(); }
    void set(int part, std::shared_ptr<const NordSampleDecoder::Instrument> instrument) { slots[size_t(part)].instrument=std::move(instrument); }
    void stop();
    void process(int part, juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                 const NordProgramDecoder::SynthPart&, const Sf2Engine::LayerConfig&);
private:
    struct Voice {
        bool active=false, down=false, release=false;
        int note=0, channel=1, zone=0;
        double position=0, step=1;
        float velocity=1, envelope=0, releaseStep=0;
        std::uint64_t age=0;
        std::array<std::array<float,4>,2> filter {};
    };
    struct Slot {
        std::shared_ptr<const NordSampleDecoder::Instrument> instrument;
        std::array<Voice,128> voices;
        std::array<bool,16> sustain {};
        LayerEqState eq;
        juce::Reverb reverb;
        juce::SmoothedValue<float> gain;
        std::array<float,2> compressor {};
    };
    std::array<Slot,3> slots;
    double sampleRate=44100;
};
