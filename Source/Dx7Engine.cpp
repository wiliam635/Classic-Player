#include "Dx7Engine.h"
#include <cmath>
#include <cstdint>

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
        if (c >= 32 && c <= 126) name << juce::String::charToString((juce::juce_wchar) c);
    }
    return name.trim();
}

Dx7Engine::Patch Dx7Engine::parsePatch(const juce::MemoryBlock& block)
{
    Patch patch;
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    const auto size = (int) block.getSize();

    // DX7 32-voice bulk dump: F0 43 .. 09 20 00, then 32 packed voices of
    // 128 bytes.  The first packed voice name is at offset 118.
    for (int start = 0; start < size; ++start)
    {
        if (bytes[start] != 0xf0 || start + 6 >= size || bytes[start + 1] != 0x43)
            continue;

        const auto remaining = size - start;
        if (remaining >= 6 + 128)
        {
            const auto* voice = bytes + start + 6;
            patch.name = decodeName(voice + 118, 10);
            patch.algorithm = voice[110] & 0x1f;
            for (int op = 0; op < 6; ++op)
            {
                const auto offset = op * 17;
                patch.levels[(size_t) op] = juce::jlimit(0.05f, 1.0f,
                    (float) voice[offset + 16] / 99.0f);
                patch.ratios[(size_t) op] = 0.5f + (float) (voice[offset + 15] & 0x1f) / 4.0f;
            }
            if (patch.name.isEmpty()) patch.name = "DX7 SysEx";
            return patch;
        }
    }

    // Single-voice dumps also have the ten-character program name in their
    // final bytes.  A valid Yamaha SysEx envelope is enough to load it.
    if (size >= 12 && bytes[0] == 0xf0 && bytes[1] == 0x43)
    {
        patch.name = decodeName(bytes + juce::jmax(0, size - 12), 10);
        if (patch.name.isEmpty()) patch.name = "DX7 Single Voice";
        return patch;
    }

    patch.name = "DX7 Patch";
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
    bool yamahaSysEx = false;
    for (size_t i = 0; i + 1 < bytes.getSize(); ++i)
        if (data[i] == 0xf0 && data[i + 1] == 0x43) { yamahaSysEx = true; break; }
    if (!yamahaSysEx)
        return juce::Result::fail("O arquivo nao contem um SysEx Yamaha DX7 valido.");

    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    layer.sourcePath = file.getFullPathName();
    layer.patch = parsePatch(bytes);
    for (auto& voice : layer.voices) voice = {};
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

juce::String Dx7Engine::patchName(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
        ? layers[(size_t) index].patch.name : juce::String{};
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
    for (auto& voice : layer.voices)
        if (!voice.active) { target = &voice; break; }
    if (target == nullptr)
        target = &layer.voices.front();
    if (config.mono)
        for (auto& voice : layer.voices) voice = {};

    target->active = true;
    target->note = note;
    target->velocity = shapedVelocity(config, message.getFloatVelocity());
    target->phase.fill(0.0);
}

void Dx7Engine::render(Layer& layer, const Sf2Engine::LayerConfig& config,
                       juce::AudioBuffer<float>& output)
{
    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        const auto base = noteFrequency(voice.note);
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            double modulator = 0.0;
            for (int op = 5; op >= 0; --op)
            {
                const auto frequency = base * layer.patch.ratios[(size_t) op];
                voice.phase[(size_t) op] += juce::MathConstants<double>::twoPi * frequency / sampleRate;
                if (voice.phase[(size_t) op] >= juce::MathConstants<double>::twoPi)
                    voice.phase[(size_t) op] = std::fmod(voice.phase[(size_t) op],
                                                         juce::MathConstants<double>::twoPi);
                const auto depth = (0.25 + (layer.patch.algorithm % 8) * 0.04);
                modulator = std::sin(voice.phase[(size_t) op] + modulator * depth)
                            * layer.patch.levels[(size_t) op];
            }
            const auto value = (float) (modulator * voice.velocity * config.gain * 0.30f);
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
