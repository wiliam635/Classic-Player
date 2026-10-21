#include "NordSampleEngine.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr float envelopeFloor = 0.0001f;
}

void NordSampleEngine::prepare(double sampleRate, int blockSize)
{
    currentSampleRate = juce::jmax(1.0, sampleRate);
    maximumBlockSize = juce::jmax(1, blockSize);
    for (int i = 0; i < layerCount; ++i)
    {
        scratch[(size_t) i].setSize(2, maximumBlockSize, false, false, true);
        combinedMidi[(size_t) i].ensureSize(4096);
        auto& layer = layers[(size_t) i];
        layer.reverb.setSampleRate(currentSampleRate);
        layer.reverb.reset();
        layer.eq.reset();
        layer.nordFx.prepare(currentSampleRate);
        layer.gain.reset(currentSampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(0.8f);
    }
}

juce::Result NordSampleEngine::loadSample(int layerIndex, const juce::File& file)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount) || !file.existsAsFile()
        || !file.hasFileExtension(".nsmp4"))
        return juce::Result::fail("Selecione um arquivo de amostra Nord NSMP4 (.nsmp4).");

    NordSampleDecoder::Instrument decoded;
    const auto result = NordSampleDecoder::decode(file, decoded);
    if (result.failed()) return result;

    // The NSMP marker gives us the loop start but leaves the loop end at the
    // end of the stored stroke. Blend a short tail into the loop head offline
    // so the callback can wrap without doing correlation or allocation work.
    for (auto& zone : decoded.zones)
    {
        const auto start = zone.loopStart;
        const auto end = zone.loopEnd;
        if (end <= start + 32 || end > zone.frames()) continue;
        const auto fade = juce::jmin((size_t) (NordSampleDecoder::sampleRate * 0.02),
                                     (end - start) / 4);
        if (fade < 8) continue;
        for (size_t i = 0; i < fade; ++i)
        {
            const auto amount = (float) (i + 1) / (float) (fade + 1);
            for (int channel = 0; channel < zone.channels; ++channel)
            {
                const auto tail = (end - fade + i) * (size_t) zone.channels + (size_t) channel;
                const auto head = (start + i) * (size_t) zone.channels + (size_t) channel;
                zone.pcm[tail] += (zone.pcm[head] - zone.pcm[tail]) * amount;
            }
        }
        zone.loopStart = start + fade;
    }

    const juce::ScopedLock stateLock(lock);
    auto& layer = layers[(size_t) layerIndex];
    layer.instrument = std::move(decoded);
    layer.sourcePath = file.getFullPathName();
    layer.loaded = true;
    layer.sustainDown.fill(false);
    layer.cutoffState.fill(0.0f);
    layer.compressorEnvelope.fill(0.0f);
    layer.eq.reset();
    layer.nordFx.reset();
    layer.reverb.reset();
    layer.lastReverbParameters.fill(-1.0f);
    for (auto& voice : layer.voices) voice = {};
    layer.peak.store(0.0f, std::memory_order_release);
    return juce::Result::ok();
}

void NordSampleEngine::unload(int layerIndex)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return;
    const juce::ScopedLock stateLock(lock);
    auto& layer = layers[(size_t) layerIndex];
    layer.instrument = {};
    layer.sourcePath.clear();
    layer.loaded = false;
    layer.sustainDown.fill(false);
    for (auto& voice : layer.voices) voice = {};
    layer.eq.reset();
    layer.nordFx.reset();
    layer.reverb.reset();
    layer.peak.store(0.0f, std::memory_order_release);
}

void NordSampleEngine::stopAllSounds()
{
    const juce::ScopedLock stateLock(lock);
    for (auto& layer : layers)
    {
        layer.sustainDown.fill(false);
        layer.nordFx.reset();
        for (auto& voice : layer.voices) voice = {};
    }
}

bool NordSampleEngine::isLoaded(int layerIndex) const
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return false;
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() && layers[(size_t) layerIndex].loaded;
}

juce::String NordSampleEngine::path(int layerIndex) const
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return {};
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() ? layers[(size_t) layerIndex].sourcePath : juce::String{};
}

juce::String NordSampleEngine::name(int layerIndex) const
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return {};
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() ? layers[(size_t) layerIndex].instrument.name : juce::String{};
}

