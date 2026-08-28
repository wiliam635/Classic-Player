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
    for (auto& peak : layerPeaks) peak.store(0.0f, std::memory_order_relaxed);
}

void Dx7Engine::prepare(double newSampleRate, int newMaximumBlockSize)
{
    const juce::ScopedLock guard(lock);
    sampleRate = juce::jmax(1.0, newSampleRate);
    maximumBlockSize = juce::jmax(64, newMaximumBlockSize);
    for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        auto& layer = layers[(size_t) layerIndex];
        layerPeaks[(size_t) layerIndex].store(0.0f, std::memory_order_relaxed);
        layer.renderScratch.setSize(2, maximumBlockSize, false, true, true);
        layer.gain.reset(sampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(0.0f);
        layer.gainReady = false;
        // 45 ms accommodates a gently modulated stereo chorus at any supported rate.
        layer.chorusDelay.setSize(2, juce::jmax(8, (int) std::ceil(sampleRate * 0.045)),
                                  false, true, true);
        layer.chorusWritePosition = 0;
        layer.chorusPhase = 0.0;
        layer.lfoPhase = 0.0;
        layer.lfoDelayProgress = 0.0;
        layer.dcInput = {};
        layer.dcOutput = {};
    }
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

    // Populate the existing layer in place. Layer contains JUCE reverb state
    // and unique voice pointers, so assigning a temporary Layer would require
    // a deleted copy/move assignment operator on macOS.
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

    loaded.renderScratch.setSize(2, maximumBlockSize, false, true, true);
    loaded.gain.reset(sampleRate, 0.02);
    loaded.gain.setCurrentAndTargetValue(0.0f);
    loaded.chorusDelay.setSize(2, juce::jmax(8, (int) std::ceil(sampleRate * 0.045)),
                               false, true, true);
    const juce::ScopedLock guard(lock);
    auto& target = layers[(size_t) index];
    target.gain.reset(sampleRate, 0.02);
    target.gain.setCurrentAndTargetValue(0.0f);
    target.gainReady = false;
    target.sourcePath = std::move(loaded.sourcePath);
    target.patches = std::move(loaded.patches);
    target.patchesLoaded = loaded.patchesLoaded;
    target.selectedPatch = loaded.selectedPatch;
    target.heldNotes.fill(false);
    target.heldVelocities.fill(0.0f);
    target.sustainDown = false;
    target.lfoPhase = 0.0;
    target.lfoDelayProgress = 0.0;
    target.chorusPhase = 0.0;
    target.chorusWritePosition = 0;
    target.dcInput = {};
    target.dcOutput = {};
    target.compressorEnvelope = {};
    target.filterState = {};
    target.reverb.reset();
    for (auto& voice : target.voices) voice = {};
    return juce::Result::ok();
}

void Dx7Engine::unload(int index)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    layer.sourcePath.clear();
    layer.patchesLoaded = 0;
    layer.selectedPatch = 0;
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.sustainDown = false;
    layer.lfoPhase = 0.0;
    layer.lfoDelayProgress = 0.0;
    layer.chorusPhase = 0.0;
    layer.chorusWritePosition = 0;
    layer.dcInput = {};
    layer.dcOutput = {};
    layer.compressorEnvelope = {};
    layer.filterState = {};
    layer.reverb.reset();
    for (auto& voice : layer.voices) voice = {};
    layerPeaks[(size_t) index].store(0.0f, std::memory_order_relaxed);
}

void Dx7Engine::stopAllSounds()
{
    const juce::ScopedLock guard(lock);
    for (auto& layer : layers)
        for (auto& voice : layer.voices) voice = {};
}

float Dx7Engine::getLayerPeak(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
        ? layerPeaks[(size_t) index].load(std::memory_order_relaxed) : 0.0f;
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
    voice.fmRead = N;
    voice.transition.retarget(sampleRate);
}

