#include "AnalogSynthEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float twoPi = juce::MathConstants<float>::twoPi;

float wrapPhase(float value)
{
    return value - std::floor(value);
}

float millisecondsToStep(float milliseconds, double sampleRate)
{
    return 1.0f / juce::jmax(1.0f, milliseconds * 0.001f * static_cast<float>(sampleRate));
}
}

AnalogSynthEngine::AnalogSynthEngine()
{
    for (auto& peak : peaks) peak.store(0.0f, std::memory_order_relaxed);
}

void AnalogSynthEngine::prepare(double newSampleRate, int)
{
    sampleRate = juce::jmax(1.0, newSampleRate);
    reset();
}

void AnalogSynthEngine::reset()
{
    for (auto& layer : layers)
    {
        layer = {};
        layer.lfoPhase = 0.0f;
    }
    for (auto& peak : peaks) peak.store(0.0f, std::memory_order_relaxed);
}

void AnalogSynthEngine::stopAllSounds()
{
    reset();
}

void AnalogSynthEngine::unload(int layerIndex)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return;
    layers[(size_t) layerIndex] = {};
    peaks[(size_t) layerIndex].store(0.0f, std::memory_order_relaxed);
}

float AnalogSynthEngine::getLayerPeak(int layer) const noexcept
{
    return juce::isPositiveAndBelow(layer, layerCount)
        ? peaks[(size_t) layer].load(std::memory_order_relaxed) : 0.0f;
}

