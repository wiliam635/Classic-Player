#include "NordSampleEngine.h"
#include "NordLegacySampleDecoder.h"
#include "FastLoadCache.h"
#include <algorithm>
#include <cmath>
#include <string>

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
        layer.arpeggiator.prepare(currentSampleRate);
        layer.reverb.setSampleRate(currentSampleRate);
        layer.reverb.reset();
        layer.effectTailPending = false;
        layer.effectQuietSamples = 0;
        layer.lastReverbParameters.fill(-1.0f);
        layer.eq.reset();
        layer.nordFx.prepare(currentSampleRate);
        layer.gain.reset(currentSampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(0.8f);
        layer.arpeggiatorGate.reset(currentSampleRate, 0.003);
        layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
    }
}

void NordSampleEngine::prepareLoopCrossfades(NordSampleDecoder::Instrument& instrument) noexcept
{
    for (auto& zone : instrument.zones)
    {
        const auto start = zone.loopStart;
        const auto end = zone.loopEnd;
        if (end <= start + 32 || end > zone.frames()) continue;
        // Splice the end onto the beginning without discarding any frames. The
        // former overlap shortened short (single-cycle) loops by 25%, changing
        // their pitch as soon as a held note reached the loop.
        const auto fade = juce::jmin(static_cast<size_t>(256), (end - start) / 4);
        if (fade < 8) continue;
        for (int channel = 0; channel < zone.channels; ++channel)
        {
            const auto index = [&](size_t frame) { return frame * (size_t) zone.channels + (size_t) channel; };
            const auto first = zone.pcm[index(start)];
            const auto second = zone.pcm[index(start + 1)];
            const auto last = zone.pcm[index(end - 1)];
            // Meet the first sample with its incoming slope. A bounded
            // raised-cosine correction avoids the overshoot a Hermite slope
            // fit can create on hot brass and saxophone samples.
            const auto valueDelta = (2.0f * first - second) - last;
            for (size_t i = 0; i < fade; ++i)
            {
                const auto t = (float) i / (float) (fade - 1);
                const auto blend = t * t * (3.0f - 2.0f * t);
                zone.pcm[index(end - fade + i)] += valueDelta * blend;
            }
        }
    }
}

juce::Result NordSampleEngine::loadSample(int layerIndex, const juce::File& file,
    std::shared_ptr<NordSampleDecoder::Instrument> prepared)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount) || (prepared == nullptr && !file.existsAsFile())
        || !file.hasFileExtension(".nsmp;.nsmp3;.nsmp4"))
        return juce::Result::fail("Selecione uma amostra Nord (.nsmp, .nsmp3 ou .nsmp4).");

    auto decoded = std::move(prepared);
    if (decoded == nullptr)
        if (const auto result = prepareSample(file, decoded); result.failed()) return result;

    const juce::ScopedLock stateLock(lock);
    auto& layer = layers[(size_t) layerIndex];
    layer.instrument = std::move(decoded);
    layer.sourcePath = file.getFullPathName();
    layer.loaded = true;
    layer.arpeggiator.reset();
    layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
    layer.sustainDown.fill(false);
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.heldSplitGains.fill(0.0f);
    layer.heldChannels.fill(0);
    layer.heldNoteOrder.fill(0);
    layer.heldNoteSequence = 0;
    layer.monoNote = -1;
    layer.monoChannel = 1;
    layer.lastMonoMode = false;
    layer.cutoffState.fill(0.0f);
    layer.compressorEnvelope.fill(0.0f);
    layer.eq.reset();
    layer.nordFx.reset();
    layer.reverb.reset();
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    layer.lastReverbParameters.fill(-1.0f);
    for (auto& voice : layer.voices) voice = {};
    layer.peak.store(0.0f, std::memory_order_release);
    return juce::Result::ok();
}

