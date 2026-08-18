#include "Dx7Engine.h"
#include <cmath>
#include <cstdint>
#include <utility>

void Dx7Engine::prepare(double newSampleRate, int)
{
    const juce::ScopedLock guard(lock);
    sampleRate = juce::jmax(1.0, newSampleRate);
}

juce::String Dx7Engine::decodeName(const uint8_t* data, int size)
{
    juce::String name;
    for (int i = 0; i < size; ++i)
    {
        const auto c = data[i] & 0x7f;
        if (c >= 32 && c <= 126)
            name << juce::String::charToString((juce::juce_wchar) c);
    }
    return name.trim();
}

Dx7Engine::Patch Dx7Engine::parsePackedPatch(const uint8_t* voice)
{
    Patch patch;
    patch.name = decodeName(voice + 118, 10);
    patch.algorithm = voice[110] & 0x1f;
    patch.feedback = (voice[111] >> 3) & 0x07;
    for (int op = 0; op < 6; ++op)
    {
        // A 128-byte DX7 bulk voice stores operators 6 through 1 in 17 bytes.
        // Keep the native order: the DX7 algorithm routing table uses this same
        // order. The old renderer converted fixed oscillators to musical ratios,
        // which created unrelated high-frequency tones in many electric pianos
        // and organs.
        const auto offset = op * 17;
        // The 128-byte DX7 bulk format packs an operator in 17 bytes:
        // byte 12 = detune/rate scale, byte 13 = key velocity/amp modulation,
        // byte 14 = output level, byte 15 = mode/coarse and byte 16 = fine.
        // Do not treat the packed control bytes as level or detune values.
        const auto modeAndCoarse = voice[offset + 15];
        const auto coarse = (modeAndCoarse >> 1) & 0x1f;
        const auto fine = juce::jlimit(0, 99, (int) voice[offset + 16]);
        const auto fixed = (modeAndCoarse & 0x01) != 0;
        patch.levels[(size_t) op] = juce::jlimit(0.0f, 1.0f,
            (float) juce::jlimit(0, 99, (int) voice[offset + 14]) / 99.0f);
        patch.detunes[(size_t) op] = juce::jlimit(0, 14, (int) ((voice[offset + 12] >> 3) & 0x0f));
        const auto ratioBase = coarse == 0 ? 0.5f : (float) coarse;
        patch.ratios[(size_t) op] = ratioBase * (1.0f + (float) fine / 100.0f);
        patch.fixedMode[(size_t) op] = fixed;
        // DX7 fixed mode is logarithmic in Hz: 10 ^ (coarse + fine / 100).
        // Only the low two coarse bits are meaningful in fixed mode.
        patch.fixedFrequency[(size_t) op] = fixed
            ? std::pow(10.0f, (float) (coarse & 0x03) + (float) fine / 100.0f)
            : 0.0f;
        for (int stage = 0; stage < 4; ++stage)
        {
            patch.egRates[(size_t) op][(size_t) stage]
                = (float) juce::jlimit(0, 99, (int) voice[offset + stage]) / 99.0f;
            patch.egLevels[(size_t) op][(size_t) stage]
                = (float) juce::jlimit(0, 99, (int) voice[offset + 4 + stage]) / 99.0f;
        }
    }
    if (patch.name.isEmpty()) patch.name = "DX7 Voice";
    return patch;
}

