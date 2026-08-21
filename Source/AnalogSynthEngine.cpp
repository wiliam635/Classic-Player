#include "AnalogSynthEngine.h"

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
float exponentialApproach(float current, float target, float milliseconds,
                          double sampleRate) noexcept
{
    const auto duration = juce::jmax(3.0f, milliseconds) * 0.001f;
    const auto tau = duration / 5.0f;
    const auto coefficient = std::exp(-1.0f / juce::jmax(1.0, tau * sampleRate));
    return target + (current - target) * static_cast<float>(coefficient);
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
}

void AnalogSynthEngine::reset()
{
    for (auto& layer : layers)
    {
        layer = {};
        layer.lfoPhase = 0.0f;
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

    layers[static_cast<size_t>(layerIndex)] = {};
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

float AnalogSynthEngine::nextEnvelope(Envelope& envelope, float attackMs, float decayMs,
                                      float sustain, float releaseMs, double rate)
{
    sustain = juce::jlimit(0.0f, 1.0f, sustain);

    switch (envelope.stage)
    {
        case EnvelopeStage::idle:
            envelope.value = 0.0f;
            break;

        case EnvelopeStage::attack:
            envelope.value = exponentialApproach(envelope.value, 1.0f, attackMs, rate);
            if (envelope.value >= 0.9995f)
            {
                envelope.value = 1.0f;
                envelope.stage = EnvelopeStage::decay;
            }
            break;

        case EnvelopeStage::decay:
            envelope.value = exponentialApproach(envelope.value, sustain, decayMs, rate);
            if (std::abs(envelope.value - sustain) <= 0.0005f)
            {
                envelope.value = sustain;
                envelope.stage = EnvelopeStage::sustain;
            }
            break;

        case EnvelopeStage::sustain:
            envelope.value = sustain;
            break;

        case EnvelopeStage::release:
            // Keep a minimum 15 ms release, matching the web version's
            // click-safe stopVoice() even when a preset asks for zero.
            envelope.value = exponentialApproach(envelope.value, 0.0001f,
                                                  juce::jmax(15.0f, releaseMs), rate);
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
                               float velocity, const Config& config)
{
    auto& layer = layers[static_cast<size_t>(layerIndex)];
    const auto mono = config.monophonic || config.routing.mono;
    Voice* voice = nullptr;

    if (mono)
    {
        layer.heldNotes[static_cast<size_t>(note)] = true;
        layer.heldVelocities[static_cast<size_t>(note)] = juce::jlimit(0.0f, 1.0f, velocity);
        layer.noteOrder[static_cast<size_t>(note)] = ++layer.noteSequence;
        voice = &layer.voices.front();
    }
    else
    {
        for (auto& candidate : layer.voices)
            if (!candidate.active)
            {
                voice = &candidate;
                break;
            }

        if (voice == nullptr)
            voice = &layer.voices.front();
    }

    const auto frequency = midiNoteToFrequency(note + config.routing.octave * 12
                                               + static_cast<int>(config.oscillator1Semitones));
    const auto legato = mono && voice->active && voice->keyDown;

    voice->active = true;
    voice->keyDown = true;
    voice->note = note;
    voice->midiChannel = midiChannel;
    voice->velocity = juce::jlimit(0.0f, 1.0f, velocity);
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
    }
}

void AnalogSynthEngine::noteOff(int layerIndex, int midiChannel, int note, const Config& config)
{
    auto& layer = layers[static_cast<size_t>(layerIndex)];
    const auto mono = config.monophonic || config.routing.mono;

    if (mono)
    {
        layer.heldNotes[static_cast<size_t>(note)] = false;

        int replacement = -1;
        uint64_t newest = 0;
        for (int candidate = 0; candidate < 128; ++candidate)
        {
            if (layer.heldNotes[static_cast<size_t>(candidate)]
                && layer.noteOrder[static_cast<size_t>(candidate)] >= newest)
            {
                newest = layer.noteOrder[static_cast<size_t>(candidate)];
                replacement = candidate;
            }
        }

        auto& voice = layer.voices.front();
        if (replacement >= 0)
        {
            voice.active = true;
            voice.keyDown = true;
            voice.note = replacement;
            voice.midiChannel = midiChannel;
            voice.velocity = layer.heldVelocities[static_cast<size_t>(replacement)];
            voice.targetFrequency = midiNoteToFrequency(replacement + config.routing.octave * 12
                                                         + static_cast<int>(config.oscillator1Semitones));
            return; // Last-note priority with a continuous envelope: true legato.
        }

        if (voice.active)
        {
            voice.keyDown = false;
            if (!layer.sustainPedal)
            {
                voice.amp.releaseStart = voice.amp.value;
                voice.filter.releaseStart = voice.filter.value;
                voice.amp.stage = EnvelopeStage::release;
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
            if (!layer.sustainPedal)
            {
                voice.amp.releaseStart = voice.amp.value;
                voice.filter.releaseStart = voice.filter.value;
                voice.amp.stage = EnvelopeStage::release;
                voice.filter.stage = EnvelopeStage::release;
            }
        }
    }
}

float AnalogSynthEngine::processLadder(Voice& voice, float input, float cutoffHz,
                                       float resonance, float drive) noexcept
{
    const auto nyquistSafe = juce::jlimit(20.0f, static_cast<float>(sampleRate) * 0.42f, cutoffHz);
    const auto oversampledRate = static_cast<float>(sampleRate) * 2.0f;
    const auto g = juce::jlimit(0.0001f, 0.98f,
        1.0f - std::exp(-twoPi * nyquistSafe / oversampledRate));
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
                                    float modWheel, float pitchBendSemitones,
                                    float& left, float& right)
{
    // No portamento/glide in Analog: Mono/Legato transitions are immediate,
    // matching the browser implementation and avoiding theremin-like slides.
    voice.currentFrequency = voice.targetFrequency;

    const auto amp = nextEnvelope(voice.amp, config.ampAttackMs, config.ampDecayMs,
                                  config.ampSustain, config.ampReleaseMs, sampleRate);
    const auto filterEnvelope = nextEnvelope(voice.filter, config.filterAttackMs,
                                             config.filterDecayMs, config.filterSustain,
                                             config.filterReleaseMs, sampleRate);
    if (voice.amp.stage == EnvelopeStage::idle)
    {
        voice.active = false;
        return;
    }

    const auto pitchDepth = config.lfoToPitch + modWheel * config.modWheelToPitch;
    const auto pitchRatio = std::pow(2.0f,
        (pitchBendSemitones + lfo * pitchDepth) / 12.0f);
    const auto base = voice.currentFrequency * pitchRatio;
    const auto ratio2 = std::pow(2.0f, (config.oscillator2Semitones
                                         + config.oscillator2FineCents * 0.01f) / 12.0f);
    const auto ratio3 = std::pow(2.0f, (config.oscillator3Semitones
                                         + config.oscillator3FineCents * 0.01f) / 12.0f);

    const auto increment1 = juce::jlimit(0.0f, 0.49f, base / static_cast<float>(sampleRate));
    const auto increment2 = juce::jlimit(0.0f, 0.49f, base * ratio2 / static_cast<float>(sampleRate));
    const auto increment3 = juce::jlimit(0.0f, 0.49f, base * ratio3 / static_cast<float>(sampleRate));
    voice.phase1 = wrapPhase(voice.phase1 + increment1);
    voice.phase2 = wrapPhase(voice.phase2 + increment2);
    voice.phase3 = wrapPhase(voice.phase3 + increment3);

    auto sample = 0.0f;
    if (config.oscillator1Enabled)
        sample += waveform(config.oscillator1Wave, voice.phase1, increment1) * config.oscillator1Level;
    if (config.oscillator2Enabled)
        sample += waveform(config.oscillator2Wave, voice.phase2, increment2) * config.oscillator2Level;
    if (config.oscillator3Enabled)
        sample += waveform(config.oscillator3Wave, voice.phase3, increment3) * config.oscillator3Level;

    auto noise = nextNoise(voice.noiseState);
    if (config.pinkNoise)
        noise = 0.98f * voice.ladderInput[0] + 0.02f * noise;
    voice.ladderInput[0] = noise;
    sample += noise * config.noiseLevel;

    const auto mixDrive = 1.0f + juce::jlimit(0.0f, 1.0f, config.mixerDrive) * 3.0f;
    sample = std::tanh(sample * mixDrive) / mixDrive;

    const auto keyboardOctaves = (static_cast<float>(voice.note) - 60.0f) / 12.0f;
    const auto keyboardTracking = std::pow(2.0f, keyboardOctaves
                                           * juce::jlimit(0.0f, 1.0f, config.filterKeyboardTracking));
    const auto modulation = lfo * (config.lfoToFilter + modWheel * config.modWheelToFilter);
    const auto normalizedCutoff = juce::jlimit(0.0f, 1.0f,
        (config.cutoff + filterEnvelope * config.filterEnvelopeAmount * 100.0f + modulation) / 100.0f);
    const auto cutoffHz = 25.0f * std::pow(700.0f, normalizedCutoff) * keyboardTracking;

    sample = processLadder(voice, sample, cutoffHz, config.resonance, config.filterDrive);
    sample *= amp * voice.velocity * config.routing.gain;

    left += sample;
    right += sample;
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

        if (message.isController() && message.getControllerNumber() == 1)
        {
            layer.modWheel = static_cast<float>(message.getControllerValue()) / 127.0f;
        }
        else if (message.isPitchWheel())
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
            if (!config.routing.sustainEnabled)
                return;

            const auto sustainDown = message.getControllerValue() >= 64;
            if (layer.sustainPedal && !sustainDown)
            {
                for (auto& voice : layer.voices)
                {
                    if (voice.active && !voice.keyDown)
                    {
                        voice.amp.releaseStart = voice.amp.value;
                        voice.filter.releaseStart = voice.filter.value;
                        voice.amp.stage = EnvelopeStage::release;
                        voice.filter.stage = EnvelopeStage::release;
                    }
                }
            }

            layer.sustainPedal = sustainDown;
        }
        else if (message.isNoteOn())
        {
            noteOn(layerIndex, message.getChannel(), message.getNoteNumber(),
                   message.getFloatVelocity(), config);
        }
        else if (message.isNoteOff())
        {
            noteOff(layerIndex, message.getChannel(), message.getNoteNumber(), config);
        }
    };

    for (const auto metadata : midi)
        for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
            dispatch(layerIndex, metadata.getMessage());

    if (routedMidi != nullptr)
        for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
            for (const auto metadata : (*routedMidi)[static_cast<size_t>(layerIndex)])
                dispatch(layerIndex, metadata.getMessage());

    for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        auto& layer = layers[static_cast<size_t>(layerIndex)];
        const auto& config = configs[static_cast<size_t>(layerIndex)];
        if (!config.routing.enabled)
        {
            peaks[static_cast<size_t>(layerIndex)].store(0.0f, std::memory_order_relaxed);
            continue;
        }

        renderScratch.setSize(2, output.getNumSamples(), false, false, true);
        renderScratch.clear();
        float peak = 0.0f;
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
        {
            layer.lfoPhase = wrapPhase(layer.lfoPhase
                + config.lfoRateHz / static_cast<float>(sampleRate));
            const auto lfo = std::sin(layer.lfoPhase * twoPi);
            float left = 0.0f, right = 0.0f;

            for (auto& voice : layer.voices)
                if (voice.active)
                    renderVoice(voice, config, lfo, layer.modWheel,
                               layer.pitchBendSemitones, left, right);

            renderScratch.setSample(0, sampleIndex, left);
            renderScratch.setSample(1, sampleIndex, right);
        }

        const auto cutoffHz = 120.0f + juce::jlimit(0.0f, 100.0f, config.cutoff) * 180.0f;
        const auto alpha = juce::jlimit(0.001f, 0.95f,
            1.0f - std::exp(-twoPi * cutoffHz / static_cast<float>(sampleRate)));
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
            for (int channel = 0; channel < 2; ++channel)
            {
                auto sample = renderScratch.getSample(channel, sampleIndex);
                auto& state = layer.filterState[(size_t) channel];
                state += alpha * (sample - state);
                renderScratch.setSample(channel, sampleIndex, state);
            }

        if (config.reverb > 0.001f)
        {
            juce::Reverb::Parameters parameters;
            parameters.roomSize = juce::jlimit(0.0f, 1.0f, config.reverbSize / 100.0f);
            parameters.damping = juce::jlimit(0.0f, 1.0f, config.reverbDamping / 100.0f);
            parameters.width = juce::jlimit(0.0f, 1.0f, config.reverbWidth / 100.0f);
            parameters.wetLevel = juce::jlimit(0.0f, 1.0f, config.reverb / 100.0f);
            parameters.dryLevel = 1.0f;
            layer.reverb.setParameters(parameters);
            layer.reverb.processStereo(renderScratch.getWritePointer(0), renderScratch.getWritePointer(1),
                                       output.getNumSamples());
        }

        if (config.compressor > 0.001f)
        {
            const auto threshold = juce::Decibels::decibelsToGain(config.compressorThreshold);
            const auto ratio = juce::jmax(1.0f, config.compressorRatio);
            const auto attackCoeff = std::exp(-1.0f / (0.001f * juce::jmax(0.1f, config.compressorAttack) * static_cast<float>(sampleRate)));
            const auto releaseCoeff = std::exp(-1.0f / (0.001f * juce::jmax(1.0f, config.compressorRelease) * static_cast<float>(sampleRate)));
            const auto makeup = juce::Decibels::decibelsToGain(config.compressorMakeup);
            const auto mix = juce::jlimit(0.0f, 1.0f, config.compressor / 100.0f);
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

        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
        {
            const auto left = renderScratch.getSample(0, sampleIndex);
            const auto right = renderScratch.getSample(1, sampleIndex);
            peak = juce::jmax(peak, juce::jmax(std::abs(left), std::abs(right)));
            if (output.getNumChannels() > 0) output.addSample(0, sampleIndex, left);
            if (output.getNumChannels() > 1) output.addSample(1, sampleIndex, right);
        }

        peaks[static_cast<size_t>(layerIndex)].store(peak, std::memory_order_relaxed);
    }
}

