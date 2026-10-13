#include "SampleEngine.h"
#include <algorithm>
#include <cmath>

SampleEngine::SampleEngine()
{
    formats.registerBasicFormats();
}

void SampleEngine::prepare(double sampleRate, int maximumBlockSize)
{
    currentSampleRate = juce::jmax(1.0, sampleRate);
    currentBlockSize = juce::jmax(1, maximumBlockSize);
    juce::ignoreUnused(currentBlockSize);
}

juce::Result SampleEngine::loadSample(int layer, const juce::File& file)
{
    if (!juce::isPositiveAndBelow(layer, layerCount) || !file.existsAsFile())
        return juce::Result::fail("Arquivo de sample invalido.");

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return juce::Result::fail("Formato de audio nao suportado ou arquivo vazio.");

    const auto channels = juce::jmin(2, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> decoded(channels, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true))
        return juce::Result::fail("Nao foi possivel decodificar o sample.");

    const juce::ScopedLock stateLock(lock);
    auto& destination = layers[(size_t) layer];
    destination.audio = std::move(decoded);
    destination.sourcePath = file.getFullPathName();
    destination.sourceSampleRate = juce::jmax(1.0, reader->sampleRate);
    destination.config.startSeconds = 0.0;
    destination.config.endSeconds = 0.0;
    destination.config.loop = false;
    destination.peak.store(0.0f, std::memory_order_release);
    for (auto& voice : destination.voices) voice = {};
    return juce::Result::ok();
}

void SampleEngine::unload(int layer)
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return;
    const juce::ScopedLock stateLock(lock);
    auto& destination = layers[(size_t) layer];
    destination.audio.setSize(0, 0);
    destination.sourcePath.clear();
    for (auto& voice : destination.voices) voice = {};
    destination.peak.store(0.0f, std::memory_order_release);
}

void SampleEngine::stopAllSounds()
{
    const juce::ScopedLock stateLock(lock);
    for (auto& layer : layers)
        for (auto& voice : layer.voices) voice = {};
}

bool SampleEngine::isLoaded(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return false;
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() && layers[(size_t) layer].audio.getNumSamples() > 0;
}

juce::String SampleEngine::path(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return {};
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() ? layers[(size_t) layer].sourcePath : juce::String{};
}

float SampleEngine::getLayerPeak(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return 0.0f;
    return layers[(size_t) layer].peak.load(std::memory_order_relaxed);
}

SampleEngine::LayerConfig SampleEngine::getConfig(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return {};
    const juce::ScopedTryLock stateLock(lock);
    return stateLock.isLocked() ? layers[(size_t) layer].config : LayerConfig{};
}

void SampleEngine::setConfig(int layer, const LayerConfig& config)
{
    if (!juce::isPositiveAndBelow(layer, layerCount)) return;
    const juce::ScopedLock stateLock(lock);
    auto& destination = layers[(size_t) layer];
    destination.config = config;
    destination.config.rootNote = juce::jlimit(0, 127, config.rootNote);
    destination.config.startSeconds = juce::jmax(0.0, config.startSeconds);
    destination.config.endSeconds = juce::jmax(0.0, config.endSeconds);
}

void SampleEngine::startVoice(Layer& layer, int note, int channel, float velocity,
                              const Sf2Engine::LayerConfig& routing)
{
    auto* candidate = &layer.voices[0];
    for (auto& voice : layer.voices)
    {
        if (!voice.active)
        {
            candidate = &voice;
            break;
        }
        if (voice.age < candidate->age) candidate = &voice;
    }

    const auto endSample = layer.config.endSeconds > 0.0
        ? juce::jmin(layer.audio.getNumSamples(),
                    static_cast<int>(layer.config.endSeconds * layer.sourceSampleRate))
        : layer.audio.getNumSamples();
    const auto startSample = juce::jlimit(0, juce::jmax(0, endSample - 1),
                                          static_cast<int>(layer.config.startSeconds * layer.sourceSampleRate));
    const auto semitones = static_cast<double>(note - layer.config.rootNote);
    candidate->active = endSample > startSample;
    candidate->released = false;
    candidate->note = note;
    candidate->channel = channel;
    candidate->age = 0;
    candidate->position = static_cast<double>(startSample);
    candidate->increment = std::pow(2.0, semitones / 12.0)
                         * layer.sourceSampleRate / currentSampleRate;
    candidate->level = velocity * routing.gain;
    candidate->releaseGain = 1.0f;
}

