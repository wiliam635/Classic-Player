#include "NordSampleExport.h"
#include "NordLegacySampleDecoder.h"
#include "NordSampleDecoder.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

juce::Result NordSampleExport::exportZonesAsWav(const juce::File& source,
                                                const juce::File& destinationFolder,
                                                int& exportedCount)
{
    exportedCount = 0;
    if (!source.existsAsFile() || !source.hasFileExtension(".nsmp;.nsmp3;.nsmp4"))
        return juce::Result::fail("Selecione uma amostra Nord válida.");
    if (!destinationFolder.isDirectory())
        return juce::Result::fail("Selecione uma pasta de destino válida.");

    NordSampleDecoder::Instrument instrument;
    const auto decoded = source.hasFileExtension(".nsmp4")
        ? NordSampleDecoder::decode(source, instrument)
        : NordLegacySampleDecoder::decodeFile(source, instrument);
    if (decoded.failed()) return decoded;

    juce::WavAudioFormat wav;
    for (const auto& zone : instrument.zones)
    {
        if (zone.channels < 1 || zone.channels > 2 || zone.frames() == 0
            || zone.frames() > static_cast<size_t>(std::numeric_limits<int>::max()))
            return juce::Result::fail("Uma zona Nord tem canais ou duração inválidos.");
        const auto label = source.getFileNameWithoutExtension()
            + "_root" + juce::String(zone.root)
            + "_keys" + juce::String(zone.low) + "-" + juce::String(zone.high)
            + "_vel" + juce::String(zone.velocityLow) + "-" + juce::String(zone.velocityHigh);
        const auto destination = destinationFolder.getNonexistentChildFile(label, ".wav", false);
        auto stream = std::unique_ptr<juce::FileOutputStream>(destination.createOutputStream());
        if (stream == nullptr || stream->failedToOpen())
            return juce::Result::fail("Não foi possível criar " + destination.getFileName());
        auto writer = std::unique_ptr<juce::AudioFormatWriter>(
            wav.createWriterFor(stream.release(), NordSampleDecoder::sampleRate,
                                static_cast<unsigned int>(zone.channels), 24, {}, 0));
        if (writer == nullptr)
        {
            destination.deleteFile();
            return juce::Result::fail("Não foi possível iniciar o arquivo WAV.");
        }

        constexpr int blockSize = 4096;
        juce::AudioBuffer<float> block(zone.channels, blockSize);
        for (size_t frame = 0; frame < zone.frames(); frame += blockSize)
        {
            const auto count = static_cast<int>(std::min<size_t>(blockSize, zone.frames() - frame));
            for (int channel = 0; channel < zone.channels; ++channel)
                for (int i = 0; i < count; ++i)
                {
                    const auto sample = zone.pcm[(frame + static_cast<size_t>(i))
                                                 * static_cast<size_t>(zone.channels)
                                                 + static_cast<size_t>(channel)];
                    block.setSample(channel, i, std::isfinite(sample)
                        ? juce::jlimit(-1.0f, 1.0f, sample) : 0.0f);
                }
            if (!writer->writeFromAudioSampleBuffer(block, 0, count))
            {
                writer.reset();
                destination.deleteFile();
                return juce::Result::fail("Falha ao gravar " + destination.getFileName());
            }
        }
        writer.reset();
        ++exportedCount;
    }
    return exportedCount > 0 ? juce::Result::ok()
                             : juce::Result::fail("A amostra Nord não contém zonas de áudio.");
}
