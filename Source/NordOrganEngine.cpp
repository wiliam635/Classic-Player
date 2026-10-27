#include "NordOrganEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr std::array<double, 9> drawbarRatios { 0.5, 1.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0 };

float drawbarGain(int value) noexcept
{
    const auto level = juce::jlimit(0, 8, value);
    return level == 0 ? 0.0f : 0.085f * std::pow(10.0f, (level - 8) * 3.0f / 20.0f);
}
}

void NordOrganEngine::prepare(double rate, int)
{
    sampleRate = juce::jmax(8000.0, rate);
    for (size_t i = 0; i < sineTable.size(); ++i)
        sineTable[i] = (float) std::sin(twoPi * (double)i / (double)(sineTable.size() - 1));
    for (auto& layer : layers)
    {
        layer.reverb.setSampleRate(sampleRate);
        layer.reverb.reset();
        layer.eq.reset();
        layer.effectTailPending = false;
        layer.effectQuietSamples = 0;
    }
}

void NordOrganEngine::releaseVoice(Voice& voice) noexcept
{
    if (!voice.active || voice.releasing) return;
    voice.down = false;
    voice.releasing = true;
    voice.releaseAge = 0;
    voice.releaseStart = voice.envelope;
}

void NordOrganEngine::unload(int index)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    auto& layer = layers[(size_t) index];
    for (auto& voice : layer.voices) voice = {};
    layer.sustain.fill(false);
    layer.reverb.reset();
    layer.eq.reset();
    layer.cutoffState.fill(0.0f);
    layer.rotaryPhase = 0.0;
    layer.vibratoPhase = 0.0;
    layer.compressorEnvelope = 0.0f;
    layer.enabled = false;
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    peaks[(size_t)index].store(0.0f, std::memory_order_relaxed);
}

void NordOrganEngine::stopAllSounds()
{
    for (int i = 0; i < layerCount; ++i) unload(i);
}

float NordOrganEngine::getLayerPeak(int index) const noexcept
{
    return juce::isPositiveAndBelow(index, layerCount)
        ? peaks[(size_t)index].load(std::memory_order_relaxed) : 0.0f;
}

juce::ValueTree NordOrganEngine::save(const Config& source, int index)
{
    auto state = juce::ValueTree("NordOrgan");
    state.setProperty("layer", index, nullptr);
    state.setProperty("model", static_cast<int>(source.model), nullptr);
    state.setProperty("percussion", source.percussion, nullptr);
    state.setProperty("vibratoChorus", source.vibratoChorus, nullptr);
    state.setProperty("rotary", source.rotary, nullptr);
    state.setProperty("drive", source.drive, nullptr);
    for (int i = 0; i < 9; ++i)
        state.setProperty("drawbar" + juce::String(i + 1), source.drawbars[(size_t)i], nullptr);
    return state;
}

NordOrganEngine::Config NordOrganEngine::restore(const juce::ValueTree& state)
{
    Config result;
    result.model = static_cast<Model>(juce::jlimit(0, 5, (int)state.getProperty("model", 0)));
    result.percussion = juce::jlimit(0, 3, (int)state.getProperty("percussion", 0));
    if (result.percussion != 0 && result.percussion != 2 && result.percussion != 3)
        result.percussion = 0;
    result.vibratoChorus = juce::jlimit(0, 2, (int)state.getProperty("vibratoChorus", 0));
    result.rotary = juce::jlimit(0, 2, (int)state.getProperty("rotary", 1));
    result.drive = juce::jlimit(0.0f, 1.0f, (float)state.getProperty("drive", 0.08f));
    for (int i = 0; i < 9; ++i)
        result.drawbars[(size_t)i] = juce::jlimit(0, 8,
            (int)state.getProperty("drawbar" + juce::String(i + 1), result.drawbars[(size_t)i]));
    return result;
}

float NordOrganEngine::oscillator(double phase) const noexcept
{
    phase -= std::floor(phase);
    const auto position = phase * (double)(sineTable.size() - 1);
    const auto index = (size_t)position;
    const auto fraction = (float)(position - (double)index);
    return sineTable[index] + (sineTable[index + 1] - sineTable[index]) * fraction;
}