float NordSampleEngine::getLayerPeak(int layerIndex) const
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return 0.0f;
    return layers[(size_t) layerIndex].peak.load(std::memory_order_relaxed);
}

NordSampleEngine::Voice& NordSampleEngine::allocateVoice(Layer& layer, int polyphony) noexcept
{
    Voice* candidate = &layer.voices.front();
    const auto limit = juce::jlimit(64, (int) layer.voices.size(), polyphony);
    const auto rank = [](const Voice& voice)
    {
        return voice.released ? 0 : voice.waitingForSustain ? 1 : 2;
    };
    for (int i = 0; i < limit; ++i)
    {
        auto& voice = layer.voices[(size_t) i];
        if (!voice.active) return voice;
        const auto voiceRank = rank(voice);
        const auto candidateRank = rank(*candidate);
        if (voiceRank < candidateRank
            || (voiceRank == candidateRank && voice.age > candidate->age))
            candidate = &voice;
    }
    return *candidate;
}

int NordSampleEngine::findZone(const NordSampleDecoder::Instrument& instrument,
                               int note, int velocity) noexcept
{
    int best = -1;
    int bestVelocityDistance = 1000;
    for (size_t i = 0; i < instrument.zones.size(); ++i)
    {
        const auto& zone = instrument.zones[i];
        if (note < zone.low || note > zone.high) continue;
        const auto distance = velocity < zone.velocityLow ? zone.velocityLow - velocity
                            : velocity > zone.velocityHigh ? velocity - zone.velocityHigh : 0;
        if (distance < bestVelocityDistance)
        {
            best = (int) i;
            bestVelocityDistance = distance;
            if (distance == 0) break;
        }
    }
    return best;
}

