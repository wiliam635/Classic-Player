#include "SampleEngine.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr double pianoCrossfadeSeconds = 0.55;

float releaseCoefficientFor(float releaseControl, double sampleRate)
{
    // Match the shared layer Release control: 50 is 0.5 s and 100 is 10 s.
    // Values are mapped to the time it takes the envelope to become inaudible.
    const auto value = juce::jlimit(0.0f, 100.0f, releaseControl);
    const auto releaseSeconds = value <= 50.0f
        ? juce::jmax(0.005f, value * 0.01f)
        : 0.5f * std::pow(20.0f, (value - 50.0f) / 50.0f);
    constexpr auto silenceThreshold = 0.0001f;
    const auto releaseSamples = juce::jmax(1.0, static_cast<double>(releaseSeconds) * sampleRate);
    return static_cast<float>(std::exp(std::log(silenceThreshold) / releaseSamples));
}

float calibratedVelocity(float input, const Sf2Engine::LayerConfig& config)
{
    const auto soft = config.velocitySoftInput;
    const auto medium = config.velocityMidInput;
    const auto hard = config.velocityHardInput;
    if (soft < 1 || medium - soft < 8 || hard - medium < 8 || hard > 127)
        return input;

    const auto raw = juce::jlimit(1.0f, 127.0f, input * 127.0f);
    const auto hardOutput = hard >= 120 ? 127.0f : 112.0f;
    const std::array<float, 5> outputAnchors { 1.0f, 24.0f, 64.0f, hardOutput, 127.0f };
    const std::array<float, 5> inputAnchors {
        1.0f, static_cast<float>(soft), static_cast<float>(medium),
        static_cast<float>(hard), 127.0f
    };
    for (size_t point = 1; point < inputAnchors.size(); ++point)
    {
        if (raw > inputAnchors[point]) continue;
        const auto span = inputAnchors[point] - inputAnchors[point - 1];
        const auto amount = span > 0.0f
            ? (raw - inputAnchors[point - 1]) / span : 0.0f;
        const auto mapped = juce::jmap(amount, outputAnchors[point - 1], outputAnchors[point]);
        return juce::jlimit(1.0f / 127.0f, 1.0f, mapped / 127.0f);
    }
    return 1.0f;
}
}

SampleEngine::SampleEngine()
{
    formats.registerBasicFormats();
}