Dx7Engine::Patch Dx7Engine::parseSinglePatch(const uint8_t* voice, int size)
{
    Patch patch;
    if (size >= 155)
    {
        patch.name = decodeName(voice + 145, 10);
        patch.algorithm = voice[134] & 0x1f;
        patch.feedback = voice[135] & 0x07;
        for (int op = 0; op < 6; ++op)
        {
            const auto offset = op * 21;
            const auto coarse = voice[offset + 18] & 0x1f;
            const auto fine = juce::jlimit(0, 99, (int) voice[offset + 19]);
            const auto fixed = voice[offset + 17] != 0;
            patch.levels[(size_t) op] = juce::jlimit(0.0f, 1.0f,
                (float) juce::jlimit(0, 99, (int) voice[offset + 16]) / 99.0f);
            const auto ratioBase = coarse == 0 ? 0.5f : (float) coarse;
            patch.ratios[(size_t) op] = ratioBase * (1.0f + (float) fine / 100.0f);
            patch.fixedMode[(size_t) op] = fixed;
            patch.detunes[(size_t) op] = juce::jlimit(0, 14, (int) voice[offset + 20]);
            patch.fixedFrequency[(size_t) op] = fixed
                ? std::pow(10.0f, (float) (coarse & 0x03) + (float) fine / 100.0f)
                : 0.0f;
            for (int stage = 0; stage < 4; ++stage)
            {
                patch.egRates[(size_t) op][(size_t) stage]
                    = (float) juce::jlimit(0, 99, (int) voice[offset + stage]) / 99.0f;
                patch.egLevels[(size_t) op][(size_t) stage]
                    = (float) juce::jlimit(0, 99, (int) voice[offset + 4 + stage]) / 99.0f;
            }
        }
    }
    if (patch.name.isEmpty()) patch.name = "DX7 Single Voice";
    return patch;
}
juce::Result Dx7Engine::loadSysEx(int index, const juce::File& file)
{
    if (!juce::isPositiveAndBelow(index, layerCount))
        return juce::Result::fail("Layer DX7 invalida.");
    if (!file.existsAsFile() || file.getFileExtension().toLowerCase() != ".syx")
        return juce::Result::fail("Selecione um arquivo DX7 SysEx (.syx).");

    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() < 6)
        return juce::Result::fail("Nao foi possivel ler o arquivo DX7.");

    const auto* data = static_cast<const uint8_t*>(bytes.getData());
    const auto size = (int) bytes.getSize();
    int bulkStart = -1;
    int singleStart = -1;
    for (int start = 0; start + 6 < size; ++start)
    {
        if (data[start] != 0xf0 || data[start + 1] != 0x43) continue;
        if (data[start + 3] == 0x09 && data[start + 4] == 0x20 && data[start + 5] == 0x00
            && start + 6 + maxPatches * 128 <= size)
        {
            bulkStart = start;
            break;
        }
        if (singleStart < 0) singleStart = start;
    }
    if (bulkStart < 0 && singleStart < 0)
        return juce::Result::fail("O arquivo nao contem um SysEx Yamaha DX7 valido.");

    Layer loaded;
    loaded.sourcePath = file.getFullPathName();
    if (bulkStart >= 0)
    {
        const auto* bank = data + bulkStart + 6;
        for (int patch = 0; patch < maxPatches; ++patch)
            loaded.patches[(size_t) patch] = parsePackedPatch(bank + patch * 128);
        loaded.patchesLoaded = maxPatches;
    }
    else
    {
        const auto payloadStart = singleStart + 6;
        const auto payloadSize = juce::jmax(0, size - payloadStart - 2);
        loaded.patches[0] = parseSinglePatch(data + payloadStart, payloadSize);
        loaded.patchesLoaded = 1;
    }

    const juce::ScopedLock guard(lock);
    layers[(size_t) index] = std::move(loaded);
    return juce::Result::ok();
}

void Dx7Engine::unload(int index)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    layers[(size_t) index] = {};
}

void Dx7Engine::stopAllSounds()
{
    const juce::ScopedLock guard(lock);
    for (auto& layer : layers)
        for (auto& voice : layer.voices) voice = {};
}

bool Dx7Engine::isLoaded(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
        && layers[(size_t) index].sourcePath.isNotEmpty();
}

int Dx7Engine::patchCount(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].patchesLoaded : 0;
}

int Dx7Engine::selectedPatch(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].selectedPatch : 0;
}

bool Dx7Engine::selectPatch(int index, int patch)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return false;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    if (!juce::isPositiveAndBelow(patch, layer.patchesLoaded)) return false;
    layer.selectedPatch = patch;
    for (auto& voice : layer.voices) voice = {};
    return true;
}

juce::String Dx7Engine::patchName(int index) const
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return {};
    const auto& layer = layers[(size_t) index];
    return juce::isPositiveAndBelow(layer.selectedPatch, layer.patchesLoaded)
        ? layer.patches[(size_t) layer.selectedPatch].name : juce::String{};
}

juce::String Dx7Engine::patchName(int index, int patch) const
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return {};
    const auto& layer = layers[(size_t) index];
    return juce::isPositiveAndBelow(patch, layer.patchesLoaded)
        ? layer.patches[(size_t) patch].name : juce::String{};
}

juce::String Dx7Engine::path(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
        ? layers[(size_t) index].sourcePath : juce::String{};
}

bool Dx7Engine::accepts(const Sf2Engine::LayerConfig& config, const juce::MidiMessage& message)
{
    return config.midiChannel == 0 || message.getChannel() == config.midiChannel;
}

