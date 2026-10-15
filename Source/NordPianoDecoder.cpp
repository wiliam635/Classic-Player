#include "NordPianoDecoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace
{
constexpr size_t cbinHeaderBytes = 44;
constexpr size_t minimumBodyBytes = 0x732;
constexpr size_t keyMapOffset = 0x8c;
constexpr size_t fineTuneOffset = 0x18c;
constexpr size_t streamVersionOffset = 0x04;
constexpr size_t echoedVersionOffset = 0x61c;
constexpr size_t channelsOffset = 0x61e;
constexpr size_t strokeCountOffset = 0x620;
constexpr size_t rootCountsOffset = 0x622;
constexpr size_t directoryOffset = 0x732;
constexpr size_t strokeRecordBytes = 118;
constexpr size_t audioAlignmentBias = 192;
constexpr size_t wordsPerBlock = 511;
constexpr size_t overlapFrames = 64;
constexpr size_t maxDecodedAudioBytes = 512u * 1024u * 1024u;

std::uint16_t readBigEndian16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t readBigEndian32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24)
         | (static_cast<std::uint32_t>(data[1]) << 16)
         | (static_cast<std::uint32_t>(data[2]) << 8)
         | static_cast<std::uint32_t>(data[3]);
}

std::uint32_t readLittleEndian32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint32_t crc32IsoHdlc(const std::uint8_t* data, size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

juce::String fixedAscii(const std::uint8_t* data, size_t size)
{
    size_t length = 0;
    while (length < size && data[length] != 0)
        ++length;

    return juce::String::fromUTF8(reinterpret_cast<const char*>(data), static_cast<int>(length)).trim();
}

struct BitFields
{
    const std::uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t nextWord = 0;
    std::uint64_t reservoir = 0;
    unsigned bitsHeld = 0;

    bool take(unsigned width, std::int32_t& result)
    {
        while (bitsHeld < width)
        {
            const auto at = nextWord * 2;
            if (at + 2 > length)
                return false;

            const auto word = readBigEndian16(bytes + at);
            reservoir |= static_cast<std::uint64_t>(word) << bitsHeld;
            bitsHeld += 16;
            ++nextWord;
        }

        const auto mask = (std::uint64_t{ 1 } << width) - 1;
        auto value = static_cast<std::int64_t>(reservoir & mask);
        reservoir >>= width;
        bitsHeld -= width;
        const auto signBit = std::int64_t{ 1 } << (width - 1);
        if ((value & signBit) != 0)
            value -= std::int64_t{ 1 } << width;
        result = static_cast<std::int32_t>(value);
        return true;
    }
};

std::int64_t predict(std::int32_t residual,
                     std::array<std::int64_t, 4>& history,
                     unsigned order)
{
    static constexpr std::int32_t coefficients[5][4] = {
        { 0, 0, 0, 0 },
        { -1, 0, 0, 0 },
        { -2, 1, 0, 0 },
        { -3, 3, -1, 0 },
        { -4, 6, -4, 1 }
    };

    std::int64_t predicted = 0;
    for (unsigned i = 0; i < order; ++i)
        predicted += static_cast<std::int64_t>(coefficients[order][i]) * history[i];

    const auto sample = static_cast<std::int64_t>(residual) - predicted;
    for (size_t i = history.size() - 1; i > 0; --i)
        history[i] = history[i - 1];
    history[0] = sample;
    return sample;
}

std::int16_t saturateToInt16(std::int64_t value)
{
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
}

juce::Result decodeStroke(const std::uint8_t* audio,
                          size_t audioBytes,
                          std::uint16_t channels,
                          std::uint32_t expectedFrames,
                          std::uint16_t blockCount,
                          const std::uint8_t* record,
                          std::vector<std::int16_t>& pcm)
{
    if (blockCount == 0 || expectedFrames == 0)
        return juce::Result::fail("O arquivo Nord declara uma gravação vazia.");

    const auto blockBytes = wordsPerBlock * 2 * static_cast<size_t>(channels);
    if (blockCount > std::numeric_limits<size_t>::max() / blockBytes
        || audioBytes != static_cast<size_t>(blockCount) * blockBytes)
        return juce::Result::fail("O tamanho de áudio declarado para uma nota Nord não confere.");

    const auto longestBlockFrames = (8 * (blockBytes - 2)) / channels;
    const auto maximumOwnedFrames = static_cast<std::uint64_t>(blockCount)
                                  * (longestBlockFrames - overlapFrames);
    if (expectedFrames > maximumOwnedFrames
        || expectedFrames > std::numeric_limits<size_t>::max() / channels)
        return juce::Result::fail("A duração declarada para uma nota Nord é inválida.");

    std::array<std::array<std::int64_t, 4>, 2> history {};
    for (size_t channel = 0; channel < channels; ++channel)
        for (size_t seed = 0; seed < 4; ++seed)
        {
            const auto at = 12 + (channel * 4 + seed) * 2;
            history[channel][3 - seed] = static_cast<std::int16_t>(readBigEndian16(record + at));
        }

    pcm.clear();
    pcm.reserve(static_cast<size_t>(expectedFrames) * channels);
    std::vector<std::int64_t> previousTail;
    size_t outputFrames = 0;

    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
    {
        const auto* raw = audio + blockIndex * blockBytes;
        const auto header = readBigEndian16(raw);
        const auto width = static_cast<unsigned>(header & 0x1fu);
        const auto order = static_cast<unsigned>((header >> 5) & 0x07u);
        if (width < 1 || width > 16 || order > 4)
            return juce::Result::fail("O arquivo Nord contém um bloco de áudio não suportado.");

        const auto frames = (8 * (blockBytes - 2)) / (width * channels);
        if (frames < overlapFrames + 4)
            return juce::Result::fail("Um bloco de áudio Nord é curto demais para a sobreposição.");

        BitFields fields { raw + 2, blockBytes - 2 };
        std::vector<std::int64_t> decoded(frames * channels);
        for (size_t frame = 0; frame < frames; ++frame)
            for (size_t channel = 0; channel < channels; ++channel)
            {
                std::int32_t residual = 0;
                if (!fields.take(width, residual))
                    return juce::Result::fail("Os dados de áudio Nord terminaram antes do esperado.");
                decoded[frame * channels + channel] = predict(residual, history[channel], order);
            }

        const auto ownedFrames = frames - overlapFrames;
        if (!previousTail.empty())
        {
            for (size_t frame = 0; frame < overlapFrames; ++frame)
                for (size_t channel = 0; channel < channels; ++channel)
                {
                    const auto index = frame * channels + channel;
                    if (decoded[index] != previousTail[index])
                        return juce::Result::fail("A sobreposição entre blocos do piano Nord não confere.");
                }
        }

        if (ownedFrames > static_cast<size_t>(expectedFrames) - outputFrames)
            return juce::Result::fail("Os blocos Nord excedem a duração declarada para a nota.");

        for (size_t frame = 0; frame < ownedFrames; ++frame)
            for (size_t channel = 0; channel < channels; ++channel)
                pcm.push_back(saturateToInt16(decoded[frame * channels + channel]));
        outputFrames += ownedFrames;

        previousTail.assign(decoded.begin() + static_cast<std::ptrdiff_t>(ownedFrames * channels),
                            decoded.end());

        for (size_t channel = 0; channel < channels; ++channel)
            for (size_t i = 0; i < 4; ++i)
                history[channel][i] = decoded[(ownedFrames - 1 - i) * channels + channel];
    }

    if (outputFrames != expectedFrames)
        return juce::Result::fail("A duração decodificada não corresponde ao registro do piano Nord.");

    return juce::Result::ok();
}

juce::Result fail(juce::String message)
{
    return juce::Result::fail(std::move(message));
}
}

