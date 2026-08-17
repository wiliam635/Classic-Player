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
    for (int op = 0; op < 6; ++op)
    {
        // DX7 bulk voices compress each operator from 21 bytes to 17:
        // output level is byte 14, followed by mode/coarse and fine frequency.
        const auto offset = op * 17;
        const auto outputLevel = voice[offset + 14];
        const auto modeAndCoarse = voice[offset + 15];
        const auto coarse = (modeAndCoarse >> 1) & 0x1f;
        const auto fine = voice[offset + 16];
        patch.levels[(size_t) op] = juce::jlimit(0.0f, 1.0f,
            (float) outputLevel / 99.0f);
        // Fixed-frequency operators are represented as ratios here as a
        // practical fallback. Ratio mode remains exact enough for DX7 banks.
        patch.ratios[(size_t) op] = coarse == 0 ? 0.5f
            : (float) coarse + (float) fine / 100.0f;
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
        for (int op = 0; op < 6; ++op)
        {
            const auto offset = op * 21;
            if (offset + 18 >= size) break;
            const auto coarse = voice[offset + 18] & 0x1f;
            const auto fine = voice[offset + 19];
            patch.levels[(size_t) op] = juce::jlimit(0.0f, 1.0f,
                (float) voice[offset + 16] / 99.0f);
            patch.ratios[(size_t) op] = coarse == 0 ? 0.5f
                : (float) coarse + (float) fine / 100.0f;
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
    if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        for (auto& voice : layer.voices) voice = {};
        return;
    }
    if (!message.isNoteOnOrOff()) return;

    const auto note = message.getNoteNumber() + config.octave * 12;
    if (note < 0 || note > 127 || message.getNoteNumber() < config.lowNote
        || message.getNoteNumber() > config.highNote) return;

    if (message.isNoteOff())
    {
        for (auto& voice : layer.voices)
            if (voice.active && voice.note == note) voice = {};
        return;
    }

    Voice* target = nullptr;
    if (config.portamento)
        for (auto& voice : layer.voices)
            if (voice.active) { target = &voice; break; }
    if (target == nullptr)
        for (auto& voice : layer.voices)
            if (!voice.active) { target = &voice; break; }
    if (target == nullptr) target = &layer.voices.front();

    const auto destination = noteFrequency(note);
    const auto wasActive = target->active;
    target->active = true;
    target->note = note;
    target->velocity = shapedVelocity(config, message.getFloatVelocity());
    target->targetFrequency = destination;
    if (!wasActive || !config.portamento)
    {
        target->currentFrequency = destination;
        target->phase.fill(0.0);
    }
}

void Dx7Engine::render(Layer& layer, const Sf2Engine::LayerConfig& config,
                       juce::AudioBuffer<float>& output)
{
    if (!juce::isPositiveAndBelow(layer.selectedPatch, layer.patchesLoaded)) return;
    const auto& patch = layer.patches[(size_t) layer.selectedPatch];
    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            const auto glide = config.portamento ? 0.0025 : 1.0;
            voice.currentFrequency += (voice.targetFrequency - voice.currentFrequency) * glide;
            for (int op = 0; op < 6; ++op)
            {
                const auto frequency = voice.currentFrequency * patch.ratios[(size_t) op];
                voice.phase[(size_t) op] += juce::MathConstants<double>::twoPi * frequency / sampleRate;
                if (voice.phase[(size_t) op] >= juce::MathConstants<double>::twoPi)
                    voice.phase[(size_t) op] = std::fmod(voice.phase[(size_t) op],
                                                         juce::MathConstants<double>::twoPi);
            }

            // The DX7 exposes 32 routing algorithms. This native player uses
            // eight FM routing families selected from that value, retaining
            // all six operator levels/ratios instead of reducing every voice
            // to one serial sine chain.
            const auto op = [&voice, &patch] (int number, double modulation)
            {
                return std::sin(voice.phase[(size_t) number] + modulation)
                       * (double) patch.levels[(size_t) number];
            };
            const auto index = 1.0 + (double) (patch.algorithm % 8) * 0.55;
            double signal = 0.0;
            switch (patch.algorithm % 8)
            {
                case 0: signal = op(0, op(1, op(2, op(3, op(4, op(5, 0.0) * index) * index) * index) * index) * index); break;
                case 1: signal = op(0, op(1, op(2, 0.0) * index) * index)
                               + op(3, op(4, op(5, 0.0) * index) * index); break;
                case 2: signal = op(0, op(1, 0.0) * index) + op(2, op(3, 0.0) * index)
                               + op(4, op(5, 0.0) * index); break;
                case 3: signal = op(0, (op(1, 0.0) + op(2, op(3, 0.0) * index)) * index)
                               + op(4, op(5, 0.0) * index); break;
                case 4: signal = op(0, op(1, op(2, 0.0) * index) * index)
                               + op(3, 0.0) + op(4, 0.0) + op(5, 0.0); break;
                case 5: signal = op(0, (op(1, 0.0) + op(2, 0.0) + op(3, 0.0)) * index)
                               + op(4, op(5, 0.0) * index); break;
                case 6: signal = op(0, op(1, 0.0) * index) + op(2, 0.0)
                               + op(3, 0.0) + op(4, 0.0) + op(5, 0.0); break;
                default: signal = op(0, 0.0) + op(1, 0.0) + op(2, 0.0)
                               + op(3, 0.0) + op(4, 0.0) + op(5, 0.0); break;
            }
            const auto value = (float) (signal * voice.velocity * config.gain * 0.15f);
            for (int channel = 0; channel < output.getNumChannels(); ++channel)
                output.addSample(channel, sample, value);
        }
    }
}

void Dx7Engine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& hostMidi,
                        const std::array<juce::MidiBuffer, layerCount>* routedMidi,
                        const std::array<Sf2Engine::LayerConfig, layerCount>& configs)
{
    const juce::ScopedLock guard(lock);
    for (int index = 0; index < layerCount; ++index)
    {
        auto& layer = layers[(size_t) index];
        const auto& config = configs[(size_t) index];
        if (layer.sourcePath.isEmpty() || !config.enabled) continue;
        for (const auto metadata : hostMidi) dispatch(layer, config, metadata.getMessage());
        if (routedMidi != nullptr)
            for (const auto metadata : (*routedMidi)[(size_t) index])
                dispatch(layer, config, metadata.getMessage());
        render(layer, config, output);
    }
}
