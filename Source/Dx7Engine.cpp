#include "Dx7Engine.h"
#include <cmath>
#include <cstdint>
#include <utility>
#include <algorithm>
#include "exp2.h"
#include "sin.h"
#include "freqlut.h"
#include "env.h"
#include "pitchenv.h"
#include "porta.h"

Dx7Engine::Dx7Engine()
    : tuning(createStandardTuning())
{
    controllers.core = &fmCore;
    controllers.refresh();
}

void Dx7Engine::prepare(double newSampleRate, int)
{
    const juce::ScopedLock guard(lock);
    sampleRate = juce::jmax(1.0, newSampleRate);
    // MSFA/Dexed lookup tables are sample-rate dependent. Initialising them
    // here prevents silent/invalid oscillator output on the first DX7 note.
    Exp2::init();
    Sin::init();
    Freqlut::init(sampleRate);
    Env::init_sr(sampleRate);
    PitchEnv::init(sampleRate);
    Porta::init_sr(sampleRate);
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
    patch.feedback = voice[111] & 0x07;
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
    // Expand Yamaha's 17-byte bulk representation into the 155-byte
    // single-voice layout expected by the MSFA/Dexed-compatible core.
    for (int op = 0; op < 6; ++op)
    {
        const auto packed = op * 17;
        const auto raw = op * 21;
        for (int i = 0; i <= 10; ++i) patch.raw[(size_t) (raw + i)] = voice[packed + i];
        patch.raw[(size_t) (raw + 11)] = voice[packed + 11] & 0x03;
        patch.raw[(size_t) (raw + 12)] = (voice[packed + 11] >> 2) & 0x03;
        patch.raw[(size_t) (raw + 13)] = voice[packed + 12] & 0x07;
        patch.raw[(size_t) (raw + 14)] = voice[packed + 13] & 0x03;
        patch.raw[(size_t) (raw + 15)] = (voice[packed + 13] >> 2) & 0x07;
        patch.raw[(size_t) (raw + 16)] = voice[packed + 14];
        patch.raw[(size_t) (raw + 17)] = voice[packed + 15] & 0x01;
        patch.raw[(size_t) (raw + 18)] = (voice[packed + 15] >> 1) & 0x1f;
        patch.raw[(size_t) (raw + 19)] = voice[packed + 16];
        patch.raw[(size_t) (raw + 20)] = (voice[packed + 12] >> 3) & 0x0f;
    }
    for (int i = 0; i < 8; ++i) patch.raw[(size_t) (126 + i)] = voice[102 + i];
    patch.raw[134] = voice[110] & 0x1f;               // algorithm
    patch.raw[135] = voice[111] & 0x07;               // feedback
    patch.raw[136] = (voice[111] >> 3) & 0x01;        // oscillator sync
    patch.raw[137] = voice[112];                      // LFO speed
    patch.raw[138] = voice[113];                      // LFO delay
    patch.raw[139] = voice[114];                      // pitch mod depth
    patch.raw[140] = voice[115];                      // amp mod depth
    patch.raw[141] = voice[116] & 0x01;               // LFO key sync
    patch.raw[142] = (voice[116] >> 1) & 0x07;        // LFO waveform
    patch.raw[143] = (voice[116] >> 4) & 0x07;        // pitch mod sensitivity
    patch.raw[144] = voice[117];                      // transpose
    for (int i = 0; i < 10; ++i) patch.raw[(size_t) (145 + i)] = voice[118 + i];
    if (patch.name.isEmpty()) patch.name = "DX7 Voice";
    return patch;
}

Dx7Engine::Patch Dx7Engine::parseSinglePatch(const uint8_t* voice, int size)
{
    Patch patch;
    if (size >= 155)
    {
        std::copy(voice, voice + 155, patch.raw.begin());
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

void Dx7Engine::beginCoreVoice(Voice& voice, const Patch& patch, int note,
                                   float velocity, bool preserveLegato)
{
    auto replacement = std::make_unique<Dx7Note>(tuning, nullptr);
    replacement->init(patch.raw.data(), note, juce::jlimit(1, 127,
        (int) std::lround(velocity * 127.0f)), 1, &controllers);

    if (preserveLegato && voice.coreVoice != nullptr)
    {
        replacement->transferState(*voice.coreVoice);
        replacement->initPortamento(*voice.coreVoice);
    }
    voice.coreVoice = std::move(replacement);
}

void Dx7Engine::dispatch(Layer& layer, const Sf2Engine::LayerConfig& config,
                         const juce::MidiMessage& message)
{
    if (!accepts(config, message)) return;

    const auto releaseVoice = [] (Voice& voice)
    {
        if (!voice.active) return;
        voice.releasing = true;
        if (voice.coreVoice != nullptr) voice.coreVoice->keyup();
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
        const auto patchIndex = juce::jlimit(0, juce::jmax(0, layer.patchesLoaded - 1),
                                               layer.selectedPatch);
        beginCoreVoice(voice, layer.patches[(size_t) patchIndex], note, velocity, wasActive);
        if (!wasActive)
        {
            voice.currentFrequency = voice.targetFrequency;
            voice.phase.fill(0.0);
            voice.envelope = 1.0f;
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
    target->envelope = 1.0f;
    const auto patchIndex = juce::jlimit(0, juce::jmax(0, layer.patchesLoaded - 1),
                                         layer.selectedPatch);
    beginCoreVoice(*target, layer.patches[(size_t) patchIndex], note, velocity, false);
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

    // MSFA renders 64 fixed-point samples per pass, just like Dexed. The
    // generated block is added to the Classic Player mix; no layer clears
    // the shared output buffer.
    constexpr float fixedToFloat = 1.0f / (float) (1 << 24);
    for (auto& voice : layer.voices)
    {
        if (!voice.active || voice.coreVoice == nullptr) continue;

        int offset = 0;
        while (offset < output.getNumSamples())
        {
            std::array<int32_t, N> fmBlock {};
            voice.coreVoice->compute(fmBlock.data(), 1 << 23, 1 << 24, &controllers);

            const auto count = juce::jmin(N, output.getNumSamples() - offset);
            const auto gain = config.gain * voice.velocity * 0.24f;
            for (int sample = 0; sample < count; ++sample)
            {
                const auto value = (float) fmBlock[(size_t) sample] * fixedToFloat * gain;
                for (int channel = 0; channel < output.getNumChannels(); ++channel)
                    output.addSample(channel, offset + sample, value);
            }
            offset += count;
        }

        if (voice.releasing && !voice.coreVoice->isPlaying())
            voice = {};
    }
}