juce::Result NordPianoDecoder::decodeFile(const juce::File& file, Instrument& destination)
{
    destination = {};

    if (!file.existsAsFile())
        return fail("Arquivo Nord não encontrado.");
    if (file.getSize() < static_cast<juce::int64>(cbinHeaderBytes + minimumBodyBytes))
        return fail("O arquivo é pequeno demais para conter um piano Nord válido.");
    if (file.getSize() > static_cast<juce::int64>(512u * 1024u * 1024u))
        return fail("O arquivo Nord excede o limite de 512 MB para importação.");

    juce::MemoryBlock fileData;
    if (!file.loadFileAsData(fileData))
        return fail("Não foi possível ler o arquivo Nord.");

    const auto* bytes = static_cast<const std::uint8_t*>(fileData.getData());
    const auto fileBytes = static_cast<size_t>(fileData.getSize());
    if (fileBytes < cbinHeaderBytes + minimumBodyBytes
        || std::memcmp(bytes, "CBIN", 4) != 0)
        return fail("O arquivo não possui um cabeçalho CBIN válido.");
    if (readLittleEndian32(bytes + 4) != 1)
        return fail("Esta versão do importador aceita arquivos Nord CBIN de geração 1.");
    if (std::memcmp(bytes + 8, "npno", 4) != 0)
        return fail("O arquivo CBIN não é uma biblioteca de piano .npno.");

    const auto* body = bytes + cbinHeaderBytes;
    const auto bodyBytes = fileBytes - cbinHeaderBytes;
    if (crc32IsoHdlc(body, bodyBytes) != readLittleEndian32(bytes + 24))
        return fail("A verificação CRC do arquivo Nord falhou; o arquivo pode estar corrompido.");
    if (std::memcmp(body, "CNSP", 4) != 0)
        return fail("O corpo do arquivo não contém um fluxo CNSP reconhecido.");

    const auto streamVersion = readBigEndian16(body + streamVersionOffset);
    if (streamVersion != 0x450 && streamVersion != 0x464)
        return fail("A versão do fluxo de piano Nord ainda não é suportada.");
    if (readBigEndian16(body + echoedVersionOffset) != streamVersion)
        return fail("Os campos de versão do fluxo de piano Nord não coincidem.");

    const auto channels = readBigEndian16(body + channelsOffset);
    if (channels < 1 || channels > 2)
        return fail("O arquivo Nord declara um número de canais não suportado.");

    const auto strokeCount = readBigEndian16(body + strokeCountOffset);
    std::array<std::uint16_t, 128> rootCounts {};
    std::uint64_t summedStrokes = 0;
    for (size_t root = 0; root < rootCounts.size(); ++root)
    {
        rootCounts[root] = readBigEndian16(body + rootCountsOffset + root * 2);
        summedStrokes += rootCounts[root];
    }
    if (summedStrokes != strokeCount)
        return fail("A tabela de notas Nord não corresponde à quantidade de gravações.");

    const auto directoryEnd = directoryOffset + static_cast<size_t>(strokeCount) * strokeRecordBytes;
    if (directoryEnd < directoryOffset || directoryEnd > bodyBytes)
        return fail("O diretório de gravações Nord está truncado.");

    const auto blockBytes = wordsPerBlock * 2 * static_cast<size_t>(channels);
    const auto firstAudio = ((directoryEnd + audioAlignmentBias + blockBytes - 1) / blockBytes)
                          * blockBytes - audioAlignmentBias;
    if (firstAudio > bodyBytes)
        return fail("O alinhamento do áudio Nord excede o tamanho do arquivo.");
    if (std::any_of(body + directoryEnd, body + firstAudio,
                    [] (std::uint8_t value) { return value != 0; }))
        return fail("O preenchimento antes do áudio Nord contém dados inesperados.");

    Instrument parsed;
    parsed.streamVersion = streamVersion;
    parsed.channels = channels;
    parsed.gainTenthsDb = static_cast<std::int8_t>(body[0x40c]);
    std::copy_n(body + keyMapOffset, parsed.keyMap.size(), parsed.keyMap.begin());
    for (size_t key = 0; key < parsed.fineTune.size(); ++key)
        parsed.fineTune[key] = static_cast<std::int8_t>(body[fineTuneOffset + key]);

    const auto combinedName = fixedAscii(body + 0x1c, 0x20);
    const auto separator = combinedName.indexOfChar('#');
    parsed.name = separator >= 0 ? combinedName.substring(0, separator).trim()
                                 : combinedName.trim();
    parsed.voicing = separator >= 0 ? combinedName.substring(separator + 1).trim()
                                    : juce::String{};
    if (streamVersion == 0x464)
    {
        const auto longName = fixedAscii(body + 0x3c, 0x20);
        if (longName.isNotEmpty()) parsed.name = longName;
        const auto voicing = fixedAscii(body + 0x5c, 0x20);
        if (voicing.isNotEmpty()) parsed.voicing = voicing;
    }

    parsed.strokes.reserve(strokeCount);
    std::array<bool, 128> rootsWithAttack {};
    auto audioCursor = firstAudio;
    size_t recordIndex = 0;
    std::uint64_t totalDecodedBytes = 0;
    int highestAttackLayer = 0;
    bool hasSignedTrim = false;

    for (size_t root = 0; root < rootCounts.size(); ++root)
        for (std::uint16_t strokeNumber = 0; strokeNumber < rootCounts[root]; ++strokeNumber)
        {
            const auto* record = body + directoryOffset + recordIndex * strokeRecordBytes;
            const auto start = readBigEndian32(record);
            const auto blockCount = readBigEndian16(record + 10);
            const auto expectedFrames = readBigEndian32(record + 6);
            const auto bank = record[4];
            const auto layer = record[5];
            const auto trimDb = readBigEndian16(record + 0x34);

            if (start != audioCursor)
                return fail("O deslocamento de uma gravação Nord não corresponde ao diretório.");
            const auto audioBytes = static_cast<size_t>(blockCount) * blockBytes;
            if (blockCount != 0 && audioBytes / blockCount != blockBytes)
                return fail("O tamanho de uma gravação Nord excede os limites suportados.");
            if (audioCursor > bodyBytes || audioBytes > bodyBytes - audioCursor)
                return fail("Os dados de áudio de uma gravação Nord estão truncados.");

            if (bank <= 2)
            {
                const auto decodedBytes = static_cast<std::uint64_t>(expectedFrames)
                                        * channels * sizeof(std::int16_t);
                if (decodedBytes > maxDecodedAudioBytes - totalDecodedBytes)
                    return fail("O áudio decodificado ultrapassaria o limite de memória de 512 MB.");

                Stroke stroke;
                stroke.rootNote = static_cast<std::uint8_t>(root);
                stroke.bank = bank;
                stroke.velocityLayer = layer;
                stroke.trimDb = trimDb;
                if (bank != 2)
                {
                    const auto loopStart = readBigEndian32(record + 0x1c);
                    const auto loopLength = readBigEndian32(record + 0x20);
                    if (loopStart > 0 && loopLength > 0
                        && loopStart < expectedFrames
                        && loopLength <= expectedFrames - loopStart)
                    {
                        stroke.loopStartFrame = loopStart;
                        stroke.loopEndFrame = loopStart + loopLength;
                    }
                }
                hasSignedTrim = hasSignedTrim || trimDb >= 0x8000;
                if (bank == 0)
                    highestAttackLayer = std::max(highestAttackLayer, static_cast<int>(layer));
                const auto result = decodeStroke(body + audioCursor, audioBytes, channels,
                                                 expectedFrames, blockCount, record,
                                                 stroke.interleavedPcm);
                if (result.failed())
                    return result;
                totalDecodedBytes += decodedBytes;
                if (bank == 0)
                    rootsWithAttack[root] = true;
                parsed.strokes.push_back(std::move(stroke));
            }

            audioCursor += audioBytes;
            ++recordIndex;
        }

    if (audioCursor != bodyBytes)
        return fail("O fluxo CNSP contém bytes extras após as gravações de áudio.");

    // Some newer pianos use signed tenths of a dB and an extended velocity scale.
    parsed.signedTenthsDbTrim = hasSignedTrim;
    if (highestAttackLayer > 30)
        parsed.velocityScale = std::max(31, (highestAttackLayer * 31 + 26) / 27);

    for (const auto root : parsed.keyMap)
        if (root != 0xff && (root >= rootsWithAttack.size() || !rootsWithAttack[root]))
            return fail("A tabela de teclas aponta para uma nota sem gravação de ataque.");

    destination = std::move(parsed);
    return juce::Result::ok();
}