float NordSampleEngine::readSample(const NordSampleDecoder::Zone& zone, int channel,
                                  double position) noexcept
{
    const auto frames = (int) zone.frames();
    if (frames <= 0 || zone.channels <= 0) return 0.0f;
    const auto start = juce::jlimit(0, frames - 1, (int) position);
    const auto fraction = (float) (position - start);
    const auto sourceChannel = juce::jmin(channel, zone.channels - 1);
    const auto read = [&](int frame)
    {
        if (zone.loopEnd > zone.loopStart && frame >= (int) zone.loopEnd)
            frame = (int) zone.loopStart + (frame - (int) zone.loopEnd)
                  % (int) (zone.loopEnd - zone.loopStart);
        frame = juce::jlimit(0, frames - 1, frame);
        return zone.pcm[(size_t) frame * (size_t) zone.channels + (size_t) sourceChannel];
    };
    // Cubic interpolation preserves the high end when the 35.002 kHz Nord
    // source is resampled or played outside its recorded root key.
    const auto y0 = read(start - 1), y1 = read(start), y2 = read(start + 1), y3 = read(start + 2);
    const auto t2 = fraction * fraction, t3 = t2 * fraction;
    return 0.5f * ((2.0f * y1) + (-y0 + y2) * fraction
        + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
        + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
}

float NordSampleEngine::releaseCoefficientFor(float control, double sampleRate) noexcept
{
    const auto value = juce::jlimit(0.0f, 100.0f, control);
    const auto seconds = value <= 50.0f ? juce::jmax(0.005f, value * 0.01f)
        : 0.5f * std::pow(20.0f, (value - 50.0f) / 50.0f);
    return (float) std::exp(std::log(envelopeFloor)
        / juce::jmax(1.0, (double) seconds * sampleRate));
}

void NordSampleEngine::handleMidiMessage(Layer& layer, const juce::MidiMessage& message,
                                         const Sf2Engine::LayerConfig& routing) noexcept
{
    if (routing.midiChannel > 0 && message.getChannel() != routing.midiChannel) return;
    const auto channel = juce::jlimit(1, 16, message.getChannel());
    if (message.isAllSoundOff() || message.isAllNotesOff())
    {
        layer.sustainDown[(size_t) channel - 1] = false;
        for (auto& voice : layer.voices)
            if (voice.active && voice.channel == channel)
            {
                if (message.isAllSoundOff()) voice = {};
                else
                {
                    voice.keyDown = false;
                    voice.waitingForSustain = false;
                    voice.released = true;
                    voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                }
            }
        return;
    }
    if (message.isController() && message.getControllerNumber() == 64)
    {
        const auto wasDown = layer.sustainDown[(size_t) channel - 1];
        const auto isDown = routing.sustainEnabled && message.getControllerValue() >= 64;
        layer.sustainDown[(size_t) channel - 1] = isDown;
        if (wasDown && !isDown)
            for (auto& voice : layer.voices)
                if (voice.active && voice.waitingForSustain && voice.channel == channel)
                {
                    voice.waitingForSustain = false;
                    voice.released = true;
                    voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                }
        return;
    }
    if (message.isNoteOn())
    {
        if (!routing.acceptsVelocity((int) message.getVelocity())) return;
        const auto inputNote = message.getNoteNumber();
        if (inputNote < routing.lowNote || inputNote > routing.highNote) return;
        const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
        auto velocity = juce::jlimit(1, 127, (int) message.getVelocity());
        if (routing.velocityCurve == 1) velocity = juce::jlimit(1, 127, juce::roundToInt(std::sqrt(velocity / 127.0f) * 127.0f));
        else if (routing.velocityCurve == 2) velocity = juce::jlimit(1, 127, juce::roundToInt((velocity / 127.0f) * (velocity / 127.0f) * 127.0f));
        const auto shapedVelocity = velocity / 127.0f;
        const auto zoneIndex = findZone(layer.instrument, note, velocity);
        if (!juce::isPositiveAndBelow(zoneIndex, (int) layer.instrument.zones.size())) return;
        if (routing.mono)
            for (auto& voice : layer.voices)
                if (voice.active && voice.channel == channel)
                {
                    voice.keyDown = false;
                    voice.released = true;
                    voice.waitingForSustain = false;
                    voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                }
        const auto& zone = layer.instrument.zones[(size_t) zoneIndex];
        auto& voice = allocateVoice(layer, routing.polyphony);
        const auto cents = layer.instrument.detune[(size_t) note];
        voice = {};
        voice.active = zone.frames() > 0;
        voice.note = note;
        voice.channel = channel;
        voice.zone = zoneIndex;
        voice.keyDown = true;
        voice.position = 0.0;
        voice.increment = std::pow(2.0, (note - zone.root + cents / 100.0) / 12.0)
                        * NordSampleDecoder::sampleRate / currentSampleRate;
        const auto noteGain = layer.instrument.gain[(size_t) note];
        voice.amplitude = shapedVelocity * zone.gain * juce::jmax(0.0f, noteGain);
        voice.attack = routing.attack <= 0.0f ? 1.0f : 0.0f;
        voice.attackStep = routing.attack <= 0.0f ? 1.0f
            : (float) (1.0 / juce::jmax(1.0, routing.attack * 0.001 * currentSampleRate));
        voice.releaseGain = 1.0f;
        return;
    }
    if (message.isNoteOff())
    {
        const auto inputNote = message.getNoteNumber();
        if (inputNote < routing.lowNote || inputNote > routing.highNote) return;
        const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
        for (auto& voice : layer.voices)
            if (voice.active && voice.keyDown && voice.note == note && voice.channel == channel)
            {
                voice.keyDown = false;
                if (layer.sustainDown[(size_t) channel - 1]) voice.waitingForSustain = true;
                else
                {
                    voice.released = true;
                    voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                }
            }
    }
}

void NordSampleEngine::renderVoices(Layer& layer, juce::AudioBuffer<float>& output,
                                    int startSample, int endSample, float& peak) noexcept
{
    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        if (!juce::isPositiveAndBelow(voice.zone, (int) layer.instrument.zones.size()))
        {
            voice = {};
            continue;
        }
        const auto& zone = layer.instrument.zones[(size_t) voice.zone];
        const auto frames = (int) zone.frames();
        const auto looped = zone.loopEnd > zone.loopStart + 8;
        for (int sample = startSample; sample < endSample; ++sample)
        {
            if (looped && voice.position >= zone.loopEnd)
                voice.position = zone.loopStart + std::fmod(voice.position - zone.loopEnd,
                    (double) (zone.loopEnd - zone.loopStart));
            if (voice.position >= frames)
            {
                voice = {};
                break;
            }
            const auto amplitude = voice.amplitude * voice.attack * voice.releaseGain;
            const auto left = readSample(zone, 0, voice.position) * amplitude;
            const auto right = readSample(zone, 1, voice.position) * amplitude;
            output.addSample(0, sample, left);
            if (output.getNumChannels() > 1) output.addSample(1, sample, right);
            peak = juce::jmax(peak, std::abs(left), std::abs(right));
            voice.attack = juce::jmin(1.0f, voice.attack + voice.attackStep);
            voice.position += voice.increment;
            ++voice.age;
            if (voice.released)
            {
                voice.releaseGain *= voice.releaseCoefficient;
                if (voice.releaseGain < envelopeFloor)
                {
                    voice = {};
                    break;
                }
            }
        }
    }
}