void Dx7Engine::dispatch(Layer& layer, const Sf2Engine::LayerConfig& config,
                         const juce::MidiMessage& message)
{
    if (!accepts(config, message)) return;

    const auto releaseVoice = [] (Voice& voice)
    {
        if (!voice.active || voice.releasing) return;
        voice.releasing = true;
        if (voice.coreVoice != nullptr) voice.coreVoice->keyup();
    };
    const auto highestHeld = [&layer] ()
    {
        int latest = -1;
        uint64_t order = 0;
        for (int note = 0; note < 128; ++note)
            if (layer.heldNotes[(size_t) note] && layer.noteOrder[(size_t) note] >= order)
            {
                latest = note;
                order = layer.noteOrder[(size_t) note];
            }
        return latest;
    };
    const auto retargetMono = [this, &layer, &config] (int note, float velocity)
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

    if (message.isPitchWheel())
    {
        // MSFA/Dexed already applies the controller value to every operator
        // during compute(). Keep the wheel in the same 14-bit MIDI domain so
        // the configured DX7 bend range and pitch-envelope behaviour remain
        // identical to the native engine.
        controllers.values_[kControllerPitch]
            = juce::jlimit(0, 16383, message.getPitchWheelValue());
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
            if (layer.voices.front().note != note) return;
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
    layer.noteOrder[(size_t) note] = ++layer.noteSequence;

    if (config.mono || config.portamento)
    {
        // Fade any residual polyphonic voices before continuing with one voice.
        for (size_t i = 1; i < layer.voices.size(); ++i) releaseVoice(layer.voices[i]);
        retargetMono(note, velocity);
        return;
    }

    Voice* target = nullptr;
    for (auto& voice : layer.voices)
        if (!voice.active) { target = &voice; break; }
    if (target == nullptr)
        for (auto& voice : layer.voices)
            if (voice.releasing) { target = &voice; break; }
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
        {
            layerPeaks[(size_t) index].store(layerPeaks[(size_t) index].load(std::memory_order_relaxed) * 0.82f,
                                               std::memory_order_relaxed);
            continue;
        }

        const juce::MidiBuffer empty;
        render(index, layer, config, output, hostMidi,
               routedMidi != nullptr ? (*routedMidi)[(size_t) index] : empty);
    }
}