juce::Result NordSampleEngine::prepareSample(const juce::File& file,
    std::shared_ptr<NordSampleDecoder::Instrument>& decoded, std::uint64_t maxDecodedBytes)
{
    if (!file.existsAsFile() || !file.hasFileExtension(".nsmp;.nsmp3;.nsmp4"))
        return juce::Result::fail("Selecione uma amostra Nord valida.");
    decoded = findCachedInstrument(file);
    if (decoded != nullptr)
    {
        std::uint64_t bytes = 0;
        for (const auto& zone : decoded->zones)
            bytes += zone.pcm.capacity() * sizeof(float);
        return bytes <= maxDecodedBytes ? juce::Result::ok()
            : juce::Result::fail("Este timbre Nord excede a memória restante para carregar o preset.");
    }
    decoded = std::make_shared<NordSampleDecoder::Instrument>();
    // Fast path: a previous import already stored the decoded and prepared instrument.
    if (FastLoadCache::loadSample(file, *decoded, maxDecodedBytes))
    {
        decoded = cacheInstrument(file, std::move(decoded));
        return juce::Result::ok();
    }
    const auto result = file.hasFileExtension(".nsmp4")
        ? NordSampleDecoder::decode(file, *decoded, maxDecodedBytes)
        : NordLegacySampleDecoder::decodeFile(file, *decoded, maxDecodedBytes);
    if (result.failed()) { decoded.reset(); return result; }
    prepareLoopCrossfades(*decoded);
    decoded = cacheInstrument(file, std::move(decoded));
    FastLoadCache::storeSample(file, decoded);
    return juce::Result::ok();
}

juce::Result NordSampleEngine::preloadSample(const juce::File& file)
{
    std::shared_ptr<NordSampleDecoder::Instrument> decoded;
    return prepareSample(file, decoded);
}

std::shared_ptr<NordSampleDecoder::Instrument> NordSampleEngine::findCachedInstrument(
    const juce::File& file)
{
    const auto key = file.getFullPathName() + "|" + juce::String(file.getSize()) + "|"
                   + juce::String(file.getLastModificationTime().toMilliseconds());
    const std::lock_guard<std::mutex> guard(instrumentCacheMutex);
    const auto found = instrumentCache.find(key.toStdString());
    if (found == instrumentCache.end())
    {
        const auto live = instrumentCacheLive.find(key.toStdString());
        return live != instrumentCacheLive.end() ? live->second.lock() : std::shared_ptr<NordSampleDecoder::Instrument>{};
    }
    found->second.lastUse = ++instrumentCacheClock;
    return found->second.instrument;
}

std::shared_ptr<NordSampleDecoder::Instrument> NordSampleEngine::cacheInstrument(
    const juce::File& file, std::shared_ptr<NordSampleDecoder::Instrument> decoded)
{
    if (decoded == nullptr) return {};
    const auto key = file.getFullPathName() + "|" + juce::String(file.getSize()) + "|"
                   + juce::String(file.getLastModificationTime().toMilliseconds());
    size_t bytes = sizeof(NordSampleDecoder::Instrument);
    for (const auto& zone : decoded->zones)
        bytes += sizeof(NordSampleDecoder::Zone) + zone.pcm.size() * sizeof(float);
    const std::lock_guard<std::mutex> guard(instrumentCacheMutex);
    const auto keyString = key.toStdString();
    if (const auto existing = instrumentCacheLive.find(keyString); existing != instrumentCacheLive.end())
        if (auto shared = existing->second.lock()) return shared;
    for (auto entry = instrumentCacheLive.begin(); entry != instrumentCacheLive.end();)
        if (entry->second.expired()) entry = instrumentCacheLive.erase(entry); else ++entry;
    instrumentCacheLive[keyString] = decoded;
    if (bytes > decodedCacheLimitBytes) return decoded;
    if (const auto found = instrumentCache.find(keyString); found != instrumentCache.end())
    {
        found->second.lastUse = ++instrumentCacheClock;
        return found->second.instrument;
    }
    while (instrumentCacheBytes + bytes > decodedCacheLimitBytes && !instrumentCache.empty())
    {
        const auto oldest = std::min_element(instrumentCache.begin(), instrumentCache.end(),
            [](const auto& left, const auto& right)
            { return left.second.lastUse < right.second.lastUse; });
        instrumentCacheBytes -= oldest->second.bytes;
        instrumentCache.erase(oldest);
    }
    instrumentCacheBytes += bytes;
    instrumentCache.emplace(keyString, CachedInstrument { decoded, bytes, ++instrumentCacheClock });
    return decoded;
}

