#include "NordBundleImporter.h"

#include <algorithm>
#include <array>

namespace
{
constexpr int maximumEntries = 4096;
constexpr juce::int64 maximumEntryBytes = 1024LL * 1024LL * 1024LL;
constexpr juce::int64 maximumExpandedBytes = 2LL * 1024LL * 1024LL * 1024LL;
constexpr const char* completionMarker = ".classic-player-bank-complete";
juce::CriticalSection bundleImportLock;

juce::String safeBankName(const juce::File& source)
{
    auto name = source.getFileNameWithoutExtension()
                    .retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()")
                    .trim();
    return name.isNotEmpty() ? name : "Nord Bank";
}

bool hasSupportedExtension(const juce::String& path)
{
    const auto lower = path.toLowerCase();
    return lower == "meta.xml" || lower.endsWith(".npno")
        || lower.endsWith(".ns4p") || lower.endsWith(".nsmp4");
}

bool normaliseAndValidatePath(const juce::String& rawPath, juce::String& normalised)
{
    normalised = rawPath.replaceCharacter('\\', '/');
    if (normalised.isEmpty() || normalised.length() > 2048
        || normalised.startsWithChar('/') || normalised.contains(":"))
        return false;

    while (normalised.endsWithChar('/'))
        normalised = normalised.dropLastCharacters(1);
    if (normalised.isEmpty()) return false;

    juce::StringArray components;
    components.addTokens(normalised, "/", "");
    components.removeEmptyStrings();
    if (components.isEmpty()) return false;

    for (const auto& component : components)
        if (component == "." || component == ".." || component.containsChar(':'))
            return false;

    normalised = components.joinIntoString("/");
    return true;
}

juce::File uniqueBankDirectory(const juce::File& root, const juce::String& baseName)
{
    auto candidate = root.getChildFile(baseName);
    for (int suffix = 2; candidate.exists(); ++suffix)
        candidate = root.getChildFile(baseName + " (" + juce::String(suffix) + ")");
    return candidate;
}

juce::File childForRelativePath(const juce::File& root, const juce::String& relativePath)
{
    juce::StringArray components;
    components.addTokens(relativePath, "/", "");
    components.removeEmptyStrings();
    auto child = root;
    for (const auto& component : components)
        child = child.getChildFile(component);
    return child;
}

juce::Result extractEntry(juce::ZipFile& zip, int index,
                          const juce::ZipFile::ZipEntry& entry,
                          const juce::File& destination,
                          std::array<char, 64 * 1024>& buffer)
{
    if (const auto createParent = destination.getParentDirectory().createDirectory();
        createParent.failed())
        return createParent;

    std::unique_ptr<juce::InputStream> input(zip.createStreamForEntry(index));
    if (input == nullptr)
        return juce::Result::fail("Nao foi possivel abrir uma entrada compactada.");
    auto output = destination.createOutputStream();
    if (output == nullptr || !output->openedOk())
        return juce::Result::fail("Nao foi possivel gravar os arquivos na biblioteca Nord.");

    juce::int64 written = 0;
    for (;;)
    {
        const auto bytesRead = input->read(buffer.data(), static_cast<int>(buffer.size()));
        if (bytesRead <= 0) break;
        written += bytesRead;
        if (written > entry.uncompressedSize || written > maximumEntryBytes)
            return juce::Result::fail("Uma entrada expandiu alem do tamanho declarado no ZIP.");
        if (!output->write(buffer.data(), static_cast<size_t>(bytesRead)))
            return juce::Result::fail("Falha ao gravar um arquivo do banco Nord.");
    }

    if (written != entry.uncompressedSize || output->getStatus().failed())
        return juce::Result::fail("Uma entrada do bundle foi lida ou gravada incompletamente.");
    return juce::Result::ok();
}

bool belongsToCompletedBank(const juce::File& file, const juce::File& root)
{
    auto bankDirectory = file.getParentDirectory();
    while (bankDirectory.exists() && bankDirectory.getParentDirectory() != root)
    {
        const auto parent = bankDirectory.getParentDirectory();
        if (parent == bankDirectory) return false;
        bankDirectory = parent;
    }
    return bankDirectory.getParentDirectory() == root
        && bankDirectory.getChildFile(completionMarker).existsAsFile();
}
}