void Dx7Engine::render(int layerIndex, Layer& layer, const Sf2Engine::LayerConfig& config,
                       juce::AudioBuffer<float>& output, const juce::MidiBuffer& hostMidi,
                       const juce::MidiBuffer& routedMidi)
{
    if (!juce::isPositiveAndBelow(layer.selectedPatch, layer.patchesLoaded)) return;
    if (maximumBlockSize < output.getNumSamples()
        || layer.chorusDelay.getNumSamples() == 0) return;

    auto& scratch = layer.renderScratch;
    scratch.setSize(2, output.getNumSamples(), false, false, true);
    scratch.clear();
    if (!layer.gainReady)
    {
        layer.gain.setCurrentAndTargetValue(config.gain);
        layer.gainReady = true;
    }
    layer.gain.setTargetValue(config.gain);
    const auto& patch = layer.patches[(size_t) layer.selectedPatch];
    constexpr float fixedToFloat = 1.0f / (float) (1 << 24);
    const auto lfoSpeed = juce::jlimit(0.0f, 1.0f, (float) patch.raw[137] / 99.0f);
    // The DX7 speed curve is deliberately exponential; a linear mapping makes
    // slow electric-piano tremolo far too fast.
    const auto lfoHz = 0.05 + 11.5 * lfoSpeed * lfoSpeed;
    const auto lfoStep = juce::MathConstants<double>::twoPi * lfoHz / sampleRate;
    const auto delaySeconds = 0.015 + 2.2 * (double) patch.raw[138] / 99.0;

    auto host = hostMidi.begin();
    auto routed = routedMidi.begin();
    for (int offset = 0; offset < scratch.getNumSamples(); ++offset)
    {
        // Merge both input streams by sample position, with host first on ties.
        while (host != hostMidi.end() || routed != routedMidi.end())
        {
            const bool useHost = routed == routedMidi.end()
                || (host != hostMidi.end() && (*host).samplePosition <= (*routed).samplePosition);
            const auto event = useHost ? *host : *routed;
            if (event.samplePosition > offset) break;
            dispatch(layer, config, event.getMessage());
            if (useHost) ++host; else ++routed;
        }
        const auto layerGain = layer.gain.getNextValue();
        for (auto& voice : layer.voices)
        {
            if (!voice.active || voice.coreVoice == nullptr) continue;
            if (voice.fmRead == N)
            {
                if (voice.releasing && !voice.coreVoice->isPlaying())
                {
                    voice = {};
                    continue;
                }
            // MSFA expects a Q24 LFO value. The previous fixed centre value
            // disabled the DX7's native amplitude modulation entirely.
            const auto blockPhase = layer.lfoPhase + (double) offset * lfoStep;
            const auto lfoWave = 0.5 + 0.5 * std::sin(blockPhase);
            const auto delayedDepth = delaySeconds > 0.0
                ? juce::jlimit(0.0, 1.0, (layer.lfoDelayProgress
                    + (double) offset / sampleRate) / delaySeconds) : 1.0;
            const auto lfoValue = juce::jlimit(0, 1 << 24,
                (int) std::lround((0.5 + (lfoWave - 0.5) * delayedDepth) * (double) (1 << 24)));
                voice.fmSamples.fill(0);
                voice.coreVoice->compute(voice.fmSamples.data(), lfoValue, 1 << 24, &controllers);
                voice.fmRead = 0;
            }
            // Preserve every MSFA sample across arbitrary host block sizes.
            const auto raw = static_cast<float>(voice.fmSamples[(size_t) voice.fmRead++])
                           * fixedToFloat * voice.velocity * 0.24f;
            const auto value = voice.transition.process(raw) * layerGain;
            scratch.addSample(0, offset, value);
            scratch.addSample(1, offset, value);
        }
    }
    layer.lfoPhase = std::fmod(layer.lfoPhase + (double) scratch.getNumSamples() * lfoStep,
                               juce::MathConstants<double>::twoPi);
    layer.lfoDelayProgress += (double) scratch.getNumSamples() / sampleRate;

    // Light stereo chorus for DX electric pianos. It is intentionally applied
    // only to the DX engine and stays dry when its layer amount is zero.
    const auto chorusAmount = juce::jlimit(0.0f, 100.0f, config.dx7Chorus) / 100.0f;
    const auto delayLength = layer.chorusDelay.getNumSamples();
    for (int sample = 0; sample < scratch.getNumSamples(); ++sample)
    {
        const auto phase = std::sin(layer.chorusPhase);
        const auto baseDelay = (int) std::round(sampleRate * 0.018);
        const auto spread = (int) std::round(sampleRate * 0.006 * phase);
        const auto leftRead = (layer.chorusWritePosition - baseDelay - spread + delayLength) % delayLength;
        const auto rightRead = (layer.chorusWritePosition - baseDelay + spread + delayLength) % delayLength;
        for (int channel = 0; channel < 2; ++channel)
        {
            auto* dry = scratch.getWritePointer(channel);
            auto* delay = layer.chorusDelay.getWritePointer(channel);
            const auto delayed = delay[channel == 0 ? leftRead : rightRead];
            delay[layer.chorusWritePosition] = dry[sample];
            dry[sample] = dry[sample] * (1.0f - 0.28f * chorusAmount)
                        + delayed * (0.28f * chorusAmount);
            // A DC blocker smooths discontinuities between note envelopes and
            // prevents the small click reported on release without dulling tone.
            const auto input = dry[sample];
            const auto filtered = input - layer.dcInput[(size_t) channel]
                                + 0.995f * layer.dcOutput[(size_t) channel];
            layer.dcInput[(size_t) channel] = input;
            layer.dcOutput[(size_t) channel] = filtered;
            dry[sample] = filtered;
        }
        layer.chorusWritePosition = (layer.chorusWritePosition + 1) % delayLength;
        layer.chorusPhase += juce::MathConstants<double>::twoPi * 0.28 / sampleRate;
        if (layer.chorusPhase >= juce::MathConstants<double>::twoPi)
            layer.chorusPhase -= juce::MathConstants<double>::twoPi;
    }

    // Apply the same layer effects used by SF2 so DX7 electric pianos and
    // organs respond to the mixer controls instead of bypassing them.
    const auto cutoffAmount = juce::jlimit(0.0f, 100.0f, config.cutoff);
    if (cutoffAmount < 99.5f)
    {
        const auto frequency = 80.0f * std::pow(250.0f, cutoffAmount / 100.0f);
        const auto coefficient = std::exp(-juce::MathConstants<float>::twoPi * frequency
                                          / static_cast<float>(sampleRate));
        for (int channel = 0; channel < 2; ++channel)
        {
            auto state = layer.filterState[(size_t) channel];
            auto* samples = scratch.getWritePointer(channel);
            for (int sample = 0; sample < scratch.getNumSamples(); ++sample)
            {
                state = (1.0f - coefficient) * samples[sample] + coefficient * state;
                samples[sample] = state;
            }
            layer.filterState[(size_t) channel] = state;
        }
    }
    const auto reverbMix = juce::jlimit(0.0f, 100.0f, config.reverb) / 100.0f;
    juce::Reverb::Parameters reverbParameters;
    reverbParameters.roomSize = juce::jlimit(0.0f, 1.0f, config.reverbSize / 100.0f);
    reverbParameters.damping = juce::jlimit(0.0f, 1.0f, config.reverbDamping / 100.0f);
    reverbParameters.width = juce::jlimit(0.0f, 1.0f, config.reverbWidth / 100.0f);
    reverbParameters.wetLevel = reverbMix * 0.72f;
    reverbParameters.dryLevel = 1.0f;
    layer.reverb.setParameters(reverbParameters);
    if (reverbMix > 0.001f)
        layer.reverb.processStereo(scratch.getWritePointer(0), scratch.getWritePointer(1),
                                   scratch.getNumSamples());
    const auto compressorMix = juce::jlimit(0.0f, 100.0f, config.compressor) / 100.0f;
    if (compressorMix > 0.001f)
    {
        const auto threshold = juce::jlimit(-60.0f, 0.0f, config.compressorThreshold);
        const auto ratio = juce::jlimit(1.0f, 20.0f, config.compressorRatio);
        const auto attack = std::exp(-1.0f / (juce::jmax(0.0001f, config.compressorAttack * 0.001f)
                                              * (float) sampleRate));
        const auto release = std::exp(-1.0f / (juce::jmax(0.001f, config.compressorRelease * 0.001f)
                                               * (float) sampleRate));
        const auto makeup = juce::Decibels::decibelsToGain(
            juce::jlimit(0.0f, 24.0f, config.compressorMakeup));
        for (int channel = 0; channel < 2; ++channel)
        {
            auto envelope = layer.compressorEnvelope[(size_t) channel];
            auto* samples = scratch.getWritePointer(channel);
            for (int sample = 0; sample < scratch.getNumSamples(); ++sample)
            {
                const auto dry = samples[sample];
                const auto magnitude = juce::jmax(1.0e-8f, std::abs(dry));
                const auto coefficient = magnitude > envelope ? attack : release;
                envelope = coefficient * envelope + (1.0f - coefficient) * magnitude;
                const auto over = juce::jmax(0.0f, juce::Decibels::gainToDecibels(envelope, -120.0f) - threshold);
                const auto reduction = over * (1.0f - 1.0f / ratio);
                const auto wet = dry * juce::Decibels::decibelsToGain(-reduction) * makeup;
                samples[sample] = dry + (wet - dry) * compressorMix;
            }
            layer.compressorEnvelope[(size_t) channel] = envelope;
        }
    }

    const auto renderedPeak = juce::jmax(scratch.getMagnitude(0, 0, output.getNumSamples()),
                                         scratch.getMagnitude(1, 0, output.getNumSamples()));
    const auto previousPeak = layerPeaks[(size_t) layerIndex].load(std::memory_order_relaxed) * 0.82f;
    layerPeaks[(size_t) layerIndex].store(juce::jmax(renderedPeak, previousPeak), std::memory_order_relaxed);

    for (int channel = 0; channel < juce::jmin(2, output.getNumChannels()); ++channel)
        output.addFrom(channel, 0, scratch, channel, 0, output.getNumSamples());
}
