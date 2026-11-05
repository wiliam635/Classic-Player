#include "AnalogSynthEngine.h"
#include "LayerEffects.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float twoPi = juce::MathConstants<float>::twoPi;

float wrapPhase(float value) noexcept
{
    return value - std::floor(value);
}

// The browser implementation uses exponential WebAudio ramps for every ADSR
// transition.  A linear decrement in the native engine made short Minimoog
// notes end with a discontinuity (the audible click users reported).  This
// helper follows the same five-time-constant convention while remaining
// sample based and real-time safe.
float envelopeCoefficient(float milliseconds, double sampleRate) noexcept
{
    const auto duration = juce::jmax(3.0f, milliseconds) * 0.001f;
    const auto tau = duration / 5.0f;
    return static_cast<float>(std::exp(-1.0f / juce::jmax(1.0, tau * sampleRate)));
}

float nextNoise(uint32_t& state) noexcept
{
    // Deterministic, denormal-safe white noise. Unlike the old floating point
    // generator this remains stable during long live sessions.
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0x00ffffffu) / 8388607.5f - 1.0f;
}
}

AnalogSynthEngine::AnalogSynthEngine()
{
    for (auto& peak : peaks)
        peak.store(0.0f, std::memory_order_relaxed);
}

void AnalogSynthEngine::prepare(double newSampleRate, int maximumBlockSize)
{
    sampleRate = juce::jmax(1.0, newSampleRate);
    renderScratch.setSize(2, juce::jmax(1, maximumBlockSize), false, true, true);
    reset();
    for (auto& layer : layers)
    {
        layer.arpeggiator.prepare(sampleRate);
        layer.nordEffects.prepare(sampleRate);
        layer.reverb.setSampleRate(sampleRate);
        layer.reverb.reset();
        layer.nordEffects.reset();
        layer.gain.reset(sampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(0.0f);
        layer.arpeggiatorGate.reset(sampleRate, 0.003);
        layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
    }
}

void AnalogSynthEngine::reset()
{
    for (auto& layer : layers)
    {
        for (auto& voice : layer.voices) voice = {};
        layer.heldNotes.fill(false);
        layer.arpeggiator.reset();
        layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
        layer.heldVelocities.fill(0.0f);
        layer.noteOrder.fill(0);
        layer.noteSequence = 0;
        layer.sustainPedal.fill(false);
        layer.modWheel = 0.0f;
        layer.pitchBendSemitones = 0.0f;
        layer.reverb.reset();
        layer.nordEffects.reset();
        layer.compressorEnvelope = {};
        layer.filterState = {};
        layer.eq.reset();
        layer.lfoPhase = 0.0f;
        layer.gainReady = false;
        layer.effectTailPending = false;
        layer.effectQuietSamples = 0;
    }

    for (auto& peak : peaks)
        peak.store(0.0f, std::memory_order_relaxed);
}

void AnalogSynthEngine::stopAllSounds()
{
    reset();
}

void AnalogSynthEngine::unload(int layerIndex)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount))
        return;

    auto& layer = layers[static_cast<size_t>(layerIndex)];
    for (auto& voice : layer.voices) voice = {};
    layer.heldNotes.fill(false);
    layer.arpeggiator.reset();
    layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
    layer.heldVelocities.fill(0.0f);
    layer.noteOrder.fill(0);
    layer.noteSequence = 0;
    layer.sustainPedal.fill(false);
    layer.modWheel = 0.0f;
    layer.pitchBendSemitones = 0.0f;
    layer.reverb.reset();
    layer.nordEffects.reset();
    layer.compressorEnvelope = {};
    layer.filterState = {};
    layer.eq.reset();
    layer.lfoPhase = 0.0f;
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    peaks[static_cast<size_t>(layerIndex)].store(0.0f, std::memory_order_relaxed);
}

float AnalogSynthEngine::getLayerPeak(int layer) const noexcept
{
    return juce::isPositiveAndBelow(layer, layerCount)
        ? peaks[static_cast<size_t>(layer)].load(std::memory_order_relaxed) : 0.0f;
}