void NordOrganEngine::handleMessage(Layer& layer, const Config& config,
                                    const juce::MidiMessage& message)
{
    if (config.routing.midiChannel != 0 && !message.isForChannel(config.routing.midiChannel))
        return;

    const auto channel = juce::jlimit(1, 16, message.getChannel());
    if (message.isController() && message.getControllerNumber() == 64)
    {
        const auto held = message.getControllerValue() >= 64 && config.routing.sustainEnabled;
        layer.sustain[(size_t)(channel - 1)] = held;
        if (!held)
            for (auto& voice : layer.voices)
                if (voice.active && voice.channel == channel && !voice.down) releaseVoice(voice);
        return;
    }
    if (message.isController() && (message.getControllerNumber() == 120
                                   || message.getControllerNumber() == 123))
    {
        for (auto& voice : layer.voices)
            if (voice.active && voice.channel == channel)
                releaseVoice(voice);
        return;
    }
    if (message.isController() && message.getControllerNumber() == 121)
    {
        layer.sustain[(size_t)(channel - 1)] = false;
        for (auto& voice : layer.voices)
            if (voice.active && voice.channel == channel && !voice.down)
                releaseVoice(voice);
        return;
    }
    if (message.isNoteOff())
    {
        for (auto& voice : layer.voices)
            if (voice.active && voice.down && voice.note == message.getNoteNumber()
                && voice.channel == channel)
            {
                voice.down = false;
                if (!layer.sustain[(size_t)(channel - 1)]) releaseVoice(voice);
            }
        return;
    }
    if (!message.isNoteOn() || !config.routing.acceptsVelocity((int)message.getVelocity())) return;
    const auto note = message.getNoteNumber();
    if (note < config.routing.lowNote || note > config.routing.highNote) return;

    auto* selected = &layer.voices[0];
    for (auto& voice : layer.voices)
        if (voice.active && voice.note == note && voice.channel == channel)
            releaseVoice(voice);
    for (auto& voice : layer.voices)
        if (!voice.active) { selected = &voice; break; }
    if (selected->active)
        for (auto& voice : layer.voices)
            if (voice.envelope < selected->envelope) selected = &voice;

    *selected = {};
    selected->active = selected->down = true;
    selected->note = note;
    selected->channel = channel;
    const auto bassOffset = config.model == Model::b3Bass ? -12 : 0;
    const auto base = 440.0 * std::exp2((note + config.routing.octave * 12 + bassOffset - 69) / 12.0);
    for (size_t i = 0; i < drawbarRatios.size(); ++i)
    {
        auto frequency = base * drawbarRatios[i];
        while (frequency > sampleRate * 0.45) frequency *= 0.5;
        selected->step[i] = frequency / sampleRate;
    }
    const auto percussionRatio = config.percussion == 3 ? 3.0 : 2.0;
    selected->percussionStep = std::min(base * percussionRatio, sampleRate * 0.45) / sampleRate;
    selected->percussionEnvelope = config.percussion > 0 ? 0.22f : 0.0f;
}

void NordOrganEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& host,
                              std::array<Config, layerCount>& configs,
                              const std::array<juce::MidiBuffer, layerCount>* routed)
{
    const juce::MidiBuffer empty;
    for (int index = 0; index < layerCount; ++index)
    {
        auto& layer = layers[(size_t)index];
        const auto& config = configs[(size_t)index];
        if (!config.routing.enabled)
        {
            if (layer.enabled) unload(index);
            continue;
        }
        layer.enabled = true;
        if (!config.routing.sustainEnabled)
            for (int channel = 0; channel < 16; ++channel)
                if (layer.sustain[(size_t)channel])
                {
                    layer.sustain[(size_t)channel] = false;
                    for (auto& voice : layer.voices)
                        if (voice.active && voice.channel == channel + 1 && !voice.down)
                            releaseVoice(voice);
                }
        const auto& local = routed != nullptr ? (*routed)[(size_t)index] : empty;
        const auto hasVoices = std::any_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active; });
        if (!hasVoices && host.isEmpty() && local.isEmpty() && !layer.effectTailPending)
        {
            const auto samples = output.getNumSamples();
            layer.vibratoPhase = std::fmod(layer.vibratoPhase
                + samples * twoPi * 5.5 / sampleRate, twoPi);
            const auto rotaryRate = config.rotary == 2 ? 5.8 : config.rotary == 1 ? 0.75 : 0.0;
            layer.rotaryPhase = std::fmod(layer.rotaryPhase
                + samples * twoPi * rotaryRate / sampleRate, twoPi);
            peaks[(size_t) index].store(0.0f, std::memory_order_relaxed);
            continue;
        }
        auto hostEvent = host.cbegin();
        auto localEvent = local.cbegin();
        const auto reverbAmount = juce::jlimit(0.0f, 1.0f, config.routing.reverb / 100.0f);
        const auto compressorMix = juce::jlimit(0.0f, 1.0f, config.routing.compressor / 100.0f);
        const auto compressorThreshold = juce::Decibels::decibelsToGain(
            juce::jlimit(-60.0f, 0.0f, config.routing.compressorThreshold));
        const auto compressorRatio = juce::jmax(1.0f,
            juce::jlimit(1.0f, 20.0f, config.routing.compressorRatio));
        const auto compressorAttack = std::exp(-1.0f / juce::jmax(1.0f,
            (float)(sampleRate * juce::jlimit(0.1f, 200.0f, config.routing.compressorAttack) * 0.001f)));
        const auto compressorRelease = std::exp(-1.0f / juce::jmax(1.0f,
            (float)(sampleRate * juce::jlimit(5.0f, 2000.0f, config.routing.compressorRelease) * 0.001f)));
        const auto compressorMakeup = juce::Decibels::decibelsToGain(
            juce::jlimit(-12.0f, 24.0f, config.routing.compressorMakeup));
        juce::Reverb::Parameters reverb;
        reverb.roomSize = juce::jlimit(0.0f, 1.0f, config.routing.reverbSize / 100.0f);
        reverb.damping = juce::jlimit(0.0f, 1.0f, config.routing.reverbDamping / 100.0f);
        reverb.width = juce::jlimit(0.0f, 1.0f, config.routing.reverbWidth / 100.0f);
        setLayerReverbMix(reverb, reverbAmount);
        layer.reverb.setParameters(reverb);
        layer.eq.setParameters(config.routing.eqLow, config.routing.eqMid, config.routing.eqHigh,
            sampleRate, config.routing.eqLowFrequency, config.routing.eqMidFrequency,
            config.routing.eqHighFrequency, config.routing.eqLowQ,
            config.routing.eqMidQ, config.routing.eqHighQ);
        const auto cutoffAmount = juce::jlimit(0.0f, 100.0f, config.routing.cutoff);
        const auto cutoffFrequency = 80.0f * std::pow(250.0f, cutoffAmount / 100.0f);
        const auto cutoffCoefficient = cutoffAmount >= 99.5f ? 0.0f
            : std::exp(-juce::MathConstants<float>::twoPi * cutoffFrequency
                       / static_cast<float>(sampleRate));
        std::array<float, 9> drawbarGains {};
        for (size_t bar = 0; bar < drawbarGains.size(); ++bar)
            drawbarGains[bar] = drawbarGain(config.drawbars[bar]);
        const auto attackSamples = juce::jmax(1, (int)(sampleRate
            * layerAttackSeconds(config.routing.attack)));
        const auto releaseSamples = juce::jmax(1, (int)(sampleRate
            * layerReleaseSeconds(config.routing.release)));
        const auto percussionDecay = std::exp(-1.0f / (0.115f * (float) sampleRate));
        const auto drive = juce::jlimit(0.0f, 1.0f, config.drive);
        const auto rotaryRate = config.rotary == 2 ? 5.8 : config.rotary == 1 ? 0.75 : 0.0;
        const auto layerGain = juce::jlimit(0.0f, 1.5f, config.routing.gain);

        float peak = 0.0f;
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            while (hostEvent != host.cend() && (*hostEvent).samplePosition <= sample)
                handleMessage(layer, config, (*hostEvent++).getMessage());
            while (localEvent != local.cend() && (*localEvent).samplePosition <= sample)
                handleMessage(layer, config, (*localEvent++).getMessage());

            const auto vibratoDepth = config.vibratoChorus == 2 ? 0.0022
                : config.vibratoChorus == 1 ? 0.0011 : 0.0;
            const auto vibrato = 1.0 + vibratoDepth * std::sin(layer.vibratoPhase);
            layer.vibratoPhase += twoPi * 5.5 / sampleRate;
            if (layer.vibratoPhase >= twoPi) layer.vibratoPhase -= twoPi;

            float mono = 0.0f;
            for (auto& voice : layer.voices)
            {
                if (!voice.active) continue;
                if (voice.releasing)
                {
                    if (voice.releaseAge >= releaseSamples)
                    {
                        voice.active = false;
                        voice.envelope = 0.0f;
                        continue;
                    }
                    voice.envelope = voice.releaseStart * 0.5f
                        * (1.0f + (float)std::cos(juce::MathConstants<double>::pi
                            * (double)++voice.releaseAge / releaseSamples));
                }
                else voice.envelope = 0.24f * juce::jmin(1.0f,
                    (float)++voice.age / (float)attackSamples);

                float voiceSample = 0.0f;
                for (size_t bar = 0; bar < 9; ++bar)
                {
                    const auto gain = drawbarGains[bar];
                    if (gain <= 0.0f) continue;
                    const auto phase = voice.phase[bar];
                    float partial = oscillator(phase);
                    if (config.model == Model::vox)
                        partial = 0.76f * partial + 0.24f * oscillator(phase * 3.0);
                    else if (config.model == Model::farfisa)
                        partial = 0.62f * partial + 0.25f * oscillator(phase * 2.0)
                                + 0.13f * oscillator(phase * 3.0);
                    else if (config.model == Model::pipe1)
                        partial = 0.92f * partial + 0.08f * oscillator(phase * 2.0);
                    else if (config.model == Model::pipe2)
                        partial = 0.76f * partial + 0.18f * oscillator(phase * 2.0)
                                + 0.06f * oscillator(phase * 3.0);
                    voiceSample += partial * gain;
                    voice.phase[bar] += voice.step[bar] * vibrato;
                    voice.phase[bar] -= std::floor(voice.phase[bar]);
                }
                if (voice.percussionEnvelope > 1.0e-5f)
                {
                    voiceSample += oscillator(voice.percussionPhase) * voice.percussionEnvelope;
                    voice.percussionPhase += voice.percussionStep;
                    voice.percussionPhase -= std::floor(voice.percussionPhase);
                    voice.percussionEnvelope *= percussionDecay;
                }
                mono += voiceSample * voice.envelope * 0.62f;
            }

            mono = (1.0f - drive) * mono + drive * std::tanh(mono * (1.0f + drive * 5.0f));
            layer.rotaryPhase += twoPi * rotaryRate / sampleRate;
            if (layer.rotaryPhase >= twoPi) layer.rotaryPhase -= twoPi;
            const auto rotaryPan = config.rotary == 0 ? 0.0f
                : 0.22f * (float) std::sin(layer.rotaryPhase);
            const auto pan = juce::jlimit(-1.0f, 1.0f, config.routing.pan + rotaryPan);
            float left = mono * layerGain * std::sqrt(0.5f * (1.0f - pan));
            float right = mono * layerGain * std::sqrt(0.5f * (1.0f + pan));
            layer.reverb.processStereo(&left, &right, 1);
            left = layer.eq.process(left, 0);
            right = layer.eq.process(right, 1);
            if (cutoffAmount < 99.5f)
            {
                auto& leftState = layer.cutoffState[0];
                auto& rightState = layer.cutoffState[1];
                leftState = (1.0f - cutoffCoefficient) * left + cutoffCoefficient * leftState;
                rightState = (1.0f - cutoffCoefficient) * right + cutoffCoefficient * rightState;
                left = leftState;
                right = rightState;
            }
            if (compressorMix > 0.0f)
            {
                const auto detector = juce::jmax(std::abs(left), std::abs(right));
                const auto coefficient = detector > layer.compressorEnvelope
                    ? compressorAttack : compressorRelease;
                layer.compressorEnvelope = coefficient * layer.compressorEnvelope
                    + (1.0f - coefficient) * detector;
                auto compressorGain = 1.0f;
                if (layer.compressorEnvelope > compressorThreshold)
                {
                    const auto compressed = compressorThreshold
                        + (layer.compressorEnvelope - compressorThreshold) / compressorRatio;
                    compressorGain = compressed / juce::jmax(1.0e-8f, layer.compressorEnvelope);
                }
                const auto wetGain = compressorGain * compressorMakeup;
                left *= 1.0f + compressorMix * (wetGain - 1.0f);
                right *= 1.0f + compressorMix * (wetGain - 1.0f);
            }
            peak = juce::jmax(peak, juce::jmax(std::abs(left), std::abs(right)));
            if (output.getNumChannels() > 0) output.addSample(0, sample, left);
            if (output.getNumChannels() > 1) output.addSample(1, sample, right);
        }
        peaks[(size_t)index].store(peak, std::memory_order_relaxed);
        const auto voicesRemaining = std::any_of(layer.voices.begin(), layer.voices.end(),
            [](const auto& voice) { return voice.active; });
        updateLayerEffectTail(layer.effectTailPending, layer.effectQuietSamples,
                              voicesRemaining, peak, output.getNumSamples(), sampleRate);
    }
}