float AnalogSynthEngine::midiNoteToFrequency(int note) noexcept
{
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

float AnalogSynthEngine::waveform(Waveform form, float phase)
{
    phase = wrapPhase(phase);
    switch (form)
    {
        case Waveform::triangle: return 1.0f - 4.0f * std::abs(phase - 0.5f);
        case Waveform::saw:      return 2.0f * phase - 1.0f;
        case Waveform::square:   return phase < 0.5f ? 1.0f : -1.0f;
        case Waveform::pulse:    return phase < 0.28f ? 1.0f : -1.0f;
    }
    return 0.0f;
}

bool AnalogSynthEngine::acceptsMessage(const Sf2Engine::LayerConfig& config,
                                       const juce::MidiMessage& message)
{
    if (!config.enabled) return false;
    if (config.midiChannel > 0 && message.getChannel() != config.midiChannel) return false;
    if (!message.isNoteOnOrOff()) return true;
    const auto note = message.getNoteNumber();
    return note >= config.lowNote && note <= config.highNote;
}

float AnalogSynthEngine::nextEnvelope(Envelope& envelope, float attackMs, float decayMs,
                                      float sustain, float releaseMs, double rate)
{
    switch (envelope.stage)
    {
        case EnvelopeStage::idle:
            envelope.value = 0.0f;
            break;
        case EnvelopeStage::attack:
            envelope.value += millisecondsToStep(attackMs, rate);
            if (envelope.value >= 1.0f)
            {
                envelope.value = 1.0f;
                envelope.stage = EnvelopeStage::decay;
            }
            break;
        case EnvelopeStage::decay:
            envelope.value -= millisecondsToStep(decayMs, rate);
            if (envelope.value <= sustain)
            {
                envelope.value = sustain;
                envelope.stage = EnvelopeStage::sustain;
            }
            break;
        case EnvelopeStage::sustain:
            envelope.value = sustain;
            break;
        case EnvelopeStage::release:
            envelope.value -= millisecondsToStep(releaseMs, rate) * juce::jmax(0.05f, envelope.releaseStart);
            if (envelope.value <= 0.0001f)
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
    auto& layer = layers[(size_t) layerIndex];
    const auto mono = config.monophonic || config.routing.mono;
    Voice* voice = nullptr;

    if (mono)
    {
        layer.heldNotes[(size_t) note] = true;
        layer.heldVelocities[(size_t) note] = juce::jlimit(0.0f, 1.0f, velocity);
        layer.noteOrder[(size_t) note] = ++layer.noteSequence;
        voice = &layer.voices.front();
    }
    else
    {
        for (auto& candidate : layer.voices)
            if (!candidate.active) { voice = &candidate; break; }
        if (voice == nullptr) voice = &layer.voices.front();
    }

    const auto frequency = midiNoteToFrequency(note + config.routing.octave * 12);
    const auto keepEnvelope = mono && voice->active && voice->keyDown;
    const auto glideSamples = juce::jmax(0.0f, config.glideMs) * 0.001f * static_cast<float>(sampleRate);

    voice->active = true;
    voice->keyDown = true;
    voice->note = note;
    voice->midiChannel = midiChannel;
    voice->velocity = juce::jlimit(0.0f, 1.0f, velocity);
    voice->targetFrequency = frequency;
    if (!keepEnvelope || glideSamples < 1.0f)
        voice->currentFrequency = frequency;
    if (!keepEnvelope)
    {
        voice->phase1 = voice->phase2 = voice->phase3 = 0.0f;
        voice->ladder = {};
        voice->amp = { 0.0f, 0.0f, EnvelopeStage::attack };
        voice->filter = { 0.0f, 0.0f, EnvelopeStage::attack };
    }
}

void AnalogSynthEngine::noteOff(int layerIndex, int midiChannel, int note, const Config& config)
{
    auto& layer = layers[(size_t) layerIndex];
    const auto mono = config.monophonic || config.routing.mono;

    if (mono)
    {
        layer.heldNotes[(size_t) note] = false;
        auto replacement = -1;
        uint64_t newest = 0;
        for (int candidate = 0; candidate < 128; ++candidate)
            if (layer.heldNotes[(size_t) candidate] && layer.noteOrder[(size_t) candidate] >= newest)
            {
                newest = layer.noteOrder[(size_t) candidate];
                replacement = candidate;
            }

        auto& voice = layer.voices.front();
        if (replacement >= 0)
        {
            voice.active = true;
            voice.keyDown = true;
            voice.note = replacement;
            voice.midiChannel = midiChannel;
            voice.velocity = layer.heldVelocities[(size_t) replacement];
            voice.targetFrequency = midiNoteToFrequency(replacement + config.routing.octave * 12);
            return; // Preserve envelope and glide for true legato.
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

void AnalogSynthEngine::renderVoice(Voice& voice, const Config& config, float lfo,
                                    float& left, float& right)
{
    const auto glideStep = config.glideMs <= 0.0f ? 1.0f
        : juce::jlimit(0.00001f, 1.0f, 1.0f / (config.glideMs * 0.001f * static_cast<float>(sampleRate)));
    voice.currentFrequency += (voice.targetFrequency - voice.currentFrequency) * glideStep;

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

    const auto pitchRatio = std::pow(2.0f, (lfo * config.lfoToPitch) / 12.0f);
    const auto base = voice.currentFrequency * pitchRatio;
    const auto ratio2 = std::pow(2.0f, config.oscillator2Semitones / 12.0f);
    const auto ratio3 = std::pow(2.0f, config.oscillator3Semitones / 12.0f);

    voice.phase1 = wrapPhase(voice.phase1 + base / static_cast<float>(sampleRate));
    voice.phase2 = wrapPhase(voice.phase2 + base * ratio2 / static_cast<float>(sampleRate));
    voice.phase3 = wrapPhase(voice.phase3 + base * ratio3 / static_cast<float>(sampleRate));
    voice.noiseState = std::fmod(voice.noiseState * 196314165.0f + 907633515.0f, 2147483647.0f);
    const auto noise = (voice.noiseState / 1073741823.5f) - 1.0f;

    auto sample = waveform(config.oscillator1Wave, voice.phase1) * config.oscillator1Level
                + waveform(config.oscillator2Wave, voice.phase2) * config.oscillator2Level
                + waveform(config.oscillator3Wave, voice.phase3) * config.oscillator3Level
                + noise * config.noiseLevel;
    sample *= 0.45f;

    const auto normalizedCutoff = juce::jlimit(0.0f, 1.0f,
        (config.cutoff + filterEnvelope * config.filterEnvelopeAmount * 100.0f
         + lfo * config.lfoToFilter) / 100.0f);
    const auto cutoffHz = 30.0f * std::pow(600.0f, normalizedCutoff);
    const auto coefficient = juce::jlimit(0.001f, 0.95f,
        1.0f - std::exp(-twoPi * cutoffHz / static_cast<float>(sampleRate)));
    // Independent four-stage nonlinear ladder topology. This preserves the
    // rounded low-pass character and musical resonance expected from Analog.
    const auto resonance = juce::jlimit(0.0f, 3.65f, config.resonance * 3.65f);
    auto ladderInput = std::tanh(sample - resonance * voice.ladder[3]);
    for (auto& stage : voice.ladder)
    {
        stage += coefficient * (std::tanh(ladderInput) - std::tanh(stage));
        ladderInput = stage;
    }
    sample = voice.ladder[3] * amp * voice.velocity * config.routing.gain;

    left += sample;
    right += sample;
}

void AnalogSynthEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                                const std::array<Config, layerCount>& configs,
                                const std::array<juce::MidiBuffer, layerCount>* routedMidi)
{
    auto dispatch = [&](int layerIndex, const juce::MidiMessage& message)
    {
        const auto& config = configs[(size_t) layerIndex];
        if (!acceptsMessage(config.routing, message)) return;
        auto& layer = layers[(size_t) layerIndex];

        if (message.isController() && message.getControllerNumber() == 64)
        {
            if (!config.routing.sustainEnabled) return;
            const auto sustainDown = message.getControllerValue() >= 64;
            if (layer.sustainPedal && !sustainDown)
            {
                for (auto& voice : layer.voices)
                    if (voice.active && !voice.keyDown)
                    {
                        voice.amp.releaseStart = voice.amp.value;
                        voice.filter.releaseStart = voice.filter.value;
                        voice.amp.stage = EnvelopeStage::release;
                        voice.filter.stage = EnvelopeStage::release;
                    }
            }
            layer.sustainPedal = sustainDown;
        }
        else if (message.isNoteOn())
            noteOn(layerIndex, message.getChannel(), message.getNoteNumber(),
                   message.getFloatVelocity(), config);
        else if (message.isNoteOff())
            noteOff(layerIndex, message.getChannel(), message.getNoteNumber(), config);
    };

    // VST3/AU hosts supply MIDI in the process buffer.
    for (const auto metadata : midi)
        for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
            dispatch(layerIndex, metadata.getMessage());

    // The JUCE standalone device callback supplies MIDI through these
    // collectors, one buffer per layer. This was missing from the first
    // Analog implementation and made the UI load while producing no sound.
    if (routedMidi != nullptr)
        for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
            for (const auto metadata : (*routedMidi)[(size_t) layerIndex])
                dispatch(layerIndex, metadata.getMessage());

    for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        auto& layer = layers[(size_t) layerIndex];
        const auto& config = configs[(size_t) layerIndex];
        if (!config.routing.enabled)
        {
            peaks[(size_t) layerIndex].store(0.0f, std::memory_order_relaxed);
            continue;
        }
        float peak = 0.0f;
        for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
        {
            layer.lfoPhase = wrapPhase(layer.lfoPhase
                + config.lfoRateHz / static_cast<float>(sampleRate));
            const auto lfo = std::sin(layer.lfoPhase * twoPi);
            float left = 0.0f, right = 0.0f;
            for (auto& voice : layer.voices)
                if (voice.active) renderVoice(voice, config, lfo, left, right);
            peak = juce::jmax(peak, juce::jmax(std::abs(left), std::abs(right)));
            if (output.getNumChannels() > 0) output.addSample(0, sampleIndex, left);
            if (output.getNumChannels() > 1) output.addSample(1, sampleIndex, right);
        }
        peaks[(size_t) layerIndex].store(peak, std::memory_order_relaxed);
    }
}