float AnalogSynthEngine::midiNoteToFrequency(int note) noexcept
{
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

float AnalogSynthEngine::polyBlep(float phase, float increment)
{
    if (increment <= 0.0f)
        return 0.0f;

    if (phase < increment)
    {
        const auto t = phase / increment;
        return t + t - t * t - 1.0f;
    }

    if (phase > 1.0f - increment)
    {
        const auto t = (phase - 1.0f) / increment;
        return t * t + t + t + 1.0f;
    }

    return 0.0f;
}

float AnalogSynthEngine::waveform(Waveform form, float phase, float increment)
{
    phase = wrapPhase(phase);

    switch (form)
    {
        case Waveform::triangle:
            // A sinusoidal triangle avoids the hard corners and aliasing of
            // the previous piecewise waveform at high notes.
            return std::asin(std::sin(twoPi * phase)) * (2.0f / juce::MathConstants<float>::pi);

        case Waveform::sine:
            return std::sin(twoPi * phase);

        case Waveform::saw:
            return 2.0f * phase - 1.0f - polyBlep(phase, increment);

        case Waveform::square:
        {
            auto output = phase < 0.5f ? 1.0f : -1.0f;
            output += polyBlep(phase, increment);
            output -= polyBlep(wrapPhase(phase + 0.5f), increment);
            return output;
        }

        case Waveform::pulse:
        {
            constexpr float duty = 0.28f;
            auto output = phase < duty ? 1.0f : -1.0f;
            output += polyBlep(phase, increment);
            output -= polyBlep(wrapPhase(phase - duty), increment);
            return output;
        }
    }

    return 0.0f;
}

bool AnalogSynthEngine::acceptsMessage(const Sf2Engine::LayerConfig& config,
                                       const juce::MidiMessage& message)
{
    if (!config.enabled)
        return false;
    if (config.midiChannel > 0 && message.getChannel() != config.midiChannel)
        return false;
    if (!message.isNoteOnOrOff())
        return true;

    const auto note = message.getNoteNumber();
    return note >= config.lowNote && note <= config.highNote;
}

float AnalogSynthEngine::nextEnvelope(Envelope& envelope, float sustain,
                                      const std::array<float, 3>& coefficients)
{
    sustain = juce::jlimit(0.0f, 1.0f, sustain);

    switch (envelope.stage)
    {
        case EnvelopeStage::idle:
            envelope.value = 0.0f;
            break;

        case EnvelopeStage::attack:
            envelope.value = 1.0f + (envelope.value - 1.0f) * coefficients[0];
            if (envelope.value >= 0.9995f)
            {
                envelope.value = 1.0f;
                envelope.stage = EnvelopeStage::decay;
            }
            break;

        case EnvelopeStage::decay:
            envelope.value = sustain + (envelope.value - sustain) * coefficients[1];
            if (std::abs(envelope.value - sustain) <= 0.0005f)
            {
                envelope.value = sustain;
                envelope.stage = EnvelopeStage::sustain;
            }
            break;

        case EnvelopeStage::sustain:
        case EnvelopeStage::legato:
            envelope.value = sustain;
            break;

        case EnvelopeStage::release:
            // Keep a minimum 15 ms release, matching the web version's
            // click-safe stopVoice() even when a preset asks for zero.
            envelope.value = 0.0001f + (envelope.value - 0.0001f) * coefficients[2];
            if (envelope.value <= 0.00011f)
            {
                envelope.value = 0.0f;
                envelope.stage = EnvelopeStage::idle;
            }
            break;
    }

    return envelope.value;
}

void AnalogSynthEngine::noteOn(int layerIndex, int midiChannel, int note,
                               float velocity, const Config& config, int splitSourceNote,
                               int patternPan)
{
    auto& layer = layers[static_cast<size_t>(layerIndex)];
    const auto mono = (config.monophonic || config.routing.mono)
        && !(config.routing.nordArp.enabled && config.routing.nordArp.mode == 1);
    Voice* voice = nullptr;

    if (mono)
    {
        const auto heldIndex = (static_cast<size_t>(midiChannel - 1) * 128u) + static_cast<size_t>(note);
        layer.heldNotes[heldIndex] = true;
        layer.heldVelocities[heldIndex] = juce::jlimit(0.0f, 1.0f, velocity);
        layer.noteOrder[heldIndex] = ++layer.noteSequence;
        voice = &layer.voices.front();
    }
    else
    {
        const auto voiceLimit = voicesPerLayer;
        if (config.nordRetriggerSameNote)
            for (int i = 0; i < voiceLimit; ++i)
            {
                auto& candidate = layer.voices[(size_t) i];
                if (candidate.active && candidate.note == note && candidate.midiChannel == midiChannel)
                {
                    voice = &candidate;
                    break;
                }
            }
        for (int i = 0; voice == nullptr && i < voiceLimit; ++i)
        {
            auto& candidate = layer.voices[(size_t) i];
            if (!candidate.active)
            {
                voice = &candidate;
                break;
            }
        }

        if (voice == nullptr)
            voice = &layer.voices.front();
    }

    const auto frequency = midiNoteToFrequency(note + config.routing.octave * 12);
    const auto splitGain = config.routing.nordSplit.gain(splitSourceNote >= 0 ? splitSourceNote : note);
    if (config.browserCompatible && mono && voice->active && voice->browserVoice)
    {
        // The approved lab treats the first 80 ms as a simultaneous chord.
        if (voice->age < static_cast<uint64_t>(sampleRate * 0.08))
            return;
        voice->transition.retarget(sampleRate);
        voice->note = note;
        voice->midiChannel = midiChannel;
        voice->splitGain = splitGain;
        voice->patternPan = juce::jlimit(-1, 1, patternPan);
        voice->keyDown = true;
        voice->targetFrequency = frequency;
        voice->amp.stage = EnvelopeStage::legato;
        return;
    }
    const auto legato = mono && voice->active && voice->keyDown;
    voice->transition.retarget(sampleRate);

    voice->active = true;
    voice->keyDown = true;
    voice->note = note;
    voice->midiChannel = midiChannel;
    voice->velocity = juce::jlimit(0.0f, 1.0f, velocity);
    voice->splitGain = splitGain;
    voice->patternPan = juce::jlimit(-1, 1, patternPan);
    voice->targetFrequency = frequency;

    // Analog uses the browser's Mono/Legato behavior: retune immediately and
    // preserve the envelope while another key is held. There is no glide mode.
    voice->currentFrequency = frequency;

    if (!legato)
    {
        voice->phase1 = voice->phase2 = voice->phase3 = 0.0f;
        voice->ladder = {};
        voice->ladderInput = {};
        voice->ladderFeedback = voice->ladderPrevious = 0.0f;
        voice->noiseState = 0x12345678u ^ static_cast<uint32_t>(note * 1664525);
        voice->amp = { 0.0f, 0.0f, EnvelopeStage::attack };
        voice->filter = { 0.0f, 0.0f, EnvelopeStage::attack };
        voice->biquadX1 = voice->biquadX2 = voice->biquadY1 = voice->biquadY2 = 0;
        voice->browserLfoPhase = 0;
        voice->age = 0;
        voice->browserVoice = config.browserCompatible;
        if (config.browserCompatible)
            voice->amp.value = 0.0001f;
    }
}

void AnalogSynthEngine::noteOff(int layerIndex, int midiChannel, int note, const Config& config)
{
    auto& layer = layers[static_cast<size_t>(layerIndex)];
    const auto mono = (config.monophonic || config.routing.mono)
        && !(config.routing.nordArp.enabled && config.routing.nordArp.mode == 1);

    if (mono)
    {
        const auto releasedIndex = static_cast<size_t>(midiChannel - 1) * 128u + static_cast<size_t>(note);
        if (!layer.heldNotes[releasedIndex]) return;
        layer.heldNotes[releasedIndex] = false;

        size_t replacementIndex = layer.heldNotes.size();
        uint64_t newest = 0;
        for (size_t candidate = 0; candidate < layer.heldNotes.size(); ++candidate)
        {
            if (layer.heldNotes[candidate] && layer.noteOrder[candidate] >= newest)
            {
                newest = layer.noteOrder[candidate];
                replacementIndex = candidate;
            }
        }

        auto& voice = layer.voices.front();
        if (replacementIndex < layer.heldNotes.size())
        {
            const auto replacement = static_cast<int>(replacementIndex % 128u);
            const auto replacementChannel = static_cast<int>(replacementIndex / 128u) + 1;
            if (voice.note == replacement && voice.midiChannel == replacementChannel) return;
            voice.transition.retarget(sampleRate);
            voice.active = true;
            voice.keyDown = true;
            voice.note = replacement;
            voice.midiChannel = replacementChannel;
            voice.velocity = layer.heldVelocities[replacementIndex];
            voice.splitGain = config.routing.nordSplit.gain(replacement);
            voice.targetFrequency = midiNoteToFrequency(replacement + config.routing.octave * 12);
            if (config.browserCompatible)
                voice.amp.stage = EnvelopeStage::legato;
            return; // Last-note priority with a continuous envelope: true legato.
        }

        if (voice.active)
        {
            voice.keyDown = false;
            if (!layer.sustainPedal[static_cast<size_t>(midiChannel - 1)])
            {
                voice.amp.releaseStart = voice.amp.value;
                voice.filter.releaseStart = voice.filter.value;
                voice.amp.stage = EnvelopeStage::release;
                voice.amp.elapsed = 0;
                voice.filter.stage = EnvelopeStage::release;
            }
        }
        return;
    }

    for (auto& voice : layer.voices)
    {
        if (voice.active && voice.note == note && voice.midiChannel == midiChannel)
        {
            voice.keyDown = false;
            if (!layer.sustainPedal[static_cast<size_t>(midiChannel - 1)])
            {
                voice.amp.releaseStart = voice.amp.value;
                voice.filter.releaseStart = voice.filter.value;
                voice.amp.stage = EnvelopeStage::release;
                voice.amp.elapsed = 0;
                voice.filter.stage = EnvelopeStage::release;
            }
        }
    }
}

float AnalogSynthEngine::nextBrowserEnvelope(Voice& voice, const Config& config)
{
    auto& e = voice.amp;
    const auto sustain = std::max(0.001f, 0.34f * config.ampSustain);
    const auto attackSamples = std::max(1.0, sampleRate * config.ampAttackMs * 0.001);
    const auto decaySamples = std::max(1.0, sampleRate * config.ampDecayMs * 0.001);
    switch (e.stage)
    {
        case EnvelopeStage::attack:
            e.value = static_cast<float>(0.0001 * std::pow(3400.0, std::min(1.0, e.elapsed / attackSamples)));
            if (++e.elapsed >= attackSamples) { e.stage = EnvelopeStage::decay; e.elapsed = 0; }
            break;
        case EnvelopeStage::decay:
            e.value = static_cast<float>(0.34 * std::pow(sustain / 0.34, std::min(1.0, e.elapsed / decaySamples)));
            if (++e.elapsed >= decaySamples) { e.stage = EnvelopeStage::sustain; e.elapsed = 0; }
            break;
        case EnvelopeStage::sustain:
            e.value = sustain;
            break;
        case EnvelopeStage::legato:
            e.value = sustain + (e.value - sustain) * static_cast<float>(std::exp(-1.0 / (sampleRate * 0.008)));
            break;
        case EnvelopeStage::release:
        {
            const auto release = config.ampDecaySwitch ? std::max(0.015, config.ampReleaseMs * 0.001) : 0.015;
            const auto tau = std::max(0.003, release / 5.0);
            e.value = 0.0001f + (e.value - 0.0001f) * static_cast<float>(std::exp(-1.0 / (sampleRate * tau)));
            // Mono keeps its oscillators running for the next note, as the lab
            // does. Poly voices are disconnected after release + 30 ms.
            if (++e.elapsed > (release + 0.03) * sampleRate
                && !(config.monophonic || config.routing.mono))
            {
                e.value = 0;
                e.stage = EnvelopeStage::idle;
            }
            break;
        }
        case EnvelopeStage::idle: e.value = 0; break;
    }
    return e.value;
}

float AnalogSynthEngine::processBrowserFilter(Voice& v, float input, float cutoffHz, float resonanceDb)
{
    // W3C Web Audio lowpass coefficients. Q is expressed in dB, not linear Q:
    // https://www.w3.org/TR/webaudio-1.0/#filters-characteristics
    const auto frequency = juce::jlimit(0.01, sampleRate * 0.4999, static_cast<double>(cutoffHz));
    if (v.browserCutoff != frequency || v.browserResonance != resonanceDb)
    {
        const auto omega = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const auto cosine = std::cos(omega);
        const auto alpha = std::sin(omega) / (2.0 * std::pow(10.0, resonanceDb / 20.0));
        const auto a0 = 1.0 + alpha;
        v.browserCoefficients = { (1.0 - cosine) * 0.5 / a0,
            (1.0 - cosine) / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0 };
        v.browserCutoff = frequency;
        v.browserResonance = resonanceDb;
    }
    const auto& c = v.browserCoefficients;
    const auto output = c[0] * input + c[1] * v.biquadX1 + c[0] * v.biquadX2
                      - c[2] * v.biquadY1 - c[3] * v.biquadY2;
    v.biquadX2 = v.biquadX1; v.biquadX1 = input;
    v.biquadY2 = v.biquadY1; v.biquadY1 = output;
    return static_cast<float>(output);
}

float AnalogSynthEngine::processLadder(Voice& voice, float input, float cutoffHz,
                                       float resonance, float drive) noexcept
{
    const auto nyquistSafe = juce::jlimit(20.0f, static_cast<float>(sampleRate) * 0.42f, cutoffHz);
    const auto oversampledRate = static_cast<float>(sampleRate) * 2.0f;
    if (voice.ladderCutoff != nyquistSafe)
    {
        voice.ladderCoefficient = juce::jlimit(0.0001f, 0.98f,
            1.0f - std::exp(-twoPi * nyquistSafe / oversampledRate));
        voice.ladderCutoff = nyquistSafe;
    }
    const auto g = voice.ladderCoefficient;
    const auto feedback = juce::jlimit(0.0f, 3.9f, resonance * 3.9f);
    const auto saturatedDrive = juce::jlimit(0.25f, 4.0f, drive);
    float output = voice.ladderPrevious;

    // Two internal passes reduce the high-frequency foldback that made the
    // old Analog layer brittle at high resonance.
    for (int pass = 0; pass < 2; ++pass)
    {
        auto stageInput = std::tanh(input * saturatedDrive - feedback * voice.ladderFeedback);
        for (size_t stage = 0; stage < voice.ladder.size(); ++stage)
        {
            const auto nonlinearInput = std::tanh(stageInput);
            const auto nonlinearState = std::tanh(voice.ladder[stage]);
            voice.ladder[stage] += g * (nonlinearInput - nonlinearState);
            stageInput = voice.ladder[stage];
        }

        voice.ladderFeedback = voice.ladder.back();
        output = 0.5f * (voice.ladder.back() + voice.ladderPrevious);
        voice.ladderPrevious = voice.ladder.back();
    }

    return output;
}

void AnalogSynthEngine::renderVoice(Voice& voice, const Config& config, float lfo,
                                    float modWheel, float pitchRatio, float browserBaseCutoff,
                                    const std::array<float, 3>& oscillatorRatios,
                                    const std::array<float, 3>& ampCoefficients,
                                    const std::array<float, 3>& filterCoefficients,
                                    float& left, float& right)
{
    // No portamento/glide in Analog: Mono/Legato transitions are immediate,
    // matching the browser implementation and avoiding theremin-like slides.
    voice.currentFrequency = voice.targetFrequency;

    const auto amp = config.browserCompatible ? nextBrowserEnvelope(voice, config)
        : nextEnvelope(voice.amp, config.ampSustain, ampCoefficients);
    const auto filterEnvelope = nextEnvelope(voice.filter, config.filterSustain, filterCoefficients);
    if (voice.amp.stage == EnvelopeStage::idle)
    {
        voice.active = false;
        return;
    }

    const auto base = voice.currentFrequency * pitchRatio;
    const auto increment1 = juce::jlimit(0.0f, 0.49f, base * oscillatorRatios[0] / static_cast<float>(sampleRate));
    const auto increment2 = juce::jlimit(0.0f, 0.49f, base * oscillatorRatios[1] / static_cast<float>(sampleRate));
    const auto increment3 = juce::jlimit(0.0f, 0.49f, base * oscillatorRatios[2] / static_cast<float>(sampleRate));
    const auto nordOscControl = juce::jlimit(0.0f, 1.0f,
        (config.nordOscControl + filterEnvelope * config.nordOscEnvelopeAmount * 127.0f) / 127.0f);
    const auto waveSample = [&](Waveform wave, float phase, float increment)
    {
        // Web Audio's saw is centred on the positive zero crossing.
        if (config.browserCompatible && wave == Waveform::saw)
            phase = wrapPhase(phase + 0.5f);
        if (!config.browserCompatible && config.nordSynthType == 0 && wave == Waveform::pulse)
        {
            phase = wrapPhase(phase);
            const auto duty = 0.1f + 0.8f * nordOscControl;
            auto output = phase < duty ? 1.0f : -1.0f;
            output += polyBlep(phase, increment);
            output -= polyBlep(wrapPhase(phase - duty), increment);
            return output;
        }
        return waveform(wave, phase, increment);
    };
    const float p1 = voice.phase1, p2 = voice.phase2, p3 = voice.phase3;
    voice.phase1 = wrapPhase(voice.phase1 + increment1);
    voice.phase2 = wrapPhase(voice.phase2 + increment2);
    voice.phase3 = wrapPhase(voice.phase3 + increment3);

    auto sample = 0.0f;
    // FM/WAVE replace the oscillator mix below; evaluating it first is wasted.
    if (config.nordSynthType < 1 || config.nordSynthType > 3)
    {
        if (config.oscillator1Enabled)
            sample += waveSample(config.oscillator1Wave, config.browserCompatible ? p1 : voice.phase1, increment1) * config.oscillator1Level;
        if (config.oscillator2Enabled)
            sample += waveSample(config.oscillator2Wave, config.browserCompatible ? p2 : voice.phase2, increment2) * config.oscillator2Level;
        if (config.oscillator3Enabled)
            sample += waveSample(config.oscillator3Wave, config.browserCompatible ? p3 : voice.phase3, increment3) * config.oscillator3Level;
    }

    if (config.nordSynthType == 1 || config.nordSynthType == 2)
    {
        const auto depth = 0.15f + 1.15f * nordOscControl
                         + 0.12f * (float)(std::abs(config.nordTimbre) % 6);
        const auto modulator = std::sin(juce::MathConstants<float>::twoPi * p2
            * (config.nordSynthType == 1 ? 2.0f : 3.0f));
        sample = 0.68f * std::sin(juce::MathConstants<float>::twoPi * p1 + depth * modulator)
            + 0.18f * std::sin(juce::MathConstants<float>::twoPi * p3);
    }
    else if (config.nordSynthType == 3)
    {
        const auto harmonic = 2.0f + (float)(std::abs(config.nordTimbre) % 4) + 5.0f * nordOscControl;
        sample = 0.55f * std::sin(juce::MathConstants<float>::twoPi * p1)
            + 0.25f * std::sin(juce::MathConstants<float>::twoPi * p1 * harmonic)
            + 0.12f * std::sin(juce::MathConstants<float>::twoPi * p2 * (harmonic + 1.0f));
    }

    auto noise = nextNoise(voice.noiseState);
    if (config.pinkNoise)
        noise = 0.98f * voice.ladderInput[0] + 0.02f * noise;
    voice.ladderInput[0] = noise;
    sample += noise * config.noiseLevel;

    const auto mixDrive = 1.0f + juce::jlimit(0.0f, 1.0f, config.mixerDrive) * 3.0f;
    if (!config.browserCompatible || config.mixerDrive > 0)
        sample = std::tanh(sample * mixDrive) / mixDrive;

    if (config.browserCompatible)
    {
        const auto browserLfo = static_cast<float>(std::sin(voice.browserLfoPhase * juce::MathConstants<double>::twoPi));
        voice.browserLfoPhase += config.lfoRateHz / sampleRate;
        voice.browserLfoPhase -= std::floor(voice.browserLfoPhase);
        sample = processBrowserFilter(voice, sample,
            browserBaseCutoff + browserLfo * config.lfoToFilter * 4.2f, config.resonance * 20.0f);
        sample *= amp * 0.45f; // Browser master; its VCA is velocity insensitive.
    }
    else
    {
        const auto tracking = juce::jlimit(0.0f, 1.0f, config.filterKeyboardTracking);
        if (voice.trackingNote != voice.note || voice.trackingAmount != tracking)
        {
            const auto keyboardOctaves = (static_cast<float>(voice.note) - 60.0f) / 12.0f;
            voice.trackingGain = std::pow(2.0f, keyboardOctaves * tracking);
            voice.trackingNote = voice.note;
            voice.trackingAmount = tracking;
        }
        const auto modulation = lfo * (config.lfoToFilter + modWheel * config.modWheelToFilter);
        const auto normalizedCutoff = juce::jlimit(0.0f, 1.0f,
            (config.cutoff + filterEnvelope * config.filterEnvelopeAmount * 100.0f + modulation) / 100.0f);
        const auto cutoffHz = 25.0f * std::pow(700.0f, normalizedCutoff) * voice.trackingGain;
        sample = processLadder(voice, sample, cutoffHz, config.resonance, config.filterDrive);
        sample *= amp * voice.velocity;
    }
    sample *= voice.splitGain;
    ++voice.age;
    sample = voice.transition.process(sample);

    left += sample * (voice.patternPan > 0 ? 0.0f : 1.0f);
    right += sample * (voice.patternPan < 0 ? 0.0f : 1.0f);
}

void AnalogSynthEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                                const std::array<Config, layerCount>& configs,
                                const std::array<juce::MidiBuffer, layerCount>* routedMidi)
{
    auto dispatch = [&](int layerIndex, const juce::MidiMessage& message)
    {
        const auto& config = configs[static_cast<size_t>(layerIndex)];
        if (!acceptsMessage(config.routing, message))
            return;

        auto& layer = layers[static_cast<size_t>(layerIndex)];
        auto emitArp = [&](const NordArpeggiator::Event& event)
        {
            if (event.gateEvent)
                layer.arpeggiatorGate.setTargetValue(event.gateOpen ? 1.0f : 0.0f);
            else if (event.noteOn)
                noteOn(layerIndex, event.channel, event.note, event.velocity / 127.0f,
                       config, event.sourceNote, event.pan);
            else
                noteOff(layerIndex, event.channel, event.note, config);
        };

        if (message.isController() && message.getControllerNumber() == 1)
        {
            layer.modWheel = static_cast<float>(message.getControllerValue()) / 127.0f;
        }
        else if (message.isPitchWheel() && config.routing.pitchBendEnabled)
        {
            // Standard MIDI pitch-bend range is ±2 semitones. Apply it to all
            // active voices in the layer, preserving their envelopes and
            // phases so bending never retriggers or clicks the note.
            const auto normalized = (static_cast<float>(message.getPitchWheelValue()) - 8192.0f)
                                  / 8192.0f;
            layer.pitchBendSemitones = juce::jlimit(-2.0f, 2.0f, normalized * 2.0f);
        }
        else if (message.isController() && message.getControllerNumber() == 64)
        {
            if (config.routing.nordArp.enabled)
                layer.arpeggiator.controller(message.getChannel(), 64,
                    config.routing.sustainEnabled ? message.getControllerValue() : 0, emitArp);
            if (!config.routing.sustainEnabled)
                return;

            const auto channelIndex = static_cast<size_t>(message.getChannel() - 1);
            const auto sustainDown = message.getControllerValue() >= 64;
            if (layer.sustainPedal[channelIndex] && !sustainDown)
            {
                for (auto& voice : layer.voices)
                {
                    if (voice.active && !voice.keyDown && voice.midiChannel == message.getChannel())
                    {
                        voice.amp.releaseStart = voice.amp.value;
                        voice.filter.releaseStart = voice.filter.value;
                        voice.amp.stage = EnvelopeStage::release;
                        voice.amp.elapsed = 0;
                        voice.filter.stage = EnvelopeStage::release;
                    }
                }
            }

            layer.sustainPedal[channelIndex] = sustainDown;
        }
        else if (message.isAllSoundOff() || message.isAllNotesOff())
        {
            if (config.routing.nordArp.enabled)
                layer.arpeggiator.controller(message.getChannel(),
                    message.isAllSoundOff() ? 120 : 123, 0, emitArp);
            for (auto& voice : layer.voices)
            {
                if (!voice.active || voice.midiChannel != message.getChannel()) continue;
                voice.keyDown = false;
                if (message.isAllSoundOff()) voice = {};
                else
                {
                    voice.amp.releaseStart = voice.amp.value;
                    voice.filter.releaseStart = voice.filter.value;
                    voice.amp.stage = EnvelopeStage::release;
                    voice.amp.elapsed = 0;
                    voice.filter.stage = EnvelopeStage::release;
                }
            }
            const auto channelIndex = static_cast<size_t>(message.getChannel() - 1);
            const auto firstHeld = channelIndex * 128u;
            for (size_t note = firstHeld; note < firstHeld + 128u; ++note)
                layer.heldNotes[note] = false;
            layer.sustainPedal[channelIndex] = false;

            const auto mono = (config.monophonic || config.routing.mono)
                && !(config.routing.nordArp.enabled && config.routing.nordArp.mode == 1);
            if (mono)
            {
                size_t replacementIndex = layer.heldNotes.size();
                uint64_t newest = 0;
                for (size_t candidate = 0; candidate < layer.heldNotes.size(); ++candidate)
                    if (layer.heldNotes[candidate] && layer.noteOrder[candidate] >= newest)
                    {
                        newest = layer.noteOrder[candidate];
                        replacementIndex = candidate;
                    }
                if (replacementIndex < layer.heldNotes.size())
                {
                    const auto replacement = static_cast<int>(replacementIndex % 128u);
                    const auto replacementChannel = static_cast<int>(replacementIndex / 128u) + 1;
                    auto& voice = layer.voices.front();
                    if (!voice.active || voice.note != replacement || voice.midiChannel != replacementChannel)
                    {
                        if (message.isAllSoundOff() || !voice.active)
                            noteOn(layerIndex, replacementChannel, replacement,
                                   layer.heldVelocities[replacementIndex], config);
                        else
                        {
                            voice.transition.retarget(sampleRate);
                            voice.active = true;
                            voice.keyDown = true;
                            voice.note = replacement;
                            voice.midiChannel = replacementChannel;
                            voice.velocity = layer.heldVelocities[replacementIndex];
                            voice.splitGain = config.routing.nordSplit.gain(replacement);
                            voice.targetFrequency = midiNoteToFrequency(replacement + config.routing.octave * 12);
                            if (config.browserCompatible)
                                voice.amp.stage = EnvelopeStage::legato;
                        }
                    }
                }
            }
        }
        else if (message.isNoteOn())
        {
            if (!config.routing.acceptsVelocity((int) message.getVelocity())) return;
            const auto velocity = config.routing.velocitySensitivityOff
                ? (100.0f / 127.0f) : message.getFloatVelocity();
            if (config.routing.nordArp.enabled)
            {
                layer.arpeggiator.input(message.getChannel(), message.getNoteNumber(),
                    juce::jlimit(1, 127, juce::roundToInt(velocity * 127.0f)), true,
                    emitArp, config.routing.nordArp.keyboardSync);
                if (config.routing.nordArp.mode != 2) return;
            }
            noteOn(layerIndex, message.getChannel(), message.getNoteNumber(), velocity, config);
        }
        else if (message.isNoteOff())
        {
            if (config.routing.nordArp.enabled)
            {
                layer.arpeggiator.input(message.getChannel(), message.getNoteNumber(), 0, false, emitArp);
                if (config.routing.nordArp.mode != 2) return;
            }
            noteOff(layerIndex, message.getChannel(), message.getNoteNumber(), config);
        }
    };

    for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        auto& layer = layers[static_cast<size_t>(layerIndex)];
        const auto& config = configs[static_cast<size_t>(layerIndex)];
        if (!config.routing.enabled)
        {
            peaks[static_cast<size_t>(layerIndex)].store(0.0f, std::memory_order_relaxed);
            continue;
        }

        const juce::MidiBuffer empty;
        const auto& routedMessages = routedMidi != nullptr ? (*routedMidi)[(size_t) layerIndex] : empty;
        const auto hasVoices = std::any_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active; });
        if (!hasVoices && midi.isEmpty() && routedMessages.isEmpty() && !layer.effectTailPending
            && !layer.arpeggiator.busy())
        {
            layer.lfoPhase = wrapPhase(layer.lfoPhase + static_cast<float>(
                config.lfoRateHz * output.getNumSamples() / sampleRate));
            if (!layer.gainReady)
            {
                layer.gain.setCurrentAndTargetValue(config.routing.gain);
                layer.gainReady = true;
            }
            layer.gain.setTargetValue(config.routing.gain);
            layer.gain.skip(output.getNumSamples());
            peaks[static_cast<size_t>(layerIndex)].store(0.0f, std::memory_order_relaxed);
            continue;
        }

        // These pitches belong to the layer, not to individual samples or voices.
        const std::array<float, 3> oscillatorRatios {
            std::exp2((config.oscillator1Semitones + config.oscillator1FineCents * 0.01f) / 12.0f),
            std::exp2((config.oscillator2Semitones + config.oscillator2FineCents * 0.01f) / 12.0f),
            std::exp2((config.oscillator3Semitones + config.oscillator3FineCents * 0.01f) / 12.0f)
        };

        renderScratch.setSize(2, output.getNumSamples(), false, false, true);
        renderScratch.clear();
        if (!layer.gainReady)
        {
            layer.gain.setCurrentAndTargetValue(config.routing.gain);
            layer.gainReady = true;
        }
        layer.gain.setTargetValue(config.routing.gain);
        auto host = midi.begin();
        auto routed = routedMessages.begin();
        float peak = 0.0f;
        const auto browserBaseCutoff = config.browserCompatible
            ? 25.0f * std::pow(700.0f, config.cutoff / 100.0f) : 0.0f;
        const std::array<float, 3> ampCoefficients {
            envelopeCoefficient(config.ampAttackMs, sampleRate),
            envelopeCoefficient(config.ampDecayMs, sampleRate),
            envelopeCoefficient(juce::jmax(15.0f, config.ampReleaseMs), sampleRate)
        };
        const std::array<float, 3> filterCoefficients {
            envelopeCoefficient(config.filterAttackMs, sampleRate),
            envelopeCoefficient(config.filterDecayMs, sampleRate),
            envelopeCoefficient(juce::jmax(15.0f, config.filterReleaseMs), sampleRate)
        };
        std::array<Voice*, voicesPerLayer> activeVoices {};
        size_t activeCount = 0;
        const auto collectActiveVoices = [&]
        {
            activeCount = 0;
            for (auto& voice : layer.voices)
                if (voice.active) activeVoices[activeCount++] = &voice;
        };
        collectActiveVoices();
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
        {
            bool voicesChanged = false;
            while (host != midi.end() || routed != routedMessages.end())
            {
                const bool useHost = routed == routedMessages.end()
                    || (host != midi.end() && (*host).samplePosition <= (*routed).samplePosition);
                const auto event = useHost ? *host : *routed;
                if (event.samplePosition > sampleIndex) break;
                dispatch(layerIndex, event.getMessage());
                voicesChanged = true;
                if (useHost) ++host; else ++routed;
            }
            if (config.routing.nordArp.enabled || layer.arpeggiator.busy())
            {
                auto emitArp = [&](const NordArpeggiator::Event& event)
                {
                    if (!event.gateEvent) voicesChanged = true;
                    if (event.gateEvent)
                        layer.arpeggiatorGate.setTargetValue(event.gateOpen ? 1.0f : 0.0f);
                    else if (event.noteOn)
                        noteOn(layerIndex, event.channel, event.note, event.velocity / 127.0f,
                           config, event.sourceNote, event.pan);
                    else
                        noteOff(layerIndex, event.channel, event.note, config);
                };
                layer.arpeggiator.tick(config.routing.nordArp, emitArp);
            }
            if (voicesChanged) collectActiveVoices();
            layer.lfoPhase = wrapPhase(layer.lfoPhase
                + config.lfoRateHz / static_cast<float>(sampleRate));
            const auto lfo = std::sin(layer.lfoPhase * twoPi);
            float left = 0.0f, right = 0.0f;

            const auto pitchDepth = config.lfoToPitch + layer.modWheel * config.modWheelToPitch;
            const auto pitchRatio = std::pow(2.0f,
                (layer.pitchBendSemitones + lfo * pitchDepth) / 12.0f);
            size_t survivors = 0;
            for (size_t i = 0; i < activeCount; ++i)
            {
                auto& voice = *activeVoices[i];
                renderVoice(voice, config, lfo, layer.modWheel, pitchRatio,
                            browserBaseCutoff, oscillatorRatios, ampCoefficients,
                            filterCoefficients, left, right);
                if (voice.active) activeVoices[survivors++] = &voice;
            }
            activeCount = survivors;

            const auto gain = layer.gain.getNextValue();
            const auto gate = config.routing.nordArp.mode == 2
                ? layer.arpeggiatorGate.getNextValue()
                : layer.arpeggiatorGate.isSmoothing() ? layer.arpeggiatorGate.getNextValue() : 1.0f;
            renderScratch.setSample(0, sampleIndex, left * gain * gate);
            renderScratch.setSample(1, sampleIndex, right * gain * gate);
        }

        const auto cutoffHz = 120.0f + juce::jlimit(0.0f, 100.0f, config.cutoff) * 180.0f;
        const auto alpha = juce::jlimit(0.001f, 0.95f,
            1.0f - std::exp(-twoPi * cutoffHz / static_cast<float>(sampleRate)));
        if (!config.browserCompatible)
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
            for (int channel = 0; channel < 2; ++channel)
            {
                auto sample = renderScratch.getSample(channel, sampleIndex);
                auto& state = layer.filterState[(size_t) channel];
                state += alpha * (sample - state);
                renderScratch.setSample(channel, sampleIndex, state);
            }

        layer.nordEffects.process(renderScratch, config.routing.nordFx);
        juce::Reverb::Parameters parameters;
        parameters.roomSize = juce::jlimit(0.0f, 1.0f, config.routing.reverbSize / 100.0f);
        parameters.damping = juce::jlimit(0.0f, 1.0f, config.routing.reverbDamping / 100.0f);
        parameters.width = juce::jlimit(0.0f, 1.0f, config.routing.reverbWidth / 100.0f);
        setLayerReverbMix(parameters, config.routing.reverb / 100.0f);
        layer.reverb.setParameters(parameters);
        if (layer.reverb.shouldProcess())
            layer.reverb.processStereo(renderScratch.getWritePointer(0), renderScratch.getWritePointer(1),
                                       output.getNumSamples());

        if (config.routing.compressor > 0.001f)
        {
            const auto threshold = juce::Decibels::decibelsToGain(config.routing.compressorThreshold);
            const auto ratio = juce::jmax(1.0f, config.routing.compressorRatio);
            const auto attackCoeff = std::exp(-1.0f / (0.001f * juce::jmax(0.1f, config.routing.compressorAttack) * static_cast<float>(sampleRate)));
            const auto releaseCoeff = std::exp(-1.0f / (0.001f * juce::jmax(1.0f, config.routing.compressorRelease) * static_cast<float>(sampleRate)));
            const auto makeup = juce::Decibels::decibelsToGain(config.routing.compressorMakeup);
            const auto mix = juce::jlimit(0.0f, 1.0f, config.routing.compressor / 100.0f);
            for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto dry = renderScratch.getSample(channel, sampleIndex);
                    const auto level = std::abs(dry);
                    auto& env = layer.compressorEnvelope[(size_t) channel];
                    env = level > env ? attackCoeff * env + (1.0f - attackCoeff) * level
                                      : releaseCoeff * env + (1.0f - releaseCoeff) * level;
                    const auto over = env > threshold ? env / threshold : 1.0f;
                    const auto reduction = over > 1.0f ? std::pow(over, -(1.0f - 1.0f / ratio)) : 1.0f;
                    renderScratch.setSample(channel, sampleIndex,
                        dry * (1.0f + mix * (reduction * makeup - 1.0f)));
                }
        }

        layer.eq.setParameters(config.routing.eqLow, config.routing.eqMid,
                               config.routing.eqHigh, sampleRate,
                               config.routing.eqLowFrequency,
                               config.routing.eqMidFrequency,
                               config.routing.eqHighFrequency,
                               config.routing.eqLowQ, config.routing.eqMidQ,
                               config.routing.eqHighQ);
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
            for (int channel = 0; channel < 2; ++channel)
                renderScratch.setSample(channel, sampleIndex,
                    layer.eq.process(renderScratch.getSample(channel, sampleIndex), channel));

        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
        {
            const auto left = renderScratch.getSample(0, sampleIndex);
            const auto right = renderScratch.getSample(1, sampleIndex);
            peak = juce::jmax(peak, juce::jmax(std::abs(left), std::abs(right)));
            if (output.getNumChannels() > 0) output.addSample(0, sampleIndex, left);
            if (output.getNumChannels() > 1) output.addSample(1, sampleIndex, right);
        }

        peaks[static_cast<size_t>(layerIndex)].store(peak, std::memory_order_relaxed);
        const auto voicesRemaining = std::any_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active; });
        updateLayerEffectTail(layer.effectTailPending, layer.effectQuietSamples,
                              voicesRemaining, peak, output.getNumSamples(), sampleRate);
    }
}