void NordSampleEngine::unload(int layerIndex)
{
    if (!juce::isPositiveAndBelow(layerIndex, layerCount)) return;
    const juce::ScopedLock stateLock(lock);
    auto& layer = layers[(size_t) layerIndex];
    layer.instrument.reset();
    layer.sourcePath.clear();
    layer.loaded = false;
    layer.arpeggiator.reset();
    layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
    layer.sustainDown.fill(false);
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.heldSplitGains.fill(0.0f);
    layer.heldChannels.fill(0);
    layer.heldNoteOrder.fill(0);
    layer.heldNoteSequence = 0;
    layer.monoNote = -1;
    layer.monoChannel = 1;
    layer.lastMonoMode = false;
    for (auto& voice : layer.voices) voice = {};
    layer.eq.reset();
    layer.nordFx.reset();
    layer.reverb.reset();
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    layer.peak.store(0.0f, std::memory_order_release);
}

void NordSampleEngine::stopAllSounds()
{
    const juce::ScopedLock stateLock(lock);
    for (auto& layer : layers)
    {
        layer.arpeggiator.reset();
        layer.arpeggiatorGate.setCurrentAndTargetValue(1.0f);
        layer.sustainDown.fill(false);
        layer.heldNotes.fill(false);
        layer.heldVelocities.fill(0.0f);
        layer.heldSplitGains.fill(0.0f);
        layer.heldChannels.fill(0);
        layer.heldNoteOrder.fill(0);
        layer.heldNoteSequence = 0;
        layer.monoNote = -1;
        layer.monoChannel = 1;
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
    return stateLock.isLocked() && layers[(size_t) layerIndex].instrument != nullptr
        ? layers[(size_t) layerIndex].instrument->name : juce::String{};
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

void NordSampleEngine::readStereoSample(const NordSampleDecoder::Zone& zone,
                                        double position, int frames, float& left, float& right) noexcept
{
    if (frames <= 0 || zone.channels <= 0)
    {
        left = right = 0.0f;
        return;
    }
    const auto start = (int) position;
    const auto fraction = (float) (position - start);
    const auto loopStart = (int) zone.loopStart;
    const auto loopEnd = (int) zone.loopEnd;
    const auto looped = loopEnd > loopStart + 8;

    // Most playback frames lie well inside the sample or loop. In that case
    // all four cubic taps are contiguous, so skip per-tap modulo and clamps.
    const auto tapsAreContiguous = start >= 1 && start + 2 < frames
        && (!looped
            || (position >= loopStart
                ? start - 1 >= loopStart && start + 2 < loopEnd
                : start + 2 < loopStart));
    const auto sourceRight = juce::jmin(1, zone.channels - 1);
    const auto* pcm = zone.pcm.data();
    const auto sampleAt = [&](int frame, int channel) noexcept
    {
        if (looped && frame < loopStart && position >= (double) loopStart)
            frame = loopEnd - (loopStart - frame);
        if (looped && frame >= loopEnd)
            frame = loopStart + (frame - loopEnd) % (loopEnd - loopStart);
        frame = juce::jlimit(0, frames - 1, frame);
        return pcm[(size_t) frame * (size_t) zone.channels + (size_t) channel];
    };
    const auto t2 = fraction * fraction, t3 = t2 * fraction;
    const auto interpolate = [&](int channel) noexcept
    {
        float y0, y1, y2, y3;
        if (tapsAreContiguous)
        {
            const auto* tap = pcm + (size_t) (start - 1) * (size_t) zone.channels + (size_t) channel;
            y0 = tap[0];
            y1 = tap[zone.channels];
            y2 = tap[2 * zone.channels];
            y3 = tap[3 * zone.channels];
        }
        else
        {
            y0 = sampleAt(start - 1, channel);
            y1 = sampleAt(start, channel);
            y2 = sampleAt(start + 1, channel);
            y3 = sampleAt(start + 2, channel);
        }
        return 0.5f * ((2.0f * y1) + (-y0 + y2) * fraction
            + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
            + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
    };
    left = interpolate(0);
    right = sourceRight == 0 ? left : interpolate(sourceRight);
}

float NordSampleEngine::releaseCoefficientFor(float control, double sampleRate) noexcept
{
    const auto seconds = layerReleaseSeconds(control);
    return (float) std::exp(std::log(envelopeFloor)
        / juce::jmax(1.0, seconds * sampleRate));
}

void NordSampleEngine::handleMidiMessage(Layer& layer, const juce::MidiMessage& message,
                                         const Sf2Engine::LayerConfig& routing,
                                         bool generated, int sourceNote, int patternPan) noexcept
{
    if (routing.midiChannel > 0 && message.getChannel() != routing.midiChannel) return;
    const auto channel = juce::jlimit(1, 16, message.getChannel());
    const bool monoMode = routing.mono
        && !(routing.nordArp.enabled && routing.nordArp.mode == 1);
    const auto heldIndex = [] (int midiChannel, int note)
    {
        return (size_t) (midiChannel - 1) * 128u + (size_t) note;
    };
    const auto highestHeld = [&layer]()
    {
        size_t best = layer.heldNotes.size();
        uint64_t newest = 0;
        for (size_t i = 0; i < layer.heldNotes.size(); ++i)
            if (layer.heldNotes[i] && layer.heldNoteOrder[i] >= newest)
            {
                best = i;
                newest = layer.heldNoteOrder[i];
            }
        return best;
    };
    const auto emitArp = [&](const NordArpeggiator::Event& event)
    {
        if (event.gateEvent)
        {
            layer.arpeggiatorGate.setTargetValue(event.gateOpen ? 1.0f : 0.0f);
            return;
        }
        const auto generatedMessage = event.noteOn
            ? juce::MidiMessage::noteOn(event.channel, event.note, (juce::uint8) event.velocity)
            : juce::MidiMessage::noteOff(event.channel, event.note);
        handleMidiMessage(layer, generatedMessage, routing, true, event.sourceNote, event.pan);
    };
    const auto startVoice = [&](int note, int midiChannel, float shapedVelocity, bool legato,
                                float splitGain, int pan) -> bool
    {
        const auto velocity = juce::jlimit(1, 127, juce::roundToInt(shapedVelocity * 127.0f));
        const auto zoneIndex = findZone(*layer.instrument, note, velocity);
        if (!juce::isPositiveAndBelow(zoneIndex, (int) layer.instrument->zones.size())) return false;

        const auto& zone = layer.instrument->zones[(size_t) zoneIndex];
        if (monoMode)
            for (auto& prior : layer.voices)
                if (prior.active && (prior.keyDown || prior.waitingForSustain))
                {
                    prior.keyDown = false;
                    prior.waitingForSustain = false;
                    prior.released = true;
                    prior.releaseCoefficient = releaseCoefficientFor(0.5f, currentSampleRate);
                }

        auto& voice = allocateVoice(layer, routing.polyphony);
        const auto cents = layer.instrument->detune[(size_t) note];
        voice = {};
        voice.active = zone.frames() > 0;
        voice.note = note;
        voice.channel = juce::jlimit(1, 16, midiChannel);
        voice.patternPan = juce::jlimit(-1, 1, pan);
        voice.zone = zoneIndex;
        voice.keyDown = true;
        voice.position = legato && zone.secondStart > 0 && zone.secondStart < zone.frames()
            ? static_cast<double>(zone.secondStart) : 0.0;
        voice.increment = std::pow(2.0, (note - zone.root + cents / 100.0) / 12.0)
                        * NordSampleDecoder::sampleRate / currentSampleRate;
        const auto noteGain = layer.instrument->gain[(size_t) note];
        voice.amplitude = shapedVelocity * zone.gain * juce::jmax(0.0f, noteGain)
            * splitGain;
        voice.attack = routing.attack <= 0.0f ? 1.0f : 0.0f;
        voice.attackStep = routing.attack <= 0.0f ? 1.0f
            : (float) (1.0 / juce::jmax(1.0, layerAttackSeconds(routing.attack) * currentSampleRate));
        voice.releaseGain = 1.0f;
        if (monoMode)
        {
            layer.monoNote = note;
            layer.monoChannel = voice.channel;
        }
        return true;
    };

    if (message.isAllSoundOff() || message.isAllNotesOff())
    {
        if (!generated && routing.nordArp.enabled)
            layer.arpeggiator.controller(channel,
                message.isAllSoundOff() ? 120 : 123, 0, emitArp);
        const auto channelIndex = (size_t) (channel - 1);
        layer.sustainDown[channelIndex] = false;
        const auto first = channelIndex * 128u;
        std::fill(layer.heldNotes.begin() + first, layer.heldNotes.begin() + first + 128, false);
        std::fill(layer.heldVelocities.begin() + first, layer.heldVelocities.begin() + first + 128, 0.0f);
        std::fill(layer.heldSplitGains.begin() + first, layer.heldSplitGains.begin() + first + 128, 0.0f);
        std::fill(layer.heldChannels.begin() + first, layer.heldChannels.begin() + first + 128, 0);
        std::fill(layer.heldNoteOrder.begin() + first, layer.heldNoteOrder.begin() + first + 128, 0);
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
        if (monoMode && layer.monoChannel == channel)
        {
            const auto fallback = highestHeld();
            if (fallback < layer.heldNotes.size())
                startVoice((int) (fallback % 128u), layer.heldChannels[fallback],
                           layer.heldVelocities[fallback], true,
                           layer.heldSplitGains[fallback], 0);
            else
                layer.monoNote = -1;
        }
        return;
    }
    if (message.isController() && message.getControllerNumber() == 64)
    {
        if (!generated && routing.nordArp.enabled)
            layer.arpeggiator.controller(channel, 64,
                routing.sustainEnabled ? message.getControllerValue() : 0, emitArp);
        const auto wasDown = layer.sustainDown[(size_t) channel - 1];
        const auto isDown = routing.sustainEnabled && message.getControllerValue() >= 64;
        layer.sustainDown[(size_t) channel - 1] = isDown;
        if (wasDown && !isDown && monoMode && layer.monoNote >= 0
            && layer.monoChannel == channel)
        {
            const auto fallback = highestHeld();
            if (fallback < layer.heldNotes.size())
            {
                const auto fallbackNote = (int) (fallback % 128u);
                const auto fallbackChannel = layer.heldChannels[fallback];
                if (fallbackNote != layer.monoNote || fallbackChannel != layer.monoChannel)
                    startVoice(fallbackNote, fallbackChannel, layer.heldVelocities[fallback], true,
                               layer.heldSplitGains[fallback], 0);
            }
            else
            {
                for (auto& voice : layer.voices)
                    if (voice.active && voice.waitingForSustain)
                    {
                        voice.waitingForSustain = false;
                        voice.released = true;
                        voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                    }
                layer.monoNote = -1;
            }
            return;
        }
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
        if (!generated && !routing.acceptsVelocity((int) message.getVelocity())) return;
        const auto inputNote = message.getNoteNumber();
        if (!generated && (inputNote < routing.lowNote || inputNote > routing.highNote)) return;
        const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
        auto velocity = generated ? juce::jlimit(1, 127, (int) message.getVelocity())
            : routing.velocitySensitivityOff ? 100
            : juce::jlimit(1, 127, (int) message.getVelocity());
        if (!generated && !routing.velocitySensitivityOff && routing.velocityCurve == 1)
            velocity = juce::jlimit(1, 127, juce::roundToInt(std::sqrt(velocity / 127.0f) * 127.0f));
        else if (!generated && !routing.velocitySensitivityOff && routing.velocityCurve == 2)
            velocity = juce::jlimit(1, 127, juce::roundToInt((velocity / 127.0f) * (velocity / 127.0f) * 127.0f));
        else if (!generated && !routing.velocitySensitivityOff && routing.velocityCurve == 4)
            velocity = juce::jlimit(1, 127, (int) routing.velocityTable[(size_t) message.getVelocity()]);
        const auto shapedVelocity = velocity / 127.0f;
        if (!generated && routing.nordArp.enabled)
        {
            layer.arpeggiator.input(channel, inputNote, velocity, true, emitArp,
                                    routing.nordArp.keyboardSync);
            if (routing.nordArp.mode != 2) return;
        }
        const auto previousNote = highestHeld();
        const auto legato = monoMode && previousNote < layer.heldNotes.size()
            && (int) (previousNote % 128u) != note;
        const auto splitSource = generated && sourceNote >= 0 ? sourceNote : inputNote;
        if (!startVoice(note, channel, shapedVelocity, legato,
                        routing.nordSplit.gain(splitSource), generated ? patternPan : 0)) return;
        if (monoMode && !generated)
        {
            const auto index = heldIndex(channel, note);
            layer.heldNotes[index] = true;
            layer.heldVelocities[index] = shapedVelocity;
            layer.heldSplitGains[index] = routing.nordSplit.gain(splitSource);
            layer.heldChannels[index] = channel;
            layer.heldNoteOrder[index] = ++layer.heldNoteSequence;
        }
        return;
    }
    if (message.isNoteOff())
    {
        const auto inputNote = message.getNoteNumber();
        if (!generated && routing.nordArp.enabled)
        {
            layer.arpeggiator.input(channel, inputNote, 0, false, emitArp);
            if (routing.nordArp.mode != 2) return;
        }
        if (!generated && (inputNote < routing.lowNote || inputNote > routing.highNote)) return;
        const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
        if (monoMode && generated)
        {
            if (layer.monoNote == note && layer.monoChannel == channel) layer.monoNote = -1;
            for (auto& voice : layer.voices)
                if (voice.active && voice.note == note && voice.channel == channel)
                {
                    voice.keyDown = false;
                    if (layer.sustainDown[(size_t) channel - 1] && routing.sustainEnabled)
                        voice.waitingForSustain = true;
                    else
                    {
                        voice.released = true;
                        voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                    }
                }
            return;
        }
        if (monoMode)
        {
            const auto index = heldIndex(channel, note);
            if (!layer.heldNotes[index]) return;
            layer.heldNotes[index] = false;
            if (note != layer.monoNote || channel != layer.monoChannel) return;
            if (layer.sustainDown[(size_t) channel - 1])
            {
                for (auto& voice : layer.voices)
                    if (voice.active && voice.note == note && voice.channel == layer.monoChannel)
                    {
                        voice.keyDown = false;
                        voice.waitingForSustain = true;
                    }
                return;
            }
            const auto fallback = highestHeld();
            if (fallback < layer.heldNotes.size())
            {
                startVoice((int) (fallback % 128u), layer.heldChannels[fallback],
                           layer.heldVelocities[fallback], true,
                           layer.heldSplitGains[fallback], 0);
                return;
            }
            for (auto& voice : layer.voices)
                if (voice.active && voice.keyDown && voice.note == note && voice.channel == layer.monoChannel)
                {
                    voice.keyDown = false;
                    voice.released = true;
                    voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
                }
            layer.monoNote = -1;
            return;
        }

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
                                    int startSample, int endSample, float& peak,
                                    const Sf2Engine::LayerConfig& routing) noexcept
{
    auto* outputLeft = output.getWritePointer(0);
    auto* outputRight = output.getNumChannels() > 1 ? output.getWritePointer(1) : nullptr;
    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        if (!juce::isPositiveAndBelow(voice.zone, (int) layer.instrument->zones.size()))
        {
            voice = {};
            continue;
        }
        const auto& zone = layer.instrument->zones[(size_t) voice.zone];
        const auto frames = (int) zone.frames();
        const auto looped = zone.loopEnd > zone.loopStart + 8;
        const auto adsrTarget = juce::jlimit(0.0f, 1.0f, routing.sustainLevel * 0.01f);
        const auto adsrStep = static_cast<float>((1.0f - adsrTarget)
            / juce::jmax(1.0, routing.decayMs * 0.001 * currentSampleRate));
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
            const auto amplitude = voice.amplitude * voice.attack * voice.decayGain * voice.releaseGain;
            const auto leftPan = voice.patternPan > 0 ? 0.0f : 1.0f;
            const auto rightPan = voice.patternPan < 0 ? 0.0f : 1.0f;
            float left, right;
            readStereoSample(zone, voice.position, frames, left, right);
            left *= amplitude * leftPan;
            right *= amplitude * rightPan;
            outputLeft[sample] += left;
            if (outputRight != nullptr) outputRight[sample] += right;
            peak = juce::jmax(peak, std::abs(left), std::abs(right));
            voice.attack = juce::jmin(1.0f, voice.attack + voice.attackStep);
            if (!voice.released && voice.attack >= 1.0f)
            {
                voice.decayGain = juce::jmax(adsrTarget, voice.decayGain - adsrStep);
            }
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
    const bool monoMode = routing.mono
        && !(routing.nordArp.enabled && routing.nordArp.mode == 1);
    if (layer.lastMonoMode != monoMode)
    {
        layer.lastMonoMode = monoMode;
        layer.heldNotes.fill(false);
        layer.heldVelocities.fill(0.0f);
        layer.heldSplitGains.fill(0.0f);
        layer.heldChannels.fill(0);
        layer.heldNoteOrder.fill(0);
        layer.heldNoteSequence = 0;
        layer.sustainDown.fill(false);
        layer.monoNote = -1;
        layer.monoChannel = 1;
        for (auto& voice : layer.voices)
            if (voice.active && (voice.keyDown || voice.waitingForSustain))
            {
                voice.keyDown = false;
                voice.waitingForSustain = false;
                voice.released = true;
                voice.releaseCoefficient = releaseCoefficientFor(routing.release, currentSampleRate);
            }
    }
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
    float peak = 0.0f;
    int renderedThrough = 0;
    auto renderSpan = [&](int spanEnd)
    {
        if (spanEnd <= renderedThrough) return;
        if (routing.nordArp.enabled || layer.arpeggiator.busy())
        {
            const auto spanStart = renderedThrough;
            const auto spanLength = spanEnd - spanStart;
            const auto emitArp = [&](int offset, const NordArpeggiator::Event& arpEvent)
            {
                const auto sample = spanStart + offset;
                renderVoices(layer, output, renderedThrough, sample, peak, routing);
                if (routing.nordArp.mode == 2 || layer.arpeggiatorGate.isSmoothing())
                {
                    for (int i = renderedThrough; i < sample; ++i)
                    {
                        const auto gate = layer.arpeggiatorGate.getNextValue();
                        output.setSample(0, i, output.getSample(0, i) * gate);
                        if (output.getNumChannels() > 1)
                            output.setSample(1, i, output.getSample(1, i) * gate);
                    }
                }
                renderedThrough = sample;
                if (arpEvent.gateEvent)
                {
                    layer.arpeggiatorGate.setTargetValue(arpEvent.gateOpen ? 1.0f : 0.0f);
                    return;
                }
                const auto message = arpEvent.noteOn
                    ? juce::MidiMessage::noteOn(arpEvent.channel, arpEvent.note,
                                                (juce::uint8) arpEvent.velocity)
                    : juce::MidiMessage::noteOff(arpEvent.channel, arpEvent.note);
                handleMidiMessage(layer, message, routing, true, arpEvent.sourceNote, arpEvent.pan);
            };
            layer.arpeggiator.advanceFrames(spanLength, routing.nordArp, emitArp);
        }
        renderVoices(layer, output, renderedThrough, spanEnd, peak, routing);
        if (routing.nordArp.mode == 2 || layer.arpeggiatorGate.isSmoothing())
            for (int i = renderedThrough; i < spanEnd; ++i)
            {
                const auto gate = layer.arpeggiatorGate.getNextValue();
                output.setSample(0, i, output.getSample(0, i) * gate);
                if (output.getNumChannels() > 1)
                    output.setSample(1, i, output.getSample(1, i) * gate);
            }
        renderedThrough = spanEnd;
    };

    for (const auto metadata : midi)
    {
        const auto eventSample = metadata.samplePosition - midiStartSample;
        if (eventSample < 0 || eventSample >= endSample) continue;
        renderSpan(eventSample);
        handleMidiMessage(layer, metadata.getMessage(), routing);
    }
    renderSpan(endSample);
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
    if (layer.eq.isActive())
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            auto* values = scratchBuffer.getWritePointer(channel);
            for (int i = 0; i < samples; ++i)
                values[i] = layer.eq.process(values[i], channel);
        }
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
        setLayerReverbMix(parameters, routing.reverb / 100.0f);
        parameters.freezeMode = 0.0f;
        layer.reverb.setParameters(parameters);
        layer.lastReverbParameters = reverbValues;
    }
    if (layer.reverb.shouldProcess())
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
    float effectTailPeak = 0.0f;
    for (int i = 0; i < samples; ++i)
    {
        effectTailPeak = juce::jmax(effectTailPeak, std::abs(scratchBuffer.getSample(0, i)),
                                    std::abs(scratchBuffer.getSample(1, i)));
        const auto gain = layer.gain.getNextValue();
        const auto left = scratchBuffer.getSample(0, i) * gain * panLeft;
        const auto right = scratchBuffer.getSample(1, i) * gain * panRight;
        output.addSample(0, i, left);
        if (output.getNumChannels() > 1) output.addSample(1, i, right);
        peak = juce::jmax(peak, std::abs(left), std::abs(right));
    }
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.82f),
                     std::memory_order_relaxed);
    const auto hasVoices = std::any_of(layer.voices.begin(), layer.voices.end(),
        [](const auto& voice) { return voice.active; });
    updateLayerEffectTail(layer.effectTailPending, layer.effectQuietSamples,
                          hasVoices, effectTailPeak, samples, currentSampleRate);
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
        const auto& localMidi = routedMidi[(size_t) i];
        const auto hasVoices = std::any_of(layer.voices.begin(), layer.voices.end(),
                                           [](const auto& voice) { return voice.active; });
        if (!hasVoices && midi.isEmpty() && localMidi.isEmpty() && !layer.effectTailPending
            && !layer.arpeggiator.busy())
        {
            layer.peak.store(0.0f, std::memory_order_relaxed);
            continue;
        }
        auto& events = combinedMidi[(size_t) i];
        events.clear();
        events.addEvents(midi, 0, -1, 0);
        events.addEvents(localMidi, 0, -1, 0);
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
