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

    if (file.getFileExtension().equalsIgnoreCase(".npno"))
    {
        NordPianoDecoder::Instrument decodedPiano;
        const auto result = NordPianoDecoder::decodeFile(file, decodedPiano);
        if (result.failed()) return result;

        const juce::ScopedLock stateLock(lock);
        auto& destination = layers[(size_t) layer];
        destination.audio.setSize(0, 0);
        destination.piano = std::move(decodedPiano);
        destination.pianoLoaded = true;
        destination.sourcePath = file.getFullPathName();
        destination.sourceSampleRate = NordPianoDecoder::sampleRate;
        destination.config.startSeconds = 0.0;
        destination.config.endSeconds = 0.0;
        destination.config.loop = false;
        destination.peak.store(0.0f, std::memory_order_release);
        destination.sustainDown.fill(false);
        for (auto& voice : destination.voices) voice = {};
        return juce::Result::ok();
    }

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
    destination.piano = {};
    destination.pianoLoaded = false;
    destination.sourcePath = file.getFullPathName();
    destination.sourceSampleRate = juce::jmax(1.0, reader->sampleRate);
    destination.sustainDown.fill(false);
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
    destination.piano = {};
    destination.pianoLoaded = false;
    destination.sourcePath.clear();
    destination.sustainDown.fill(false);
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
    return stateLock.isLocked() && (layers[(size_t) layer].pianoLoaded
                                    || layers[(size_t) layer].audio.getNumSamples() > 0);
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
    candidate->sampleIndex = -1;
    candidate->position = static_cast<double>(startSample);
    candidate->increment = std::pow(2.0, semitones / 12.0)
                         * layer.sourceSampleRate / currentSampleRate;
    candidate->level = velocity * routing.gain;
    candidate->velocity = velocity;
    candidate->releaseGain = 1.0f;
    candidate->pianoRelease = false;
    candidate->waitingForSustain = false;
}

int SampleEngine::findPianoStroke(const Layer& layer, int note, float velocity, int bank) const
{
    if (!layer.pianoLoaded || !juce::isPositiveAndBelow(note, 128)) return -1;
    const auto root = layer.piano.keyMap[(size_t) note];
    if (root == 0xff) return -1;

    const auto midiVelocity = juce::jlimit(1, 127, juce::roundToInt(velocity * 127.0f));
    const auto layerLimit = ((127 - midiVelocity) * layer.piano.velocityScale) / 127;
    int bestIndex = -1;
    int bestLayer = -1;
    for (size_t i = 0; i < layer.piano.strokes.size(); ++i)
    {
        const auto& stroke = layer.piano.strokes[i];
        if (stroke.rootNote != root || stroke.bank != bank
            || stroke.velocityLayer > layerLimit || stroke.velocityLayer <= bestLayer)
            continue;
        bestIndex = static_cast<int>(i);
        bestLayer = stroke.velocityLayer;
    }
    return bestIndex;
}

void SampleEngine::startPianoVoice(Layer& layer, int sampleIndex, int note, int channel,
                                  float velocity, const Sf2Engine::LayerConfig& routing)
{
    if (!juce::isPositiveAndBelow(sampleIndex, static_cast<int>(layer.piano.strokes.size())))
        return;

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

    const auto& stroke = layer.piano.strokes[(size_t) sampleIndex];
    const auto frames = stroke.frameCount(layer.piano.channels);
    const auto cents = static_cast<double>(layer.piano.fineTune[(size_t) note]) * 0.7;
    const auto semitones = static_cast<double>(note - stroke.rootNote) + cents / 100.0;
    const auto globalGainDb = static_cast<double>(layer.piano.gainTenthsDb) / 10.0;
    const auto trim = static_cast<int>(stroke.trimDb);
    const auto signedTrim = trim >= 0x8000 ? trim - 0x10000 : trim;
    const auto strokeGainDb = layer.piano.signedTenthsDbTrim
                            ? -static_cast<double>(signedTrim) / 10.0
                            : -static_cast<double>(trim);
    const auto strokeGain = static_cast<float>(std::pow(10.0, (globalGainDb + strokeGainDb) / 20.0));

    candidate->active = frames > 0;
    candidate->released = false;
    candidate->note = note;
    candidate->channel = channel;
    candidate->age = 0;
    candidate->sampleIndex = sampleIndex;
    candidate->position = 0.0;
    candidate->increment = std::pow(2.0, semitones / 12.0)
                         * static_cast<double>(NordPianoDecoder::sampleRate) / currentSampleRate;
    candidate->level = velocity * routing.gain * strokeGain;
    candidate->velocity = velocity;
    candidate->releaseGain = 1.0f;
    candidate->pianoRelease = stroke.bank == 2;
    candidate->waitingForSustain = false;
}

