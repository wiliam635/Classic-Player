#include "NsmpEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "NordLegacySampleDecoder.h"

namespace nsmp_android {
namespace {
constexpr float envelopeFloor = 0.0001f; // -80 dB
std::string loadInfo;

float releaseCoefficientFor(float seconds, int outputRate)
{
    const double clamped = std::clamp(static_cast<double>(seconds), 0.005, 15.0);
    return static_cast<float>(std::exp(std::log(static_cast<double>(envelopeFloor))
                                       / std::max(1.0, clamped * outputRate)));
}

// Splice the loop end onto its start (same as the desktop engine) so a held note does not click at the
// seam, without discarding frames: shortening a single-cycle loop would change its pitch.
void prepareLoopCrossfades(Instrument& instrument)
{
    for (auto& zone : instrument.zones) {
        const auto start = zone.loopStart, end = zone.loopEnd;
        if (end <= start + 32 || end > zone.frames()) continue;
        const auto fade = std::min<std::size_t>(256, (end - start) / 4);
        if (fade < 8) continue;
        for (int channel = 0; channel < zone.channels; ++channel) {
            const auto index = [&](std::size_t frame) { return frame * (std::size_t) zone.channels + (std::size_t) channel; };
            const auto first = zone.pcm[index(start)] * NordSampleDecoder::pcmToFloat;
            const auto second = zone.pcm[index(start + 1)] * NordSampleDecoder::pcmToFloat;
            const auto last = zone.pcm[index(end - 1)] * NordSampleDecoder::pcmToFloat;
            const auto valueDelta = (2.0f * first - second) - last;
            for (std::size_t i = 0; i < fade; ++i) {
                const auto t = (float) i / (float) (fade - 1);
                const auto blend = t * t * (3.0f - 2.0f * t);
                auto& stored = zone.pcm[index(end - fade + i)];
                stored = NordSampleDecoder::floatToPcm(stored * NordSampleDecoder::pcmToFloat + valueDelta * blend);
            }
        }
    }
}

int findZone(const Instrument& instrument, int note, int velocity)
{
    int best = -1, bestDistance = 1000;
    for (std::size_t i = 0; i < instrument.zones.size(); ++i) {
        const auto& zone = instrument.zones[i];
        if (note < zone.low || note > zone.high) continue;
        const auto distance = velocity < zone.velocityLow ? zone.velocityLow - velocity
                            : velocity > zone.velocityHigh ? velocity - zone.velocityHigh : 0;
        if (distance < bestDistance) {
            best = (int) i;
            bestDistance = distance;
            if (distance == 0) break;
        }
    }
    return best;
}

void readStereoSample(const NordSampleDecoder::Zone& zone, double position, int frames, float& left, float& right)
{
    if (frames <= 0 || zone.channels <= 0) { left = right = 0.f; return; }
    const int start = (int) position;
    const float fraction = (float) (position - start);
    const int loopStart = (int) zone.loopStart, loopEnd = (int) zone.loopEnd;
    const bool looped = loopEnd > loopStart + 8;
    const bool contiguous = start >= 1 && start + 2 < frames
        && (!looped || (position >= loopStart ? start - 1 >= loopStart && start + 2 < loopEnd
                                              : start + 2 < loopStart));
    const int sourceRight = std::min(1, zone.channels - 1);
    const auto* pcm = zone.pcm.data();
    const auto sampleAt = [&](int frame, int channel) {
        if (looped && frame < loopStart && position >= (double) loopStart) frame = loopEnd - (loopStart - frame);
        if (looped && frame >= loopEnd) frame = loopStart + (frame - loopEnd) % (loopEnd - loopStart);
        frame = std::clamp(frame, 0, frames - 1);
        return (float) pcm[(std::size_t) frame * (std::size_t) zone.channels + (std::size_t) channel] * NordSampleDecoder::pcmToFloat;
    };
    const float t2 = fraction * fraction, t3 = t2 * fraction;
    const auto interpolate = [&](int channel) {
        float y0, y1, y2, y3;
        if (contiguous) {
            const auto* tap = pcm + (std::size_t) (start - 1) * (std::size_t) zone.channels + (std::size_t) channel;
            y0 = (float) tap[0] * NordSampleDecoder::pcmToFloat;
            y1 = (float) tap[zone.channels] * NordSampleDecoder::pcmToFloat;
            y2 = (float) tap[2 * zone.channels] * NordSampleDecoder::pcmToFloat;
            y3 = (float) tap[3 * zone.channels] * NordSampleDecoder::pcmToFloat;
        } else {
            y0 = sampleAt(start - 1, channel); y1 = sampleAt(start, channel);
            y2 = sampleAt(start + 1, channel); y3 = sampleAt(start + 2, channel);
        }
        return 0.5f * ((2.0f * y1) + (-y0 + y2) * fraction + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
                       + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
    };
    left = interpolate(0);
    right = sourceRight == 0 ? left : interpolate(sourceRight);
}

Voice& allocateVoice(Layer& layer, int budget)
{
    const int limit = std::clamp(budget, 1, maxVoices);
    Voice* candidate = &layer.voices[0];
    const auto rank = [](const Voice& v) { return v.released ? 0 : v.waitingForSustain ? 1 : 2; };
    for (int i = 0; i < limit; ++i) {
        auto& v = layer.voices[(std::size_t) i];
        if (!v.active) return v;
        if (rank(v) < rank(*candidate) || (rank(v) == rank(*candidate) && v.serial < candidate->serial))
            candidate = &v;
    }
    return *candidate;
}
} // namespace

bool load(const std::string& path, std::uint64_t budget, std::shared_ptr<Instrument>& destination, std::string& error)
{
    const auto started = std::chrono::steady_clock::now();
    auto instrument = std::make_shared<Instrument>();
    juce::File file(path);
    const auto lower = [&] { std::string s = path; for (auto& c : s) c = (char) std::tolower((unsigned char) c); return s; }();
    const bool legacy = lower.size() > 5 && (lower.compare(lower.size() - 5, 5, ".nsmp") == 0
                                             || (lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".nsmp3") == 0));
    const auto result = legacy ? NordLegacySampleDecoder::decodeFile(file, *instrument, budget)
                               : NordSampleDecoder::decode(file, *instrument, budget);
    if (result.failed()) { error = result.getErrorMessage().toStdString(); return false; }
    if (instrument->zones.empty()) { error = "Instrumento Nord sem zonas de áudio"; return false; }
    prepareLoopCrossfades(*instrument);
    destination = std::move(instrument);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    loadInfo = "decodificado " + std::to_string((long long) ms) + " ms";
    return true;
}

std::string lastLoadInfo() { return loadInfo; }

std::shared_ptr<Instrument> makeTestToneInstrument()
{
    auto instrument = std::make_shared<Instrument>();
    instrument->gain.fill(1.0f);
    NordSampleDecoder::Zone zone;
    zone.root = 60; zone.low = 0; zone.high = 127; zone.velocityLow = 0; zone.velocityHigh = 127;
    zone.channels = 1; zone.gain = 0.5f;
    const int frames = (int) NordSampleDecoder::sampleRate * 2;
    zone.pcm.resize((std::size_t) frames);
    for (int i = 0; i < frames; ++i)
        zone.pcm[(std::size_t) i] = NordSampleDecoder::floatToPcm(
            0.5f * (float) std::sin(2.0 * 3.14159265358979 * 261.6256 * i / NordSampleDecoder::sampleRate));
    zone.loopStart = 4000; zone.loopEnd = (std::size_t) frames - 1;
    instrument->zones.push_back(std::move(zone));
    prepareLoopCrossfades(*instrument);
    return instrument;
}

void noteOn(Layer& layer, int note, int velocity, int channel, bool sustain, int voiceBudget,
            float attackSeconds, float releaseSeconds)
{
    if (!layer.instrument || note < 0 || note > 127 || velocity < 1) return;
    const auto& instrument = *layer.instrument;
    const int vel = std::clamp(velocity, 1, 127);
    const int zoneIndex = findZone(instrument, note, vel);
    if (zoneIndex < 0) return;
    const auto& zone = instrument.zones[(std::size_t) zoneIndex];
    if (zone.frames() == 0) return;

    // Re-strike under the pedal: the voice the pedal still holds for this key fades out quickly.
    if (sustain) {
        const float fast = releaseCoefficientFor(restrikeFadeSeconds, 44100);
        const float normal = releaseCoefficientFor(releaseSeconds, 44100);
        for (auto& prior : layer.voices)
            if (prior.active && prior.waitingForSustain && !prior.keyDown && prior.note == note && prior.channel == channel) {
                prior.waitingForSustain = false;
                prior.released = true;
                prior.releaseCoefficient = std::min(normal, fast);
            }
    }

    auto& voice = allocateVoice(layer, voiceBudget);
    voice = {};
    voice.active = true;
    voice.note = note;
    voice.channel = channel;
    voice.zone = zoneIndex;
    voice.keyDown = true;
    voice.serial = ++layer.serial;
    const double cents = instrument.detune[(std::size_t) note];
    voice.increment = std::pow(2.0, (note - zone.root + cents / 100.0) / 12.0) * NordSampleDecoder::sampleRate;
    voice.amplitude = (vel / 127.0f) * zone.gain * std::max(0.0f, instrument.gain[(std::size_t) note]);
    voice.attack = attackSeconds <= 0.f ? 1.f : 0.f;
    voice.attackStep = attackSeconds <= 0.f ? 1.f : static_cast<float>(1.0 / std::max(1.0, attackSeconds * 44100.0));
    voice.releaseGain = 1.f;
}

void noteOff(Layer& layer, int note, int channel, bool sustain, float releaseSeconds)
{
    for (auto& voice : layer.voices)
        if (voice.active && voice.keyDown && voice.note == note && voice.channel == channel) {
            voice.keyDown = false;
            if (sustain) voice.waitingForSustain = true;
            else { voice.released = true; voice.releaseCoefficient = releaseCoefficientFor(releaseSeconds, 44100); }
        }
}

void pedalUp(Layer& layer, int channel, float releaseSeconds)
{
    for (auto& voice : layer.voices)
        if (voice.active && voice.waitingForSustain && voice.channel == channel) {
            voice.waitingForSustain = false;
            voice.released = true;
            voice.releaseCoefficient = releaseCoefficientFor(releaseSeconds, 44100);
        }
}

void render(Layer& layer, float* stereo, int frames, int outputRate, int voiceBudget, float decayMs, float sustainPercent)
{
    if (!layer.instrument || stereo == nullptr) return;
    const auto& instrument = *layer.instrument;
    const int limit = std::clamp(voiceBudget, 0, maxVoices);
    const float sustainTarget = std::clamp(sustainPercent * 0.01f, 0.f, 1.f);
    const float decayStep = decayMs > 0.f
        ? static_cast<float>((1.0f - sustainTarget) / std::max(1.0, decayMs * 0.001 * outputRate)) : 0.f;
    const double pitchScale = 44100.0 / outputRate; // voice increments are computed against 44.1 kHz output
    for (int vi = 0; vi < limit; ++vi) {
        auto& voice = layer.voices[(std::size_t) vi];
        if (!voice.active) continue;
        if (voice.zone < 0 || voice.zone >= (int) instrument.zones.size()) { voice = {}; continue; }
        const auto& zone = instrument.zones[(std::size_t) voice.zone];
        const int zoneFrames = (int) zone.frames();
        const bool looped = zone.loopEnd > zone.loopStart + 8;
        const double step = voice.increment / 44100.0 * pitchScale;
        for (int f = 0; f < frames; ++f) {
            if (looped && voice.position >= zone.loopEnd)
                voice.position = zone.loopStart + std::fmod(voice.position - zone.loopEnd, (double) (zone.loopEnd - zone.loopStart));
            if (voice.position >= zoneFrames) { voice = {}; break; }
            const float amplitude = voice.amplitude * voice.attack * voice.decayGain * voice.releaseGain;
            float left, right;
            readStereoSample(zone, voice.position, zoneFrames, left, right);
            stereo[(std::size_t) f * 2] += left * amplitude;
            stereo[(std::size_t) f * 2 + 1] += right * amplitude;
            voice.attack = std::min(1.f, voice.attack + voice.attackStep);
            if (!voice.released && voice.attack >= 1.f && decayStep > 0.f)
                voice.decayGain = std::max(sustainTarget, voice.decayGain - decayStep);
            voice.position += step;
            if (voice.released) {
                voice.releaseGain *= voice.releaseCoefficient;
                if (voice.releaseGain < envelopeFloor) { voice = {}; break; }
            }
        }
    }
}

int activeVoices(const Layer& layer)
{
    int count = 0;
    for (const auto& voice : layer.voices) if (voice.active) ++count;
    return count;
}
}
