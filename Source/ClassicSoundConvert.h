#pragma once

#include "ClassicSoundFile.h"
#include "ClassicSf2Export.h"
#include "NordLegacySampleDecoder.h"

// NPNO / NSMP -> .ckp / .cks, or an NPNO / .ckp piano -> .sf2. Used by the Classic Converter app.
namespace ClassicSoundConvert
{
enum class Format { classic, soundFont };

// The extension of the file this source becomes, or empty when it cannot be converted to that format.
inline juce::String outputExtension(const juce::File& source, Format format = Format::classic)
{
    if (format == Format::soundFont)
        return source.hasFileExtension(".npno;.ckp") ? juce::String(".sf2") : juce::String();
    if (source.hasFileExtension(".npno")) return ".ckp";
    if (source.hasFileExtension(".nsmp;.nsmp3;.nsmp4")) return ".cks";
    return {};
}

// Converts one file into `folder`. Existing files are never overwritten: a numbered name is used instead.
// The written file is read back and every audio block checked before it is published.
inline juce::Result convert(const juce::File& source, const juce::File& folder, juce::File& created,
                            Format format = Format::classic)
{
    created = juce::File();
    const auto extension = outputExtension(source, format);
    if (!source.existsAsFile() || extension.isEmpty())
        return ClassicSoundFile::failure(format == Format::soundFont
            ? "Para SoundFont, escolha um piano .npno ou .ckp."
            : "Escolha um arquivo .npno, .nsmp, .nsmp3 ou .nsmp4.");
    if (const auto made = folder.createDirectory(); made.failed()) return made;
    const auto destination = folder.getNonexistentChildFile(source.getFileNameWithoutExtension(), extension, true);

    if (extension == ".sf2")
    {
        ClassicSf2Export::Writer writer(destination);
        NordPianoDecoder::Instrument piano;
        const auto result = source.hasFileExtension(".ckp")
            ? ClassicSoundFile::readPiano(source, piano, 0, &writer)
            : NordPianoDecoder::decodeFile(source, piano, 0, &writer);
        if (result.failed()) return result;
        if (!writer.completed()) return ClassicSoundFile::damaged();
    }
    else if (extension == ".ckp")
    {
        ClassicSoundFile::PianoWriter writer(destination);
        NordPianoDecoder::Instrument piano;
        // A zero memory budget makes the decoder hand over one stroke at a time.
        if (const auto result = NordPianoDecoder::decodeFile(source, piano, 0, &writer); result.failed())
            return result;
        if (!writer.completed()) return ClassicSoundFile::damaged();
        NordPianoDecoder::Instrument check;
        if (const auto result = ClassicSoundFile::readPiano(writer.temporary(), check, 0, nullptr, true); result.failed())
            return result;
        if (const auto result = writer.publish(); result.failed()) return result;
    }
    else
    {
        NordSampleDecoder::Instrument sample;
        const auto decoded = source.hasFileExtension(".nsmp4")
            ? NordSampleDecoder::decode(source, sample)
            : NordLegacySampleDecoder::decodeFile(source, sample);
        if (decoded.failed()) return decoded;
        ClassicSoundFile::PendingFile pending;
        if (const auto result = ClassicSoundFile::writeSample(destination, sample, pending); result.failed())
            return result;
        NordSampleDecoder::Instrument check;
        if (const auto result = ClassicSoundFile::readSample(pending.temporary(), check, 0, true); result.failed())
            return result;
        if (const auto result = pending.publish(); result.failed()) return result;
    }
    created = destination;
    return juce::Result::ok();
}
}