void SampleEngine::releaseVoice(Layer& layer, int note, int channel)
{
    for (auto& voice : layer.voices)
        if (voice.active && voice.note == note && (channel == 0 || voice.channel == channel))
            voice.released = true;
}

float SampleEngine::readInterpolated(const juce::AudioBuffer<float>& audio,
                                     int channel, double position)
{
    if (audio.getNumSamples() <= 0) return 0.0f;
    const auto first = juce::jlimit(0, audio.getNumSamples() - 1, static_cast<int>(position));
    const auto second = juce::jmin(audio.getNumSamples() - 1, first + 1);
    const auto fraction = static_cast<float>(position - static_cast<double>(first));
    return audio.getSample(channel, first)
         + (audio.getSample(channel, second) - audio.getSample(channel, first)) * fraction;
}

void SampleEngine::renderLayer(Layer& layer, juce::AudioBuffer<float>& output,
                               const juce::MidiBuffer& midi,
                               const Sf2Engine::LayerConfig& routing)
{
    if (layer.audio.getNumSamples() <= 0 || !routing.enabled) return;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (routing.midiChannel > 0 && message.getChannel() != routing.midiChannel)
            continue;
        if (message.isNoteOn())
        {
            const auto inputNote = message.getNoteNumber();
            if (inputNote < routing.lowNote || inputNote > routing.highNote) continue;
            startVoice(layer, juce::jlimit(0, 127, inputNote + routing.octave * 12),
                       message.getChannel(), message.getFloatVelocity(), routing);
        }
        else if (message.isNoteOff())
        {
            const auto inputNote = message.getNoteNumber();
            if (inputNote < routing.lowNote || inputNote > routing.highNote) continue;
            releaseVoice(layer, juce::jlimit(0, 127, inputNote + routing.octave * 12),
                         message.getChannel());
        }
    }

    const auto endSample = layer.config.endSeconds > 0.0
        ? juce::jmin(layer.audio.getNumSamples(),
                    static_cast<int>(layer.config.endSeconds * layer.sourceSampleRate))
        : layer.audio.getNumSamples();
    const auto startSample = juce::jlimit(0, juce::jmax(0, endSample - 1),
                                          static_cast<int>(layer.config.startSeconds * layer.sourceSampleRate));
    const auto loopStart = static_cast<double>(startSample);
    const auto loopEnd = static_cast<double>(juce::jmax(startSample + 1, endSample));
    float peak = 0.0f;

    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            if (voice.position >= loopEnd)
            {
                if (layer.config.loop && !voice.released)
                    voice.position = loopStart;
                else
                {
                    voice.active = false;
                    break;
                }
            }

            const auto gain = voice.level * voice.releaseGain;
            const auto left = readInterpolated(layer.audio, 0, voice.position) * gain;
            const auto right = readInterpolated(layer.audio,
                                                juce::jmin(1, layer.audio.getNumChannels() - 1),
                                                voice.position) * gain;
            if (output.getNumChannels() > 0) output.addSample(0, sample, left);
            if (output.getNumChannels() > 1) output.addSample(1, sample, right);
            peak = juce::jmax(peak, std::abs(left), std::abs(right));
            voice.position += voice.increment;
            if (voice.released)
            {
                voice.releaseGain *= std::exp(-1.0f / static_cast<float>(0.12 * currentSampleRate));
                if (voice.releaseGain < 0.0001f) { voice.active = false; break; }
            }
        }
        ++voice.age;
    }
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.8f),
                     std::memory_order_relaxed);
}

void SampleEngine::process(juce::AudioBuffer<float>& output,
                           const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                           const std::array<Sf2Engine::LayerConfig, layerCount>& routing)
{
    const juce::ScopedTryLock stateLock(lock);
    if (!stateLock.isLocked()) return;
    for (int layer = 0; layer < layerCount; ++layer)
        renderLayer(layers[(size_t) layer], output, routedMidi[(size_t) layer], routing[(size_t) layer]);
}