float Dx7Engine::shapedVelocity(const Sf2Engine::LayerConfig& config, float velocity)
{
    if (config.velocityCurve == 1) velocity = std::sqrt(velocity);
    else if (config.velocityCurve == 2) velocity *= velocity;
    return juce::jlimit(0.0f, 1.0f, velocity);
}

double Dx7Engine::noteFrequency(int note)
{
    return 440.0 * std::pow(2.0, ((double) note - 69.0) / 12.0);
}

void Dx7Engine::dispatch(Layer& layer, const Sf2Engine::LayerConfig& config,
                         const juce::MidiMessage& message)
{
    if (!accepts(config, message)) return;

    const auto releaseVoice = [] (Voice& voice)
    {
        if (voice.active) voice.releasing = true;
    };
    const auto highestHeld = [&layer] ()
    {
        for (int note = 127; note >= 0; --note)
            if (layer.heldNotes[(size_t) note]) return note;
        return -1;
    };
    const auto retargetMono = [&layer, &config] (int note, float velocity)
    {
        auto& voice = layer.voices.front();
        const auto wasActive = voice.active && !voice.releasing;
        voice.active = true;
        voice.releasing = false;
        voice.note = note;
        voice.velocity = velocity;
        voice.targetFrequency = noteFrequency(note);
        // A mono transition preserves oscillator phase and envelope. Only a
        // fresh voice receives an attack, avoiding clicks and retrigger gaps.
        if (!wasActive)
        {
            voice.currentFrequency = voice.targetFrequency;
            voice.phase.fill(0.0);
            voice.envelope = 0.0f;
        }
        else if (!config.portamento)
        {
            voice.currentFrequency = voice.targetFrequency;
        }
    };

    if (message.isAllSoundOff())
    {
        layer.heldNotes.fill(false);
        layer.sustainDown = false;
        for (auto& voice : layer.voices) voice = {};
        return;
    }
    if (message.isAllNotesOff())
    {
        layer.heldNotes.fill(false);
        for (auto& voice : layer.voices) releaseVoice(voice);
        return;
    }

    if (message.isController() && message.getControllerNumber() == 64)
    {
        if (!config.sustainEnabled) return;
        layer.sustainDown = message.getControllerValue() >= 64;
        if (!layer.sustainDown)
        {
            if (config.mono || config.portamento)
            {
                const auto fallback = highestHeld();
                if (fallback >= 0)
                    retargetMono(fallback, layer.heldVelocities[(size_t) fallback]);
                else
                    releaseVoice(layer.voices.front());
            }
            else
            {
                for (auto& voice : layer.voices)
                    if (voice.active && !layer.heldNotes[(size_t) voice.note])
                        releaseVoice(voice);
            }
        }
        return;
    }
    if (!message.isNoteOnOrOff()) return;

    const auto note = message.getNoteNumber() + config.octave * 12;
    if (note < 0 || note > 127 || message.getNoteNumber() < config.lowNote
        || message.getNoteNumber() > config.highNote) return;

    if (message.isNoteOff())
    {
        layer.heldNotes[(size_t) note] = false;
        if (config.mono || config.portamento)
        {
            const auto fallback = highestHeld();
            if (fallback >= 0)
                retargetMono(fallback, layer.heldVelocities[(size_t) fallback]);
            else if (!layer.sustainDown)
                releaseVoice(layer.voices.front());
        }
        else if (!layer.sustainDown)
        {
            for (auto& voice : layer.voices)
                if (voice.active && voice.note == note) releaseVoice(voice);
        }
        return;
    }

    const auto velocity = shapedVelocity(config, message.getFloatVelocity());
    layer.heldNotes[(size_t) note] = true;
    layer.heldVelocities[(size_t) note] = velocity;

    if (config.mono || config.portamento)
    {
        // Fade any residual polyphonic voices before continuing with one voice.
        for (size_t i = 1; i < layer.voices.size(); ++i) releaseVoice(layer.voices[i]);
        retargetMono(note, velocity);
        return;
    }

    Voice* target = nullptr;
    for (auto& voice : layer.voices)
        if (!voice.active || voice.releasing) { target = &voice; break; }
    if (target == nullptr) target = &layer.voices.front();

    target->active = true;
    target->releasing = false;
    target->note = note;
    target->velocity = velocity;
    target->targetFrequency = noteFrequency(note);
    target->currentFrequency = target->targetFrequency;
    target->phase.fill(0.0);
    target->envelope = 0.0f;
}