void SampleEngine::prepare(double sampleRate, int maximumBlockSize)
{
    currentSampleRate = juce::jmax(1.0, sampleRate);
    currentBlockSize = juce::jmax(1, maximumBlockSize);
    scratch.setSize(2, currentBlockSize, false, false, true);
    for (auto& layer : layers)
    {
        layer.reverb.setSampleRate(currentSampleRate);
        layer.reverb.reset();
        layer.compressorEnvelope.fill(0.0f);
        layer.cutoffState.fill(0.0f);
        layer.pianoLowEqState.fill(0.0f);
        layer.pianoHighEqState.fill(0.0f);
        for (auto& gain : layer.pianoTimbreGains)
        {
            gain.reset(currentSampleRate, 0.02);
            gain.setCurrentAndTargetValue(1.0f);
        }
        layer.eq.reset();
        layer.nordFx.prepare(currentSampleRate);
        layer.gain.reset(currentSampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(0.8f);
    }
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

        // Prepare fresh decoded PCM outside the audio lock/callback. The
        // source NPNO stays untouched and voices need no extra fade buffers.
        if (file.getParentDirectory().getFileName() != "Organ")
            preparePianoLoops(decodedPiano);

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
        destination.cutoffState.fill(0.0f);
        destination.compressorEnvelope.fill(0.0f);
        destination.pianoLowEqState.fill(0.0f);
        destination.pianoHighEqState.fill(0.0f);
        destination.modulationAmount = 0.0f;
        destination.modulationPhase = 0.0;
        destination.eq.reset();
        destination.nordFx.reset();
        destination.reverb.reset();
        destination.lastReverbParameters.fill(-1.0f);
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
    destination.cutoffState.fill(0.0f);
    destination.compressorEnvelope.fill(0.0f);
    destination.pianoLowEqState.fill(0.0f);
    destination.pianoHighEqState.fill(0.0f);
    destination.modulationAmount = 0.0f;
    destination.modulationPhase = 0.0;
    destination.eq.reset();
    destination.nordFx.reset();
    destination.reverb.reset();
    destination.lastReverbParameters.fill(-1.0f);
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
    destination.cutoffState.fill(0.0f);
    destination.compressorEnvelope.fill(0.0f);
    destination.pianoLowEqState.fill(0.0f);
    destination.pianoHighEqState.fill(0.0f);
    destination.modulationAmount = 0.0f;
    destination.modulationPhase = 0.0;
    destination.eq.reset();
    destination.nordFx.reset();
    destination.reverb.reset();
    destination.lastReverbParameters.fill(-1.0f);
    for (auto& voice : destination.voices) voice = {};
    destination.peak.store(0.0f, std::memory_order_release);
}

void SampleEngine::stopAllSounds()
{
    const juce::ScopedLock stateLock(lock);
    for (auto& layer : layers)
    {
        layer.sustainDown.fill(false);
        layer.nordFx.reset();
        for (auto& voice : layer.voices) voice = {};
    }
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
    // This is queried by the UI and state serializer, never by the audio
    // callback. A failed try-lock made a loaded piano briefly look absent,
    // and that empty path could then be saved over the real instrument.
    const juce::ScopedLock stateLock(lock);
    return layers[(size_t) layer].sourcePath;
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

SampleEngine::Voice& SampleEngine::allocateVoice(Layer& layer, int polyphony)
{
    auto* candidate = &layer.voices[0];
    const auto limit = juce::jlimit(64, (int) layer.voices.size(), polyphony);
    const auto priority = [] (const Voice& voice)
    {
        return voice.released ? 0 : voice.waitingForSustain ? 1 : 2;
    };
    for (int i = 0; i < limit; ++i)
    {
        auto& voice = layer.voices[(size_t) i];
        if (!voice.active) return voice;
        const auto rank = priority(voice);
        const auto candidateRank = priority(*candidate);
        // Recycle quiet release tails first, then the quietest sustained
        // note, and only finally a held key. With a long piano decay the
        // oldest sustained note can still be loud when the pool fills.
        const auto loudness = [this] (const Voice& v)
        {
            const auto seconds = static_cast<double>(v.age) / currentSampleRate;
            const auto decay = v.sampleIndex >= 0
                ? juce::jmax(0.0, 1.0 - seconds / v.pianoDecaySeconds) : 1.0;
            return v.level * v.releaseGain * v.attackGain * static_cast<float>(decay * decay);
        };
        if (rank < candidateRank
            || (rank == candidateRank
                && ((rank < 2 && loudness(voice) < loudness(*candidate))
                    || (loudness(voice) == loudness(*candidate)
                        && voice.age > candidate->age))))
            candidate = &voice;
    }
    return *candidate;
}

void SampleEngine::startVoice(Layer& layer, int note, int channel, float velocity,
                              const Sf2Engine::LayerConfig& routing)
{
    auto* candidate = &allocateVoice(layer, routing.polyphony);

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
    candidate->level = velocity;
    candidate->velocity = velocity;
    const auto attackSamples = juce::jmax(1.0, routing.attack * 0.001 * currentSampleRate);
    candidate->attackGain = routing.attack <= 0.0f ? 1.0f : 0.0f;
    candidate->attackStep = routing.attack <= 0.0f
        ? 1.0f : static_cast<float>(1.0 / attackSamples);
    candidate->releaseGain = 1.0f;
    candidate->releaseCoefficient = 0.0f;
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

void SampleEngine::preparePianoLoops(NordPianoDecoder::Instrument& piano)
{
    const auto channels = piano.channels;
    if (channels < 1 || channels > 2) return;
    for (auto& stroke : piano.strokes)
    {
        const auto start = static_cast<int>(stroke.loopStartFrame);
        const auto end = static_cast<int>(stroke.loopEndFrame); // exclusive
        const auto length = end - start;
        if (stroke.bank == 2 || start < 0 || length < 8
            || end > stroke.frameCount(channels))
            continue;
        const auto maximum = static_cast<int>(length * 0.45);
        const auto aim = juce::jmin(maximum, juce::roundToInt(
            pianoCrossfadeSeconds * NordPianoDecoder::sampleRate));
        const auto frequency = 440.0 * std::pow(2.0, (stroke.rootNote - 69) / 12.0);
        const auto period = NordPianoDecoder::sampleRate / frequency;
        const auto low = juce::jmax(2, juce::roundToInt(aim - period));
        const auto high = juce::jmin(maximum, juce::roundToInt(aim + period));
        const auto step = juce::jmax(4, juce::roundToInt(period / 24.0));
        if (low > high) continue;
        const auto read = [&](int frame, int channel)
        {
            return static_cast<double>(stroke.interleavedPcm[
                static_cast<size_t>(frame * channels + channel)]);
        };
        struct LoopQuality
        {
            double score = -2.0;
            double correlation = 0.0;
            double levelDifferenceDb = 0.0;
            double localMismatchDb = 0.0;
            double phasePenalty = 0.0;
        };
        const auto analyse = [&](int candidate, int stride)
        {
            constexpr int windows = 12;
            std::array<double, windows> headPower {}, tailPower {}, dot {};
            for (int frame = 0; frame < candidate; frame += stride)
                for (int channel = 0; channel < channels; ++channel)
                {
                    const auto head = read(start + frame, channel);
                    const auto tail = read(end - candidate + frame, channel);
                    const auto window = juce::jmin(windows - 1, frame * windows / candidate);
                    dot[(size_t) window] += head * tail;
                    headPower[(size_t) window] += head * head;
                    tailPower[(size_t) window] += tail * tail;
                }
            double totalHead = 0.0, totalTail = 0.0, totalDot = 0.0;
            double localMismatch = 0.0, phasePenalty = 0.0;
            int activeWindows = 0;
            for (int window = 0; window < windows; ++window)
            {
                const auto head = headPower[(size_t) window];
                const auto tail = tailPower[(size_t) window];
                totalHead += head;
                totalTail += tail;
                totalDot += dot[(size_t) window];
                if (head + tail < 1.0) continue;
                ++activeWindows;
                localMismatch += std::abs(10.0 * std::log10((head + 1.0) / (tail + 1.0)));
                const auto norm = std::sqrt(head * tail);
                const auto localCorrelation = norm > 0.0 ? dot[(size_t) window] / norm : 1.0;
                phasePenalty += juce::jmax(0.0, 0.7 - localCorrelation);
            }
            LoopQuality quality;
            const auto norm = std::sqrt(totalHead * totalTail);
            quality.correlation = norm > 0.0 ? totalDot / norm : 1.0;
            quality.levelDifferenceDb = std::abs(10.0 * std::log10((totalHead + 1.0) / (totalTail + 1.0)));
            quality.localMismatchDb = localMismatch / juce::jmax(1, activeWindows);
            quality.phasePenalty = phasePenalty / juce::jmax(1, activeWindows);
            // Correlation alone can rank a loop with a large level mismatch
            // as "perfect". Such a join makes the 200-250 Hz body pulse at
            // each repetition even though it has no click at the seam.
            quality.score = quality.correlation - 0.018 * quality.levelDifferenceDb
                - 0.007 * quality.localMismatchDb - 0.08 * quality.phasePenalty
                - 0.01 * std::abs(candidate - aim) / juce::jmax(1, aim);
            return quality;
        };
        auto fade = low;
        auto best = analyse(fade, 8);
        for (int candidate = low + step; candidate <= high; candidate += step)
        {
            const auto quality = analyse(candidate, 8);
            if (quality.score > best.score) { best = quality; fade = candidate; }
        }
        // Widen the search only for a visibly poor join. Most NPNOs retain
        // the previous fast path and loading time remains close to unchanged.
        if (best.correlation < 0.85 || best.levelDifferenceDb > 3.0
            || best.localMismatchDb > 4.0 || best.phasePenalty > 0.15)
        {
            const auto radius = juce::roundToInt(0.08 * NordPianoDecoder::sampleRate);
            const auto broadLow = juce::jmax(2, aim - radius);
            const auto broadHigh = juce::jmin(maximum, aim + radius);
            const auto coarseStep = juce::jmax(96, juce::roundToInt(period / 2.0));
            auto coarseFade = fade;
            auto coarseBest = analyse(coarseFade, 32);
            for (int candidate = broadLow; candidate <= broadHigh; candidate += coarseStep)
            {
                const auto quality = analyse(candidate, 32);
                if (quality.score > coarseBest.score) { coarseBest = quality; coarseFade = candidate; }
            }
            const auto fineStep = juce::jmax(4, juce::roundToInt(period / 16.0));
            for (int candidate = juce::jmax(broadLow, coarseFade - coarseStep);
                 candidate <= juce::jmin(broadHigh, coarseFade + coarseStep); candidate += fineStep)
            {
                const auto quality = analyse(candidate, 8);
                if (quality.score > best.score) { best = quality; fade = candidate; }
            }
        }
        // Local energy matching keeps the overlap from briefly hollowing out
        // a strong piano note. It never attenuates the body or applies an EQ.
        constexpr int energyWindows = 12;
        std::array<double, energyWindows> headEnergy {}, tailEnergy {}, crossEnergy {}, counts {};
        for (int frame = 0; frame < fade; frame += 8)
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto window = juce::jmin(energyWindows - 1, frame * energyWindows / fade);
                const auto head = read(start + frame, channel);
                const auto tail = read(end - fade + frame, channel);
                headEnergy[(size_t) window] += head * head;
                tailEnergy[(size_t) window] += tail * tail;
                crossEnergy[(size_t) window] += head * tail;
                counts[(size_t) window] += 1.0;
            }
        for (int window = 0; window < energyWindows; ++window)
        {
            const auto count = juce::jmax(1.0, counts[(size_t) window]);
            headEnergy[(size_t) window] /= count;
            tailEnergy[(size_t) window] /= count;
            crossEnergy[(size_t) window] /= count;
        }
        const auto blend = [&](int frame, int channel, bool powerCompensation)
        {
            const auto weight = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::pi * frame / (fade - 1));
            auto gain = 1.0;
            if (powerCompensation && weight > 0.0 && weight < 1.0)
            {
                const auto position = static_cast<double>(frame) * (energyWindows - 1) / (fade - 1);
                const auto first = juce::jmin(energyWindows - 1, static_cast<int>(position));
                const auto second = juce::jmin(energyWindows - 1, first + 1);
                const auto fraction = position - first;
                const auto interpolate = [&](const auto& values)
                {
                    return values[(size_t) first] * (1.0 - fraction) + values[(size_t) second] * fraction;
                };
                const auto head = interpolate(headEnergy);
                const auto tail = interpolate(tailEnergy);
                const auto cross = interpolate(crossEnergy);
                const auto dry = 1.0 - weight;
                const auto actual = std::sqrt(juce::jmax(0.0,
                    dry * dry * tail + weight * weight * head + 2.0 * dry * weight * cross));
                const auto target = dry * std::sqrt(tail) + weight * std::sqrt(head);
                if (actual > 1.0 && target > actual)
                    gain = juce::jmin(1.15, target / actual);
            }
            return gain * ((1.0 - weight) * read(end - fade + frame, channel)
                           + weight * read(start + frame, channel));
        };
        bool powerCompensation = true;
        for (int frame = 0; frame < fade && powerCompensation; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                if (std::abs(blend(frame, channel, true)) > 32767.0)
                {
                    powerCompensation = false;
                    break;
                }
        for (int frame = 0; frame < fade; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                stroke.interleavedPcm[static_cast<size_t>((end - fade + frame) * channels + channel)]
                    = static_cast<std::int16_t>(std::round(blend(frame, channel, powerCompensation)));
        // The head has already played inside the overlap: resume after it.
        stroke.loopStartFrame += static_cast<std::uint32_t>(fade);
    }
}

void SampleEngine::startPianoVoice(Layer& layer, int sampleIndex, int note, int channel,
                                  float velocity, const Sf2Engine::LayerConfig& routing)
{
    if (!juce::isPositiveAndBelow(sampleIndex, static_cast<int>(layer.piano.strokes.size())))
        return;

    auto* candidate = &allocateVoice(layer, routing.polyphony);

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
    candidate->level = velocity * strokeGain;
    candidate->velocity = velocity;
    const auto attackSamples = juce::jmax(1.0, routing.attack * 0.001 * currentSampleRate);
    candidate->attackGain = routing.attack <= 0.0f ? 1.0f : 0.0f;
    candidate->attackStep = routing.attack <= 0.0f
        ? 1.0f : static_cast<float>(1.0 / attackSamples);
    candidate->releaseGain = 1.0f;
    candidate->releaseCoefficient = 0.0f;
    candidate->pianoRelease = stroke.bank == 2;
    candidate->pianoDecaySeconds = juce::jlimit(3.0f, 15.0f, routing.pianoDecaySeconds);
    candidate->waitingForSustain = false;
}

void SampleEngine::releasePianoVoice(Voice& voice, float releaseControl)
{
    if (!voice.active || voice.pianoRelease || voice.released) return;
    voice.waitingForSustain = false;
    voice.released = true;
    voice.releaseCoefficient = releaseCoefficientFor(releaseControl, currentSampleRate);
}

void SampleEngine::releaseVoice(Layer& layer, int note, int channel, float releaseControl)
{
    for (auto& voice : layer.voices)
        if (voice.active && !voice.released && voice.note == note
            && (channel == 0 || voice.channel == channel))
        {
            voice.released = true;
            voice.releaseCoefficient = releaseCoefficientFor(releaseControl, currentSampleRate);
        }
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
                                          int channels, int channel, double position, bool looped)
{
    const auto frames = stroke.frameCount(channels);
    if (frames <= 0 || channels <= 0) return 0.0f;
    const auto first = juce::jlimit(0, frames - 1, static_cast<int>(position));
    const auto sourceChannel = juce::jmin(channel, channels - 1);
    const auto fraction = static_cast<float>(position - static_cast<double>(first));
    const auto read = [&] (int frame)
    {
        if (looped && stroke.loopEndFrame > stroke.loopStartFrame
            && frame >= static_cast<int>(stroke.loopEndFrame))
            frame = static_cast<int>(stroke.loopStartFrame)
                + (frame - static_cast<int>(stroke.loopEndFrame))
                    % static_cast<int>(stroke.loopEndFrame - stroke.loopStartFrame);
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

void SampleEngine::readPianoStereoInterpolated(const NordPianoDecoder::Stroke& stroke,
                                               int channels, double position, bool looped,
                                               float& left, float& right)
{
    const auto frames = stroke.frameCount(channels);
    const auto first = static_cast<int>(position);
    // The usual sustain path is wholly inside the sample/loop. Read both
    // channels together so bounds and loop checks are not repeated eight
    // times per output frame. The boundary path retains the original reader.
    if (channels < 1 || first < 1 || first + 2 >= frames
        || (looped && first + 2 >= static_cast<int>(stroke.loopEndFrame)))
    {
        left = readPianoInterpolated(stroke, channels, 0, position, looped);
        right = readPianoInterpolated(stroke, channels, 1, position, looped);
        return;
    }

    const auto* pcm = stroke.interleavedPcm.data() + (size_t) (first - 1) * (size_t) channels;
    const auto fraction = static_cast<float>(position - static_cast<double>(first));
    const auto t2 = fraction * fraction;
    const auto t3 = t2 * fraction;
    const auto interpolate = [&] (int channel)
    {
        const auto y0 = static_cast<float>(pcm[channel]);
        const auto y1 = static_cast<float>(pcm[channels + channel]);
        const auto y2 = static_cast<float>(pcm[2 * channels + channel]);
        const auto y3 = static_cast<float>(pcm[3 * channels + channel]);
        return (0.5f / 32768.0f) * ((2.0f * y1)
            + (-y0 + y2) * fraction
            + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
            + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
    };
    left = interpolate(0);
    right = channels > 1 ? interpolate(1) : left;
}

void SampleEngine::renderLayer(Layer& layer, juce::AudioBuffer<float>& output,
                               const juce::MidiBuffer& midi,
                               const Sf2Engine::LayerConfig& routing)
{
    if ((!layer.pianoLoaded && layer.audio.getNumSamples() <= 0) || !routing.enabled) return;

    if (!routing.sustainEnabled)
    {
        layer.sustainDown.fill(false);
        for (auto& voice : layer.voices)
            if (voice.active && voice.waitingForSustain)
                releasePianoVoice(voice, routing.release);
    }

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (routing.midiChannel > 0 && message.getChannel() != routing.midiChannel)
            continue;

        if (message.isAllSoundOff() || message.isAllNotesOff())
        {
            const auto channel = juce::jlimit(1, 16, message.getChannel());
            layer.sustainDown[(size_t) channel - 1] = false;
            for (auto& voice : layer.voices)
                if (voice.active && voice.channel == channel)
                {
                    if (message.isAllSoundOff()) voice = {};
                    else if (layer.pianoLoaded) releasePianoVoice(voice, routing.release);
                    else releaseVoice(layer, voice.note, channel, routing.release);
                }
            continue;
        }

        if (message.isController() && message.getControllerNumber() == 1)
        {
            layer.modulationAmount = routing.modulationEnabled
                ? message.getControllerValue() / 127.0f : 0.0f;
            continue;
        }

        if (layer.pianoLoaded && message.isController() && message.getControllerNumber() == 64)
        {
            const auto channel = juce::jlimit(1, 16, message.getChannel());
            const auto wasDown = layer.sustainDown[(size_t) channel - 1];
            const auto isDown = routing.sustainEnabled && message.getControllerValue() >= 64;
            layer.sustainDown[(size_t) channel - 1] = isDown;
            if (wasDown && !isDown)
                for (auto& voice : layer.voices)
                    if (voice.active && voice.waitingForSustain && voice.channel == channel)
                        releasePianoVoice(voice, routing.release);
            continue;
        }

        if (message.isNoteOn())
        {
            if (!routing.acceptsVelocity((int) message.getVelocity())) continue;
            const auto inputNote = message.getNoteNumber();
            if (inputNote < routing.lowNote || inputNote > routing.highNote) continue;
            const auto note = juce::jlimit(0, 127, inputNote + routing.octave * 12);
            auto velocity = juce::jlimit(0.0f, 1.0f, message.getFloatVelocity());
            if (routing.velocityCurve == 3 && routing.velocityCalibrationEnabled)
                velocity = calibratedVelocity(velocity, routing);
            else if (routing.velocityCurve == 1) velocity = std::sqrt(velocity);
            else if (routing.velocityCurve == 2) velocity *= velocity;
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
                            releasePianoVoice(voice, routing.release);
                }
            }
            else
                releaseVoice(layer, note, message.getChannel(), routing.release);
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
    // Piano loops must not occupy the pool indefinitely under a held pedal.
    // Organs retain continuous sustain; piano notes use the per-voice tail length.
    const auto finitePiano = layer.pianoLoaded
        && juce::File(layer.sourcePath).getParentDirectory().getFileName() != "Organ";

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
            const auto seconds = static_cast<double>(voice.age) / currentSampleRate;
            if (isPianoVoice && finitePiano && seconds >= voice.pianoDecaySeconds)
            {
                voice.active = false;
                break;
            }
            const auto loopedPiano = pianoStroke != nullptr && !voice.pianoRelease
                && pianoStroke->loopEndFrame > pianoStroke->loopStartFrame;
            // Keep cycling during Release so the layer knob, rather than the
            // PCM end, determines the tail after a key/pedal is released.
            if (loopedPiano
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

            const auto remaining = juce::jlimit(0.0, 1.0, 1.0 - seconds / voice.pianoDecaySeconds);
            const auto lifetimeGain = isPianoVoice && finitePiano
                ? static_cast<float>(remaining * remaining) : 1.0f;
            const auto gain = voice.level * voice.releaseGain * voice.attackGain * lifetimeGain;
            float left = 0.0f;
            float right = 0.0f;
            if (isPianoVoice)
            {
                const auto& stroke = layer.piano.strokes[(size_t) voice.sampleIndex];
                readPianoStereoInterpolated(stroke, layer.piano.channels, voice.position,
                                            loopedPiano, left, right);
                left *= gain;
                right *= gain;
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
            voice.attackGain = juce::jmin(1.0f, voice.attackGain + voice.attackStep);
            voice.position += voice.increment;
            ++voice.age;
            if (voice.released)
            {
                voice.releaseGain *= voice.releaseCoefficient;
                if (voice.releaseGain < 0.0001f) { voice.active = false; break; }
            }
        }
    }
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.8f),
                     std::memory_order_relaxed);
}

void SampleEngine::processEffectsAndMix(Layer& layer, juce::AudioBuffer<float>& output,
                                        const Sf2Engine::LayerConfig& routing)
{
    const auto samples = scratch.getNumSamples();
    if (samples <= 0) return;

    if (layer.pianoLoaded)
    {
        const auto timbre = juce::jlimit(-1, 2, routing.pianoTimbre);
        const auto lowCorner = juce::jmin(250.0, currentSampleRate * 0.2);
        const auto highCorner = juce::jmin(2000.0, currentSampleRate * 0.4);
        // Independent voicings, applied to the entire sustained signal. Soft
        // no longer changes sample velocity or depends on the sustain bank.
        constexpr std::array<std::array<float, 3>, 3> voicingDb {{
            {{ 1.5f, -1.0f, -9.0f }}, // Soft: warm, subdued hammer attack
            {{ -3.0f, 3.0f, -3.0f }}, // Mid: focused body and presence
            {{ -1.0f, 0.0f, 6.0f }}  // Bright: clear upper harmonics
        }};
        for (size_t band = 0; band < 3; ++band)
            layer.pianoTimbreGains[band].setTargetValue(timbre < 0 ? 1.0f
                : juce::Decibels::decibelsToGain(voicingDb[(size_t) timbre][band]));
        const auto lowPole = static_cast<float>(std::exp(
            -juce::MathConstants<double>::twoPi * lowCorner / currentSampleRate));
        const auto highPole = static_cast<float>(std::exp(
            -juce::MathConstants<double>::twoPi * highCorner / currentSampleRate));
        for (int sample = 0; sample < samples; ++sample)
        {
            const auto lowGain = layer.pianoTimbreGains[0].getNextValue();
            const auto midGain = layer.pianoTimbreGains[1].getNextValue();
            const auto highGain = layer.pianoTimbreGains[2].getNextValue();
            for (int channel = 0; channel < 2; ++channel)
            {
                auto& lowState = layer.pianoLowEqState[(size_t) channel];
                auto& highState = layer.pianoHighEqState[(size_t) channel];
                auto* values = scratch.getWritePointer(channel);
                const auto input = values[sample];
                lowState = (1.0f - lowPole) * input + lowPole * lowState;
                highState = (1.0f - highPole) * input + highPole * highState;
                const auto lowBand = lowState;
                const auto midBand = highState - lowState;
                const auto highBand = input - highState;
                values[sample] = lowGain * lowBand + midGain * midBand + highGain * highBand;
            }
        }
    }

    const auto category = juce::File(layer.sourcePath).getParentDirectory().getFileName();
    const auto modulation = routing.modulationEnabled
        ? juce::jlimit(0.0f, 1.0f, layer.modulationAmount) : 0.0f;
    if (modulation > 0.001f && (category == "Piano Eletrico" || category == "Organ"))
    {
        const auto tremolo = category == "Piano Eletrico";
        const auto speedHz = tremolo ? 5.0f : 6.8f;
        const auto phaseStep = juce::MathConstants<double>::twoPi * speedHz / currentSampleRate;
        auto* left = scratch.getWritePointer(0);
        auto* right = scratch.getWritePointer(1);
        for (int sample = 0; sample < samples; ++sample)
        {
            const auto wave = static_cast<float>(std::sin(layer.modulationPhase));
            layer.modulationPhase += phaseStep;
            if (layer.modulationPhase >= juce::MathConstants<double>::twoPi)
                layer.modulationPhase -= juce::MathConstants<double>::twoPi;
            if (tremolo)
            {
                const auto gain = 1.0f - modulation * 0.58f * (0.5f + 0.5f * wave);
                left[sample] *= gain;
                right[sample] *= gain;
            }
            else
            {
                const auto pan = wave * modulation * 0.62f;
                left[sample] *= 1.0f - pan;
                right[sample] *= 1.0f + pan;
            }
        }
    }

    const auto cutoff = juce::jlimit(0.0f, 100.0f, routing.cutoff);
    if (cutoff < 99.5f)
    {
        const auto frequency = 80.0f * std::pow(250.0f, cutoff / 100.0f);
        const auto coefficient = std::exp(-juce::MathConstants<float>::twoPi * frequency
                                          / static_cast<float>(currentSampleRate));
        for (int channel = 0; channel < 2; ++channel)
        {
            auto state = layer.cutoffState[(size_t) channel];
            auto* values = scratch.getWritePointer(channel);
            for (int sample = 0; sample < samples; ++sample)
            {
                state = (1.0f - coefficient) * values[sample] + coefficient * state;
                values[sample] = state;
            }
            layer.cutoffState[(size_t) channel] = state;
        }
    }

    layer.eq.setParameters(routing.eqLow, routing.eqMid, routing.eqHigh,
                           currentSampleRate, routing.eqLowFrequency,
                           routing.eqMidFrequency, routing.eqHighFrequency,
                           routing.eqLowQ, routing.eqMidQ, routing.eqHighQ);
    for (int channel = 0; channel < 2; ++channel)
    {
        auto* values = scratch.getWritePointer(channel);
        for (int sample = 0; sample < samples; ++sample)
            values[sample] = layer.eq.process(values[sample], channel);
    }

    if (layer.pianoLoaded)
        layer.nordFx.process(scratch, routing.nordFx);

    const auto reverbMix = juce::jlimit(0.0f, 100.0f, routing.reverb) / 100.0f;
    const std::array<float, 4> reverbValues {
        reverbMix, routing.reverbSize, routing.reverbDamping, routing.reverbWidth
    };
    if (reverbValues != layer.lastReverbParameters)
    {
        juce::Reverb::Parameters parameters;
        parameters.roomSize = juce::jlimit(0.0f, 1.0f, routing.reverbSize / 100.0f);
        parameters.damping = juce::jlimit(0.0f, 1.0f, routing.reverbDamping / 100.0f);
        parameters.width = juce::jlimit(0.0f, 1.0f, routing.reverbWidth / 100.0f);
        parameters.wetLevel = reverbMix * 0.72f;
        parameters.dryLevel = 1.0f;
        parameters.freezeMode = 0.0f;
        layer.reverb.setParameters(parameters);
        layer.lastReverbParameters = reverbValues;
    }
    if (reverbMix > 0.001f)
        layer.reverb.processStereo(scratch.getWritePointer(0), scratch.getWritePointer(1), samples);

    const auto compressorMix = juce::jlimit(0.0f, 100.0f, routing.compressor) / 100.0f;
    if (compressorMix > 0.001f)
    {
        const auto thresholdDb = juce::jlimit(-60.0f, 0.0f, routing.compressorThreshold);
        const auto ratio = juce::jlimit(1.0f, 20.0f, routing.compressorRatio);
        const auto attackSeconds = juce::jmax(0.0001f, routing.compressorAttack * 0.001f);
        const auto releaseSeconds = juce::jmax(0.001f, routing.compressorRelease * 0.001f);
        const auto attackCoefficient = std::exp(-1.0f / (attackSeconds * (float) currentSampleRate));
        const auto releaseCoefficient = std::exp(-1.0f / (releaseSeconds * (float) currentSampleRate));
        const auto makeup = juce::Decibels::decibelsToGain(
            juce::jlimit(0.0f, 24.0f, routing.compressorMakeup));
        for (int channel = 0; channel < 2; ++channel)
        {
            auto envelope = layer.compressorEnvelope[(size_t) channel];
            auto* values = scratch.getWritePointer(channel);
            for (int sample = 0; sample < samples; ++sample)
            {
                const auto dry = values[sample];
                const auto magnitude = juce::jmax(1.0e-8f, std::abs(dry));
                const auto coefficient = magnitude > envelope ? attackCoefficient : releaseCoefficient;
                envelope = coefficient * envelope + (1.0f - coefficient) * magnitude;
                const auto levelDb = juce::Decibels::gainToDecibels(envelope, -120.0f);
                const auto overDb = juce::jmax(0.0f, levelDb - thresholdDb);
                const auto reductionDb = overDb * (1.0f - 1.0f / ratio);
                const auto wet = dry * juce::Decibels::decibelsToGain(-reductionDb) * makeup;
                values[sample] = dry + (wet - dry) * compressorMix;
            }
            layer.compressorEnvelope[(size_t) channel] = envelope;
        }
    }

    layer.gain.setTargetValue(juce::jmax(0.0f, routing.gain));
    const auto leftPan = routing.pan <= 0.0f ? 1.0f : 1.0f - routing.pan;
    const auto rightPan = routing.pan >= 0.0f ? 1.0f : 1.0f + routing.pan;
    float peak = 0.0f;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto gain = layer.gain.getNextValue();
        const auto left = scratch.getSample(0, sample) * gain * leftPan;
        const auto right = scratch.getSample(1, sample) * gain * rightPan;
        if (output.getNumChannels() > 0) output.addSample(0, sample, left);
        if (output.getNumChannels() > 1) output.addSample(1, sample, right);
        peak = juce::jmax(peak, std::abs(left), std::abs(right));
    }
    layer.peak.store(juce::jmax(peak, layer.peak.load(std::memory_order_relaxed) * 0.82f),
                     std::memory_order_relaxed);
}

void SampleEngine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                           const std::array<Sf2Engine::LayerConfig, layerCount>& routing)
{
    const juce::ScopedTryLock stateLock(lock);
    if (!stateLock.isLocked()) return;
    if (scratch.getNumSamples() != output.getNumSamples())
        scratch.setSize(2, output.getNumSamples(), false, false, true);
    for (int layer = 0; layer < layerCount; ++layer)
    {
        if (!layers[(size_t) layer].pianoLoaded || !routing[(size_t) layer].enabled)
            continue;
        auto& events = combinedMidi[(size_t) layer];
        events.clear();
        events.addEvents(midi, 0, -1, 0);
        events.addEvents(routedMidi[(size_t) layer], 0, -1, 0);
        scratch.clear();
        renderLayer(layers[(size_t) layer], scratch, events, routing[(size_t) layer]);
        processEffectsAndMix(layers[(size_t) layer], output, routing[(size_t) layer]);
    }
}