void SampleEngine::releasePianoVoice(Voice& voice)
{
    if (!voice.active || voice.pianoRelease) return;
    voice.waitingForSustain = false;
    voice.released = true;
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

float SampleEngine::readPianoInterpolated(const NordPianoDecoder::Stroke& stroke,
                                          int channels, int channel, double position)
{
    const auto frames = stroke.frameCount(channels);
    if (frames <= 0 || channels <= 0) return 0.0f;
    const auto first = juce::jlimit(0, frames - 1, static_cast<int>(position));
    const auto sourceChannel = juce::jmin(channel, channels - 1);
    const auto fraction = static_cast<float>(position - static_cast<double>(first));
    const auto read = [&] (int frame)
    {
        const auto boundedFrame = juce::jlimit(0, frames - 1, frame);
        const auto index = static_cast<size_t>(boundedFrame * channels + sourceChannel);
        return static_cast<float>(stroke.interleavedPcm[index]) / 32768.0f;
    };

    // Four-point Catmull-Rom interpolation avoids the softened, grainy top end of
    // linear interpolation when a 35.002 kHz Nord stroke is transposed or resampled.
    const auto y0 = read(first - 1);
    const auto y1 = read(first);
    const auto y2 = read(first + 1);
    const auto y3 = read(first + 2);
    const auto t2 = fraction * fraction;
    const auto t3 = t2 * fraction;
    return 0.5f * ((2.0f * y1)
                   + (-y0 + y2) * fraction
                   + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
                   + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
}

void SampleEngine::renderLayer(Layer& layer, juce::AudioBuffer<float>& output,
                               const juce::MidiBuffer& midi,
                               const Sf2Engine::LayerConfig& routing)
{
    if ((!layer.pianoLoaded && layer.audio.getNumSamples() <= 0) || !routing.enabled) return;

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (routing.midiChannel > 0 && message.getChannel() != routing.midiChannel)
            continue;

        if (layer.pianoLoaded && message.isController() && message.getControllerNumber() == 64)
        {
            const auto channel = juce::jlimit(1, 16, message.getChannel());
            const auto wasDown = layer.sustainDown[(size_t) channel - 1];
            const auto isDown = message.getControllerValue() >= 64;
            layer.sustainDown[(size_t) channel - 1] = isDown;
            if (wasDown && !isDown)
                for (auto& voice : layer.voices)
                    if (voice.active && voice.waitingForSustain && voice.channel == channel)
                        releasePianoVoice(voice);
            continue;
        }

        if (message.isNoteOn())
        {
            const auto inputNote = message.getNoteNumber();
            if (inputNote < routing.lowNote || inputNote > routing.highNote) continue;
            const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
            const auto velocity = message.getFloatVelocity();
            if (layer.pianoLoaded)
            {
                const auto channel = juce::jlimit(1, 16, message.getChannel());
                const auto bank = layer.sustainDown[(size_t) channel - 1] ? 1 : 0;
                auto stroke = findPianoStroke(layer, note, velocity, bank);
                if (stroke < 0 && bank == 1)
                    stroke = findPianoStroke(layer, note, velocity, 0);
                if (stroke >= 0)
                    startPianoVoice(layer, stroke, note, channel, velocity, routing);
            }
            else
                startVoice(layer, note, message.getChannel(), velocity, routing);
        }
        else if (message.isNoteOff())
        {
            const auto inputNote = message.getNoteNumber();
            if (inputNote < routing.lowNote || inputNote > routing.highNote) continue;
            const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
            if (layer.pianoLoaded)
            {
                const auto channel = juce::jlimit(1, 16, message.getChannel());
                if (layer.sustainDown[(size_t) channel - 1])
                {
                    for (auto& voice : layer.voices)
                        if (voice.active && voice.sampleIndex >= 0 && !voice.pianoRelease
                            && voice.note == note && voice.channel == channel)
                            voice.waitingForSustain = true;
                }
                else
                {
                    for (auto& voice : layer.voices)
                        if (voice.active && voice.sampleIndex >= 0 && !voice.pianoRelease
                            && voice.note == note && voice.channel == channel)
                            releasePianoVoice(voice);
                }
            }
            else
                releaseVoice(layer, note, message.getChannel());
        }
    }

    const auto normalEndSample = layer.config.endSeconds > 0.0
        ? juce::jmin(layer.audio.getNumSamples(),
                    static_cast<int>(layer.config.endSeconds * layer.sourceSampleRate))
        : layer.audio.getNumSamples();
    const auto normalStartSample = juce::jlimit(0, juce::jmax(0, normalEndSample - 1),
                                          static_cast<int>(layer.config.startSeconds * layer.sourceSampleRate));
    const auto loopStart = static_cast<double>(normalStartSample);
    const auto normalLoopEnd = static_cast<double>(juce::jmax(normalStartSample + 1, normalEndSample));
    float peak = 0.0f;

    for (auto& voice : layer.voices)
    {
        if (!voice.active) continue;

        const auto isPianoVoice = voice.sampleIndex >= 0;
        if (isPianoVoice && (!layer.pianoLoaded
            || !juce::isPositiveAndBelow(voice.sampleIndex, static_cast<int>(layer.piano.strokes.size()))))
        {
            voice.active = false;
            continue;
        }

        const auto* pianoStroke = isPianoVoice
            ? &layer.piano.strokes[(size_t) voice.sampleIndex] : nullptr;
        const auto pianoFrames = pianoStroke != nullptr
            ? pianoStroke->frameCount(layer.piano.channels) : 0;
        const auto loopEnd = isPianoVoice ? static_cast<double>(pianoFrames) : normalLoopEnd;
        if (pianoFrames <= 0 && isPianoVoice)
        {
            voice.active = false;
            continue;
        }

        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            if (pianoStroke != nullptr && !voice.released && !voice.pianoRelease
                && pianoStroke->loopEndFrame > pianoStroke->loopStartFrame
                && voice.position >= pianoStroke->loopEndFrame)
                voice.position = pianoStroke->loopStartFrame
                               + std::fmod(voice.position - pianoStroke->loopEndFrame,
                                           static_cast<double>(pianoStroke->loopEndFrame
                                                               - pianoStroke->loopStartFrame));
            if (voice.position >= loopEnd)
            {
                if (!isPianoVoice && layer.config.loop && !voice.released)
                    voice.position = loopStart;
                else
                {
                    voice.active = false;
                    break;
                }
            }

            const auto gain = voice.level * voice.releaseGain;
            float left = 0.0f;
            float right = 0.0f;
            if (isPianoVoice)
            {
                const auto& stroke = layer.piano.strokes[(size_t) voice.sampleIndex];
                left = readPianoInterpolated(stroke, layer.piano.channels, 0, voice.position) * gain;
                right = readPianoInterpolated(stroke, layer.piano.channels, 1, voice.position) * gain;
            }
            else
            {
                left = readInterpolated(layer.audio, 0, voice.position) * gain;
                right = readInterpolated(layer.audio,
                                         juce::jmin(1, layer.audio.getNumChannels() - 1),
                                         voice.position) * gain;
            }
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

void SampleEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                           const std::array<Sf2Engine::LayerConfig, layerCount>& routing)
{
    const juce::ScopedTryLock stateLock(lock);
    if (!stateLock.isLocked()) return;
    for (int layer = 0; layer < layerCount; ++layer)
    {
        if (!layers[(size_t) layer].pianoLoaded || !routing[(size_t) layer].enabled)
            continue;
        auto& events = combinedMidi[(size_t) layer];
        events.clear();
        events.addEvents(midi, 0, -1, 0);
        events.addEvents(routedMidi[(size_t) layer], 0, -1, 0);
        renderLayer(layers[(size_t) layer], output, events, routing[(size_t) layer]);
    }
}