juce::Result NordBundleImporter::importBundle(const juce::File& source,
                                               const juce::File& libraryRoot,
                                               ImportSummary& summary)
{
    const juce::ScopedLock lock(bundleImportLock);
    summary = {};
    if (!source.existsAsFile()
        || !source.getFileExtension().equalsIgnoreCase(".ns4pbundle"))
        return juce::Result::fail("Selecione um arquivo .ns4pbundle valido.");

    if (source.getSize() <= 0 || source.getSize() > maximumExpandedBytes)
        return juce::Result::fail("O arquivo do banco esta vazio ou excede o limite de importacao.");

    juce::ZipFile zip(source);
    const auto entryCount = zip.getNumEntries();
    if (entryCount <= 0 || entryCount > maximumEntries)
        return juce::Result::fail("O bundle nao e um ZIP valido ou possui entradas demais.");

    juce::int64 totalExpandedBytes = 0;
    bool hasManifest = false;
    int pianoCount = 0;
    int programCount = 0;
    int sampleCount = 0;

    // Validate the complete archive before creating files. We reject links and
    // traversal paths rather than relying on ZIP extraction defaults.
    for (int index = 0; index < entryCount; ++index)
    {
        const auto* entry = zip.getEntry(index);
        if (entry == nullptr || entry->isSymbolicLink)
            return juce::Result::fail("O bundle contem uma entrada invalida ou link simbolico.");

        auto normalised = entry->filename.replaceCharacter('\\', '/');
        if (normalised.endsWithChar('/'))
        {
            normalised = normalised.dropLastCharacters(1);
            if (normalised.isEmpty()) continue;
        }

        juce::String safePath;
        if (!normaliseAndValidatePath(normalised, safePath))
            return juce::Result::fail("O bundle contem um caminho inseguro.");

        if (entry->uncompressedSize < 0 || entry->uncompressedSize > maximumEntryBytes
            || totalExpandedBytes > maximumExpandedBytes - entry->uncompressedSize)
            return juce::Result::fail("O bundle excede o limite de tamanho expandido.");
        totalExpandedBytes += entry->uncompressedSize;

        const auto lower = safePath.toLowerCase();
        hasManifest = hasManifest || lower == "meta.xml";
        if (lower.endsWith(".npno")) ++pianoCount;
        else if (lower.endsWith(".ns4p")) ++programCount;
        else if (lower.endsWith(".nsmp4")) ++sampleCount;
    }

    if (!hasManifest || pianoCount == 0)
        return juce::Result::fail("O bundle nao contem meta.xml e pianos .npno reconheciveis.");

    if (const auto createRoot = libraryRoot.createDirectory(); createRoot.failed())
        return createRoot;

    const auto bankDirectory = uniqueBankDirectory(libraryRoot, safeBankName(source));
    if (const auto createBank = bankDirectory.createDirectory(); createBank.failed())
        return createBank;

    auto failImport = [&bankDirectory](const juce::String& message)
    {
        bankDirectory.deleteRecursively(false);
        return juce::Result::fail(message);
    };

    std::array<char, 64 * 1024> buffer {};
    for (int index = 0; index < entryCount; ++index)
    {
        const auto* entry = zip.getEntry(index);
        if (entry == nullptr) return failImport("Nao foi possivel ler uma entrada do bundle.");

        auto rawPath = entry->filename.replaceCharacter('\\', '/');
        if (rawPath.endsWithChar('/')) continue;

        juce::String relativePath;
        if (!normaliseAndValidatePath(rawPath, relativePath))
            return failImport("O bundle contem um caminho inseguro.");
        if (!hasSupportedExtension(relativePath)) continue;

        const auto destination = childForRelativePath(bankDirectory, relativePath);
        if (!destination.isAChildOf(bankDirectory))
            return failImport("O caminho de uma entrada escaparia da pasta do banco.");
        if (destination.exists())
            return failImport("O bundle contem nomes de arquivo duplicados.");
        const auto extractResult = extractEntry(zip, index, *entry, destination, buffer);
        if (extractResult.failed()) return failImport(extractResult.getErrorMessage());
    }

    bool markerWritten = false;
    {
        auto markerStream = bankDirectory.getChildFile(completionMarker).createOutputStream();
        markerWritten = markerStream != nullptr && markerStream->openedOk()
            && markerStream->writeString(source.getFileName())
            && markerStream->getStatus().wasOk();
    }
    if (!markerWritten)
        return failImport("Nao foi possivel finalizar o banco importado.");

    summary.bankDirectory = bankDirectory;
    summary.pianoCount = pianoCount;
    summary.programCount = programCount;
    summary.sampleLibraryCount = sampleCount;
    return juce::Result::ok();
}

juce::Array<juce::File> NordBundleImporter::findNordPianos(const juce::File& libraryRoot)
{
    juce::Array<juce::File> candidates;
    juce::Array<juce::File> result;
    libraryRoot.findChildFiles(candidates, juce::File::findFiles, true, "*",
                               juce::File::FollowSymlinks::no);
    for (const auto& file : candidates)
        if (file.getFileExtension().equalsIgnoreCase(".npno")
            && file.isAChildOf(libraryRoot) && belongsToCompletedBank(file, libraryRoot))
            result.add(file);

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
    {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    return result;
}