void Dx7Engine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& hostMidi,
                        const std::array<juce::MidiBuffer, layerCount>* routedMidi,
                        const std::array<Sf2Engine::LayerConfig, layerCount>& configs)
{
    const juce::ScopedLock guard(lock);

    // The SF2 engine clears the mix before this engine is called. DX7 and
    // external-instrument layers are additive, so do not clear output here.
    for (int index = 0; index < layerCount; ++index)
    {
        auto& layer = layers[(size_t) index];
        const auto& config = configs[(size_t) index];

        if (!config.enabled || layer.patchesLoaded <= 0)
            continue;

        for (const auto metadata : hostMidi)
            dispatch(layer, config, metadata.getMessage());

        if (routedMidi != nullptr)
            for (const auto metadata : (*routedMidi)[(size_t) index])
                dispatch(layer, config, metadata.getMessage());

        render(layer, config, output);
    }
}

void Dx7Engine::render(Layer& layer, const Sf2Engine::LayerConfig& config,
                       juce::AudioBuffer<float>& output)
{
    if (!juce::isPositiveAndBelow(layer.selectedPatch, layer.patchesLoaded)) return;
    const auto& patch = layer.patches[(size_t) layer.selectedPatch];

    // DX7 algorithms are six-operator bus programs. These flags correspond to
    // the published MSFA/Dexed Apache-2.0 algorithm representation:
    // bits 0..1 output bus, bit 2 add, bits 4..5 input bus, bits 6..7 feedback.
    // Keeping all 32 patterns avoids collapsing e.g. the Hammond algorithm 32
    // into an unrelated six-carrier sound.
    static constexpr std::array<std::array<uint8_t, 6>, 32> algorithms {{
        {{0xc1,0x11,0x11,0x14,0x01,0x14}}, {{0x01,0x11,0x11,0x14,0xc1,0x14}},
        {{0xc1,0x11,0x14,0x01,0x11,0x14}}, {{0xc1,0x11,0x94,0x01,0x11,0x14}},
        {{0xc1,0x14,0x01,0x14,0x01,0x14}}, {{0xc1,0x94,0x01,0x14,0x01,0x14}},
        {{0xc1,0x11,0x05,0x14,0x01,0x14}}, {{0x01,0x11,0xc5,0x14,0x01,0x14}},
        {{0x01,0x11,0x05,0x14,0xc1,0x14}}, {{0x01,0x05,0x14,0xc1,0x11,0x14}},
        {{0xc1,0x05,0x14,0x01,0x11,0x14}}, {{0x01,0x05,0x05,0x14,0xc1,0x14}},
        {{0xc1,0x05,0x05,0x14,0x01,0x14}}, {{0xc1,0x05,0x11,0x14,0x01,0x14}},
        {{0x01,0x05,0x11,0x14,0xc1,0x14}}, {{0xc1,0x11,0x02,0x25,0x05,0x14}},
        {{0x01,0x11,0x02,0x25,0xc5,0x14}}, {{0x01,0x11,0x11,0xc5,0x05,0x14}},
        {{0xc1,0x14,0x14,0x01,0x11,0x14}}, {{0x01,0x05,0x14,0xc1,0x14,0x14}},
        {{0x01,0x14,0x14,0xc1,0x14,0x14}}, {{0xc1,0x14,0x14,0x14,0x01,0x14}},
        {{0xc1,0x14,0x14,0x01,0x14,0x04}}, {{0xc1,0x14,0x14,0x14,0x04,0x04}},
        {{0xc1,0x14,0x14,0x04,0x04,0x04}}, {{0xc1,0x05,0x14,0x01,0x14,0x04}},
        {{0x01,0x05,0x14,0xc1,0x14,0x04}}, {{0x04,0xc1,0x11,0x14,0x01,0x14}},
        {{0xc1,0x14,0x01,0x14,0x04,0x04}}, {{0x04,0xc1,0x11,0x14,0x04,0x04}},
        {{0xc1,0x14,0x04,0x04,0x04,0x04}}, {{0xc4,0x04,0x04,0x04,0x04,0x04}}
    }};
    const auto& routing = algorithms[(size_t) juce::jlimit(0, 31, patch.algorithm)];
    int carrierCount = 0;
    for (const auto flags : routing)
        if ((flags & 0x07) == 0x04) ++carrierCount;
    carrierCount = juce::jmax(1, carrierCount);

    // Do not force every DX7 voice through a fixed 20 ms release. The fourth
    // EG rate belongs to each operator; use the average of the carriers to
    // keep a voice alive long enough for its programmed release contour and
    // avoid a discontinuity/click on note-off.
    float carrierReleaseRate = 0.0f;
    for (int op = 0; op < 6; ++op)
        if ((routing[(size_t) op] & 0x07) == 0x04)
            carrierReleaseRate += patch.egRates[(size_t) op][3];
    carrierReleaseRate /= (float) carrierCount;
    const auto releaseSeconds = juce::jlimit(0.025, 1.2,
        0.025 + (1.0 - (double) carrierReleaseRate) * 0.65);

    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            if (voice.releasing)
            {
                voice.envelope = juce::jmax(0.0f, voice.envelope
                    - (float) (1.0 / (sampleRate * releaseSeconds)));
                if (voice.envelope <= 0.0f)
                {
                    voice = {};
                    break;
                }
            }
            else
            {
                voice.envelope = juce::jmin(1.0f, voice.envelope
                    + (float) (1.0 / (sampleRate * 0.003)));
            }

            const auto glide = config.portamento ? 0.0025 : 1.0;
            voice.currentFrequency += (voice.targetFrequency - voice.currentFrequency) * glide;

            std::array<float, 3> buses {};
            std::array<bool, 3> hasBus {};
            for (int op = 0; op < 6; ++op)
            {
                const auto flags = routing[(size_t) op];
                const auto inputBus = (flags >> 4) & 0x03;
                const auto outputBus = flags & 0x03;
                const auto add = (flags & 0x04) != 0;
                // DX7 detune shifts frequency by a small, note-dependent
                // amount. This approximation is intentionally bounded; the
                // preceding fix restores the raw packed detune value instead
                // of accidentally treating it as oscillator fine tuning.
                const auto detuneSemitones = ((double) patch.detunes[(size_t) op] - 7.0) * 0.015;
                const auto detuneMultiplier = std::pow(2.0, detuneSemitones / 12.0);
                const auto frequency = (patch.fixedMode[(size_t) op]
                    ? (double) patch.fixedFrequency[(size_t) op]
                    : voice.currentFrequency * patch.ratios[(size_t) op]) * detuneMultiplier;
                // Avoid alias frequencies from an invalid/corrupt patch while
                // retaining the intended fixed-frequency or ratio behavior.
                const auto boundedFrequency = juce::jlimit(0.1, sampleRate * 0.45, frequency);
                voice.phase[(size_t) op] += juce::MathConstants<double>::twoPi
                    * boundedFrequency / sampleRate;
                if (voice.phase[(size_t) op] >= juce::MathConstants<double>::twoPi)
                    voice.phase[(size_t) op] = std::fmod(voice.phase[(size_t) op],
                                                         juce::MathConstants<double>::twoPi);

                auto& stage = voice.operatorStage[(size_t) op];
                auto& operatorEnvelope = voice.operatorEnvelope[(size_t) op];
                const auto target = patch.egLevels[(size_t) op][(size_t) stage];
                const auto rate = 0.00004f + patch.egRates[(size_t) op][(size_t) stage] * 0.008f;
                operatorEnvelope += (target - operatorEnvelope)
                    * juce::jlimit(0.0f, 1.0f, rate);
                if (!voice.releasing && stage < 2
                    && std::abs(target - operatorEnvelope) < 0.003f)
                    ++stage;
                if (voice.releasing)
                {
                    stage = 3;
                    operatorEnvelope *= 1.0f - juce::jlimit(0.00001f, 0.03f,
                        0.00004f + patch.egRates[(size_t) op][3] * 0.008f);
                }

                const auto modulation = inputBus != 0 && hasBus[(size_t) inputBus]
                    ? buses[(size_t) inputBus] * 6.0f : 0.0f;
                const auto feedback = ((flags & 0xc0) == 0xc0)
                    ? voice.feedback[(size_t) op] * ((float) patch.feedback / 7.0f) * 2.0f : 0.0f;
                const auto opSample = std::sin(voice.phase[(size_t) op] + modulation + feedback)
                    * patch.levels[(size_t) op] * operatorEnvelope;
                voice.feedback[(size_t) op] = (float) opSample;

                if (outputBus == 0)
                    buses[0] = add ? buses[0] + (float) opSample : (float) opSample;
                else
                    buses[(size_t) outputBus] = add && hasBus[(size_t) outputBus]
                        ? buses[(size_t) outputBus] + (float) opSample : (float) opSample;
                hasBus[(size_t) outputBus] = true;
            }

            const auto signal = std::tanh(buses[0] / (float) carrierCount);
            const auto value = signal * voice.velocity * voice.envelope * config.gain * 0.28f;
            for (int channel = 0; channel < output.getNumChannels(); ++channel)
                output.addSample(channel, sample, value);
        }
    }
}