void NordSampleEngine::renderLayer(Layer& layer, juce::AudioBuffer<float>& output,
                                   const juce::MidiBuffer& midi,
                                   const Sf2Engine::LayerConfig& routing, int midiStartSample)
{
    if (!layer.loaded || !routing.enabled) return;
    if (!routing.sustainEnabled)
    {
        layer.sustainDown.fill(false);
        for (auto& voice : layer.voices)
            if (voice.active && voice.waitingForSustain)
        {
            voice.waitingForSustain = false;
            voice.released = true;
            voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
        }
    }

    const auto endSample = output.getNumSamples();
    int renderedThrough = 0;
    float peak = 0.0f;
    for (const auto metadata : midi)
    {
        const auto eventSample = metadata.samplePosition - midiStartSample;
        if (eventSample < 0 || eventSample >= endSample) continue;
        renderVoices(layer, output, renderedThrough, eventSample, peak);
        handleMidiMessage(layer, metadata.getMessage(), routing);
        renderedThrough = eventSample;
    }
    renderVoices(layer, output, renderedThrough, endSample, peak);
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.82f),
                     std::memory_order_relaxed);
}

void NordSampleEngine::processEffectsAndMix(Layer& layer, juce::AudioBuffer<float>& scratchBuffer,
                                            juce::AudioBuffer<float>& output,
                                            const Sf2Engine::LayerConfig& routing)
{
    const auto samples = scratchBuffer.getNumSamples();
    const auto cutoff = juce::jlimit(0.0f, 100.0f, routing.cutoff);
    if (cutoff < 99.5f)
    {
        const auto frequency = 80.0f * std::pow(250.0f, cutoff / 100.0f);
        const auto coefficient = std::exp(-juce::MathConstants<float>::twoPi * frequency
                                          / (float) currentSampleRate);
        for (int channel = 0; channel < 2; ++channel)
        {
            auto state = layer.cutoffState[(size_t) channel];
            auto* values = scratchBuffer.getWritePointer(channel);
            for (int i = 0; i < samples; ++i)
            {
                state = (1.0f - coefficient) * values[i] + coefficient * state;
                values[i] = state;
            }
            layer.cutoffState[(size_t) channel] = state;
        }
    }
    layer.eq.setParameters(routing.eqLow, routing.eqMid, routing.eqHigh, currentSampleRate,
                           routing.eqLowFrequency, routing.eqMidFrequency, routing.eqHighFrequency,
                           routing.eqLowQ, routing.eqMidQ, routing.eqHighQ);
    for (int channel = 0; channel < 2; ++channel)
    {
        auto* values = scratchBuffer.getWritePointer(channel);
        for (int i = 0; i < samples; ++i) values[i] = layer.eq.process(values[i], channel);
    }
    layer.nordFx.process(scratchBuffer, routing.nordFx);
    const std::array<float, 4> reverbValues { routing.reverb, routing.reverbSize,
                                              routing.reverbDamping, routing.reverbWidth };
    if (reverbValues != layer.lastReverbParameters)
    {
        juce::Reverb::Parameters parameters;
        parameters.roomSize = juce::jlimit(0.0f, 1.0f, routing.reverbSize / 100.0f);
        parameters.damping = juce::jlimit(0.0f, 1.0f, routing.reverbDamping / 100.0f);
        parameters.width = juce::jlimit(0.0f, 1.0f, routing.reverbWidth / 100.0f);
        parameters.wetLevel = juce::jlimit(0.0f, 1.0f, routing.reverb / 100.0f) * 0.72f;
        parameters.dryLevel = 1.0f;
        parameters.freezeMode = 0.0f;
        layer.reverb.setParameters(parameters);
        layer.lastReverbParameters = reverbValues;
    }
    if (routing.reverb > 0.001f)
        layer.reverb.processStereo(scratchBuffer.getWritePointer(0), scratchBuffer.getWritePointer(1), samples);

    const auto compMix = juce::jlimit(0.0f, 100.0f, routing.compressor) / 100.0f;
    if (compMix > 0.001f)
    {
        const auto threshold = juce::jlimit(-60.0f, 0.0f, routing.compressorThreshold);
        const auto ratio = juce::jmax(1.0f, routing.compressorRatio);
        const auto attack = std::exp(-1.0f / (juce::jmax(0.1f, routing.compressorAttack) * 0.001f * (float) currentSampleRate));
        const auto release = std::exp(-1.0f / (juce::jmax(1.0f, routing.compressorRelease) * 0.001f * (float) currentSampleRate));
        const auto makeup = juce::Decibels::decibelsToGain(juce::jlimit(0.0f, 24.0f, routing.compressorMakeup));
        for (int channel = 0; channel < 2; ++channel)
        {
            auto envelope = layer.compressorEnvelope[(size_t) channel];
            auto* values = scratchBuffer.getWritePointer(channel);
            for (int i = 0; i < samples; ++i)
            {
                const auto dry = values[i];
                const auto magnitude = juce::jmax(1.0e-8f, std::abs(dry));
                const auto coefficient = magnitude > envelope ? attack : release;
                envelope = coefficient * envelope + (1.0f - coefficient) * magnitude;
                const auto over = juce::jmax(0.0f, juce::Decibels::gainToDecibels(envelope, -120.0f) - threshold);
                const auto wet = dry * juce::Decibels::decibelsToGain(-over * (1.0f - 1.0f / ratio)) * makeup;
                values[i] = dry + (wet - dry) * compMix;
            }
            layer.compressorEnvelope[(size_t) channel] = envelope;
        }
    }

    layer.gain.setTargetValue(juce::jmax(0.0f, routing.gain));
    const auto panLeft = routing.pan <= 0.0f ? 1.0f : 1.0f - routing.pan;
    const auto panRight = routing.pan >= 0.0f ? 1.0f : 1.0f + routing.pan;
    float peak = 0.0f;
    for (int i = 0; i < samples; ++i)
    {
        const auto gain = layer.gain.getNextValue();
        const auto left = scratchBuffer.getSample(0, i) * gain * panLeft;
        const auto right = scratchBuffer.getSample(1, i) * gain * panRight;
        output.addSample(0, i, left);
        if (output.getNumChannels() > 1) output.addSample(1, i, right);
        peak = juce::jmax(peak, std::abs(left), std::abs(right));
    }
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.82f),
                     std::memory_order_relaxed);
}

void NordSampleEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                               const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                               const std::array<Sf2Engine::LayerConfig, layerCount>& routing)
{
    const juce::ScopedTryLock stateLock(lock);
    if (!stateLock.isLocked() || output.getNumChannels() <= 0) return;
    for (int i = 0; i < layerCount; ++i)
    {
        auto& layer = layers[(size_t) i];
        if (!layer.loaded || !routing[(size_t) i].enabled) continue;
        auto& events = combinedMidi[(size_t) i];
        events.clear();
        events.addEvents(midi, 0, -1, 0);
        events.addEvents(routedMidi[(size_t) i], 0, -1, 0);
        auto& storage = scratch[(size_t) i];
        const auto chunkLimit = juce::jmax(1, maximumBlockSize);
        for (int start = 0; start < output.getNumSamples(); start += chunkLimit)
        {
            const auto count = juce::jmin(chunkLimit, output.getNumSamples() - start);
            float* channels[] { storage.getWritePointer(0), storage.getWritePointer(1) };
            juce::AudioBuffer<float> layerScratch(channels, 2, count);
            layerScratch.clear();
            renderLayer(layer, layerScratch, events, routing[(size_t) i], start);
            auto* outLeft = output.getWritePointer(0, start);
            auto* outRight = output.getNumChannels() > 1 ? output.getWritePointer(1, start) : nullptr;
            float* outputChannels[] { outLeft, outRight };
            juce::AudioBuffer<float> outputChunk(outputChannels,
                                                  juce::jmin(2, output.getNumChannels()), count);
            processEffectsAndMix(layer, layerScratch, outputChunk, routing[(size_t) i]);
        }
    }
}
