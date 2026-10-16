#include "NordSampleDecoder.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

extern "C"
{
typedef bool ZoneCallback(void*, std::uint8_t, std::uint8_t, std::uint8_t,
                          std::uint8_t, std::uint8_t, std::uint16_t,
                          std::uint32_t, float, std::uint32_t, std::uint32_t,
                          const std::int16_t*, std::size_t);

int classic_nord_decode_nsmp4(const char* path, void* context, ZoneCallback* callback,
                              char* errorBuffer, std::size_t errorCapacity);
}

#if defined(CLASSIC_PLAYER_HAS_NORD_NSMP4_DECODER)
namespace
{
struct DecodeContext
{
    NordPianoDecoder::Instrument& instrument;
    bool channelsSet = false;
    bool failed = false;
};

extern "C" bool appendZone(void* opaque, std::uint8_t rootNote, std::uint8_t lowNote,
                           std::uint8_t highNote, std::uint8_t lowVelocity,
                           std::uint8_t highVelocity, std::uint16_t channels,
                           std::uint32_t sampleRate, float gainDb, std::uint32_t loopStart,
                           std::uint32_t loopEnd, const std::int16_t* samples,
                           std::size_t sampleCount)
{
    auto& context = *static_cast<DecodeContext*>(opaque);
    auto& instrument = context.instrument;
    if (context.failed || samples == nullptr || sampleCount == 0
        || channels < 1 || channels > 2 || sampleRate == 0
        || rootNote > 127 || lowNote > highNote || highNote > 127
        || lowVelocity > highVelocity || highVelocity > 127
        || sampleCount % channels != 0
        || instrument.strokes.size() >= static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max()))
    {
        context.failed = true;
        return false;
    }

    if (context.channelsSet && instrument.channels != static_cast<int>(channels))
    {
        context.failed = true;
        return false;
    }
    context.channelsSet = true;
    instrument.channels = static_cast<int>(channels);
    instrument.sampleRateHz = static_cast<double>(sampleRate);

    NordPianoDecoder::Stroke stroke;
    stroke.rootNote = rootNote;
    stroke.bank = 0;
    stroke.levelGain = std::pow(10.0f, juce::jlimit(-60.0f, 24.0f, gainDb) / 20.0f);
    if (loopStart != std::numeric_limits<std::uint32_t>::max() && loopEnd > loopStart
        && loopEnd <= sampleCount / channels)
    {
        stroke.loopStartFrame = loopStart;
        stroke.loopEndFrame = loopEnd;
    }
    stroke.interleavedPcm.assign(samples, samples + sampleCount);

    const auto strokeIndex = static_cast<std::int16_t>(instrument.strokes.size());
    instrument.strokes.push_back(std::move(stroke));
    for (int note = lowNote; note <= highNote; ++note)
        for (int velocity = lowVelocity; velocity <= highVelocity; ++velocity)
    {
        const auto noteIndex = static_cast<std::size_t>(note);
        const auto velocityIndex = static_cast<std::size_t>(velocity);
        if (instrument.zoneMap[noteIndex][velocityIndex] >= 0)
        {
            context.failed = true;
            return false;
        }
        instrument.zoneMap[noteIndex][velocityIndex] = strokeIndex;
        instrument.keyMap[noteIndex] = rootNote;
    }
    return true;
}
}

juce::Result NordSampleDecoder::decodeFile(const juce::File& file,
                                            NordPianoDecoder::Instrument& destination)
{
    if (!file.existsAsFile() || !file.hasFileExtension(".nsmp4"))
        return juce::Result::fail("Selecione um sample Nord .nsmp4 válido.");
    if (file.getSize() > 350ll * 1024ll * 1024ll)
        return juce::Result::fail("O sample Nord excede o limite de 350 MB.");

    auto decoded = NordPianoDecoder::Instrument{};
    decoded.keyMap.fill(0xff);
    for (auto& velocityMap : decoded.zoneMap) velocityMap.fill(-1);
    decoded.applyPianoTimbre = false;
    decoded.finitePianoDecay = false;
    decoded.velocityScale = 0;
    decoded.name = file.getFileNameWithoutExtension();
    DecodeContext context { decoded };
    std::array<char, 512> errorBuffer {};
    const auto path = file.getFullPathName().toStdString();
    const auto status = classic_nord_decode_nsmp4(path.c_str(), &context, appendZone,
                                                   errorBuffer.data(), errorBuffer.size());
    if (status != 0 || context.failed || decoded.strokes.empty())
    {
        const auto message = juce::String::fromUTF8(errorBuffer.data()).trim();
        return juce::Result::fail(message.isNotEmpty()
            ? message : "Não foi possível decodificar o sample Nord 4.");
    }

    destination = std::move(decoded);
    return juce::Result::ok();
}
#else
juce::Result NordSampleDecoder::decodeFile(const juce::File& file,
                                            NordPianoDecoder::Instrument&)
{
    juce::ignoreUnused(file);
    return juce::Result::fail("A leitura de samples .nsmp4 não está disponível nesta plataforma.");
}
#endif
