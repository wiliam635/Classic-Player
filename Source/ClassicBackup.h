#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <miniz.h>
#include <map>
#include <set>
#include <limits>
#include <vector>

// Filesystem-only work; run on a worker, never on the audio/message thread.
namespace ClassicBackup
{
struct TemporaryFolder
{
    juce::File file;
    explicit TemporaryFolder(const juce::File& parent)
        : file(parent.getChildFile(".classic-backup-" + juce::Uuid().toString())) {}
    ~TemporaryFolder() { file.deleteRecursively(); }
};
inline juce::String normalise(juce::String path) { return path.replaceCharacter('\\', '/'); }
inline bool safeRelative(const juce::String& path)
{
    const auto p = normalise(path);
    if (p.isEmpty() || p.startsWithChar('/') || p.containsChar(':') || p.containsChar('\0')) return false;
    for (const auto& part : juce::StringArray::fromTokens(p, "/", ""))
        if (part == ".." || part == "." || part.isEmpty()) return false;
    return true;
}
inline bool assetPath(const juce::String& s)
{
    return juce::File::isAbsolutePath(s) && juce::File(s).hasFileExtension(
        "sf2;npno;nsmp;nsmp3;nsmp4;syx;wav;aif;aiff;flac;mp3;ogg;mid;midi");
}
template <typename Fn> void walk(juce::XmlElement& xml, const Fn& fn)
{
    for (int i = 0; i < xml.getNumAttributes(); ++i)
    {
        const auto key = xml.getAttributeName(i);
        xml.setAttribute(key, fn(xml.getAttributeValue(i)));
    }
    for (auto* child : xml.getChildIterator()) walk(*child, fn);
}
inline std::unique_ptr<juce::XmlElement> readState(const juce::File& file, bool& binary)
{
    binary = file.hasFileExtension("ckprogram;cklayer");
    if (binary)
    {
        juce::MemoryBlock b;
        if (file.getSize() > 64 * 1024 * 1024 || !file.loadFileAsData(b)) return {};
        return juce::AudioProcessor::getXmlFromBinary(b.getData(), static_cast<int>(b.getSize()));
    }
    if (!file.hasFileExtension("xml;settings;ui-settings") || file.getSize() > 64 * 1024 * 1024) return {};
    return juce::XmlDocument::parse(file);
}
inline bool writeState(const juce::File& file, juce::XmlElement& xml, bool binary)
{
    if (!binary) return xml.writeTo(file);
    juce::MemoryBlock b;
    juce::AudioProcessor::copyXmlToBinary(xml, b);
    return file.replaceWithData(b.getData(), b.getSize());
}
inline size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t bytes)
{
    auto& stream = *static_cast<juce::FileInputStream*>(opaque);
    if (offset > static_cast<mz_uint64>(std::numeric_limits<juce::int64>::max())
        || !stream.setPosition(static_cast<juce::int64>(offset))) return 0;
    return static_cast<size_t>(juce::jmax(0, stream.read(buffer,
        static_cast<int>(juce::jmin(bytes, static_cast<size_t>(std::numeric_limits<int>::max()))))));
}
inline size_t write(void* opaque, mz_uint64 offset, const void* buffer, size_t bytes)
{
    auto& stream = *static_cast<juce::FileOutputStream*>(opaque);
    return offset <= static_cast<mz_uint64>(std::numeric_limits<juce::int64>::max())
        && stream.setPosition(static_cast<juce::int64>(offset)) && stream.write(buffer, bytes) ? bytes : 0;
}
struct Archive
{
    mz_zip_archive zip {};
    bool writing = false;
    ~Archive() { if (writing) mz_zip_writer_end(&zip); else mz_zip_reader_end(&zip); }
};
inline juce::Result pack(const juce::File& folder, const juce::File& destination)
{
    juce::TemporaryFile temp(destination);
    {
        juce::FileOutputStream output(temp.getFile());
        if (!output.openedOk()) return juce::Result::fail("Nao foi possivel criar o ZIP.");
        Archive archive;
        archive.writing = true;
        archive.zip.m_pWrite = write;
        archive.zip.m_pIO_opaque = &output;
        if (!mz_zip_writer_init_v2(&archive.zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64))
            return juce::Result::fail("Nao foi possivel iniciar o ZIP64.");
        juce::Array<juce::File> files;
        folder.findChildFiles(files, juce::File::findFiles, true);
        for (const auto& file : files)
        {
            juce::FileInputStream input(file);
            if (!input.openedOk()) return juce::Result::fail("Falha ao ler: " + file.getFileName());
            const auto name = normalise(file.getRelativePathFrom(folder));
            if (!mz_zip_writer_add_read_buf_callback(&archive.zip, name.toRawUTF8(), read, &input,
                    static_cast<mz_uint64>(file.getSize()), nullptr, nullptr, 0,
                    MZ_BEST_SPEED, nullptr, 0, nullptr, 0))
                return juce::Result::fail("Falha ao arquivar: " + name);
        }
        if (!mz_zip_writer_finalize_archive(&archive.zip)) return juce::Result::fail("Falha ao finalizar ZIP64.");
        output.flush();
        if (output.getStatus().failed()) return output.getStatus();
    }
    return temp.overwriteTargetFileWithTemporary() ? juce::Result::ok()
        : juce::Result::fail("Nao foi possivel gravar o backup no destino.");
}
inline juce::Result unpack(const juce::File& source, const juce::File& folder)
{
    juce::FileInputStream input(source);
    if (!input.openedOk()) return juce::Result::fail("Nao foi possivel abrir o backup.");
    Archive archive;
    archive.zip.m_pRead = read;
    archive.zip.m_pIO_opaque = &input;
    if (!mz_zip_reader_init(&archive.zip, static_cast<mz_uint64>(source.getSize()), 0))
        return juce::Result::fail("Arquivo ZIP/ZIP64 invalido.");
    const auto count = mz_zip_reader_get_num_files(&archive.zip);
    if (count > 1000000) return juce::Result::fail("Backup com arquivos demais.");
    std::set<std::string> names;
    mz_uint64 total = 0;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&archive.zip, i, &stat) || stat.m_is_encrypted || !stat.m_is_supported)
            return juce::Result::fail("Entrada ZIP nao suportada.");
        if (stat.m_is_directory) continue;
        const auto length = mz_zip_reader_get_filename(&archive.zip, i, nullptr, 0);
        if (length == 0 || length > 16384) return juce::Result::fail("Nome ZIP invalido.");
        std::vector<char> buffer(length);
        mz_zip_reader_get_filename(&archive.zip, i, buffer.data(), length);
        const auto name = normalise(juce::String::fromUTF8(buffer.data()));
        if (!safeRelative(name) || !names.insert(name.toLowerCase().toStdString()).second
            || ((stat.m_external_attr >> 16) & 0170000) == 0120000)
            return juce::Result::fail("Caminho ZIP inseguro ou duplicado.");
        if (stat.m_uncomp_size > 1024ull * 1024 * 1024 * 1024 - total)
            return juce::Result::fail("Backup maior que o limite de 1 TiB.");
        total += stat.m_uncomp_size;
    }
    if (static_cast<juce::int64>(total) > folder.getParentDirectory().getBytesFreeOnVolume())
        return juce::Result::fail("Espaco em disco insuficiente para restaurar o backup.");
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&archive.zip, i, &stat)) return juce::Result::fail("Indice ZIP invalido.");
        if (stat.m_is_directory) continue;
        const auto length = mz_zip_reader_get_filename(&archive.zip, i, nullptr, 0);
        std::vector<char> buffer(length);
        mz_zip_reader_get_filename(&archive.zip, i, buffer.data(), length);
        const auto target = folder.getChildFile(normalise(juce::String::fromUTF8(buffer.data())));
        if (!target.isAChildOf(folder) || target.getParentDirectory().createDirectory().failed())
            return juce::Result::fail("Falha ao preparar pasta de restauracao.");
        juce::FileOutputStream output(target);
        if (!output.openedOk() || !mz_zip_reader_extract_to_callback(&archive.zip, i, write, &output, 0))
            return juce::Result::fail("Falha de leitura/CRC ao restaurar: " + target.getFileName());
        output.flush();
        if (output.getStatus().failed()) return output.getStatus();
    }
    return juce::Result::ok();
}
inline juce::Result create(const juce::File& root, const juce::File& uiSettings,
                           const juce::MemoryBlock& currentState, const juce::File& destination)
{
    if (destination.isAChildOf(root)) return juce::Result::fail("Salve o ZIP fora da pasta de dados da Classic Player.");
    TemporaryFolder temp(juce::File::getSpecialLocation(juce::File::tempDirectory));
    const auto data = temp.file.getChildFile("Data");
    if (data.createDirectory().failed()) return juce::Result::fail("Falha ao preparar backup.");
    juce::Array<juce::File> files;
    root.findChildFiles(files, juce::File::findFiles, true);
    for (const auto& source : files)
    {
        for (auto parent = source; parent != root && parent.isAChildOf(root); parent = parent.getParentDirectory())
            if (parent.isSymbolicLink()) return juce::Result::fail("Backup nao segue links simbolicos: " + parent.getFileName());
        const auto target = data.getChildFile(source.getRelativePathFrom(root));
        if (target.getParentDirectory().createDirectory().failed() || !source.copyFileTo(target))
            return juce::Result::fail("Falha ao copiar: " + source.getFileName());
    }
    const auto session = data.getChildFile("Programs").getNonexistentChildFile("Backup Session", ".ckprogram", false);
    if (session.getParentDirectory().createDirectory().failed()
        || !session.replaceWithData(currentState.getData(), currentState.getSize()))
        return juce::Result::fail("Falha ao salvar a performance atual no backup.");
    juce::XmlElement manifest("ClassicPlayerBackup");
    manifest.setAttribute("version", 1);
    manifest.setAttribute("originalRoot", root.getFullPathName());
    manifest.setAttribute("session", normalise(session.getRelativePathFrom(data)));
    const auto preferences = temp.file.getChildFile("Preferences");
    preferences.createDirectory();
    // Application preferences live beside, rather than inside, the data folder.
    juce::Array<juce::File> settingsFiles;
    settingsFiles.add(uiSettings);
    settingsFiles.add(uiSettings.getSiblingFile("Classic Player.settings"));
    for (int i = 0; i < settingsFiles.size(); ++i)
        if (settingsFiles[i].existsAsFile() && !settingsFiles[i].copyFileTo(preferences.getChildFile(juce::String(i) + ".xml")))
            return juce::Result::fail("Falha ao copiar preferencias.");
    files.clear();
    data.findChildFiles(files, juce::File::findFiles, true);
    std::map<juce::String, juce::String> assets;
    bool copyFailed = false;
    juce::StringArray missingAssets;
    for (const auto& file : files)
    {
        bool binary = false;
        if (auto xml = readState(file, binary))
            walk(*xml, [&](const juce::String& value)
            {
                if (!assetPath(value)) return value;
                const juce::File source(value);
                if (!source.existsAsFile()) { missingAssets.addIfNotAlreadyThere(value); return value; }
                if (source.isAChildOf(root) || assets.count(value) != 0) return value;
                const auto rel = "Imported Assets/" + juce::String::toHexString(value.hashCode64()) + "/" + source.getFileName();
                const auto target = data.getChildFile(rel);
                if (source.isSymbolicLink() || target.getParentDirectory().createDirectory().failed() || !source.copyFileTo(target)) copyFailed = true;
                assets[value] = rel;
                return value;
            });
    }
    if (!missingAssets.isEmpty()) return juce::Result::fail("Backup nao concluido: timbre referenciado ausente: " + missingAssets[0]);
    if (copyFailed) return juce::Result::fail("Nao foi possivel incluir um timbre externo no backup.");
    for (const auto& item : assets)
    {
        auto* entry = manifest.createNewChildElement("asset");
        entry->setAttribute("old", item.first); entry->setAttribute("relative", normalise(item.second));
    }
    if (!manifest.writeTo(temp.file.getChildFile("manifest.xml"))) return juce::Result::fail("Falha ao salvar indice do backup.");
    return pack(temp.file, destination);
}
inline juce::Result restore(const juce::File& source, const juce::File& root,
                            const juce::File& uiSettings, juce::File& session)
{
    const auto parent = root.getParentDirectory();
    if (parent.createDirectory().failed()) return juce::Result::fail("Falha ao preparar pasta de dados.");
    TemporaryFolder temp(parent);
    if (temp.file.createDirectory().failed()) return juce::Result::fail("Falha ao preparar restauracao.");
    if (auto r = unpack(source, temp.file); r.failed()) return r;
    const auto manifest = juce::XmlDocument::parse(temp.file.getChildFile("manifest.xml"));
    if (!manifest || !manifest->hasTagName("ClassicPlayerBackup") || manifest->getIntAttribute("version") != 1)
        return juce::Result::fail("Este ZIP nao e um backup da Classic Player.");
    const auto oldRoot = normalise(manifest->getStringAttribute("originalRoot"));
    const auto sessionPath = manifest->getStringAttribute("session");
    if (oldRoot.isEmpty() || !safeRelative(sessionPath)) return juce::Result::fail("Indice de backup invalido.");
    std::map<juce::String, juce::String> assets;
    for (auto* entry : manifest->getChildIterator())
    {
        const auto rel = entry->getStringAttribute("relative");
        if (!safeRelative(rel)) return juce::Result::fail("Referencia de timbre invalida.");
        assets[normalise(entry->getStringAttribute("old"))] = root.getChildFile(rel).getFullPathName();
    }
    const auto data = temp.file.getChildFile("Data");
    if (!data.isDirectory() || !data.getChildFile(sessionPath).existsAsFile()) return juce::Result::fail("Backup incompleto.");
    juce::Array<juce::File> files;
    temp.file.findChildFiles(files, juce::File::findFiles, true);
    for (const auto& file : files)
    {
        bool binary = false;
        if (auto xml = readState(file, binary))
        {
            walk(*xml, [&](const juce::String& value)
            {
                const auto p = normalise(value);
                if (const auto found = assets.find(p); found != assets.end()) return found->second;
                if (p == oldRoot) return root.getFullPathName();
                if (p.startsWith(oldRoot + "/"))
                {
                    const auto rel = p.substring(oldRoot.length() + 1);
                    if (safeRelative(rel)) return root.getChildFile(rel).getFullPathName();
                }
                return value;
            });
            if (!writeState(file, *xml, binary)) return juce::Result::fail("Falha ao atualizar caminhos do backup.");
        }
    }
    const auto rollback = parent.getNonexistentChildFile("Classic Player antes do backup", {}, false);
    const bool hadRoot = root.exists();
    if (hadRoot && !root.moveFileTo(rollback)) return juce::Result::fail("Nao foi possivel preservar os dados atuais.");
    if (!data.moveFileTo(root))
    {
        if (hadRoot) rollback.moveFileTo(root);
        return juce::Result::fail("Falha ao instalar backup; os dados anteriores foram mantidos.");
    }
    const auto prefs = temp.file.getChildFile("Preferences");
    const auto savedUi = prefs.getChildFile("0.xml");
    if (savedUi.existsAsFile())
    {
        uiSettings.getParentDirectory().createDirectory();
        if (!savedUi.copyFileTo(uiSettings))
            return juce::Result::fail("Biblioteca restaurada, mas houve falha nas preferencias. Dados anteriores em: " + rollback.getFullPathName());
    }
    // The running standalone owns its audio-device PropertiesFile. Keep that
    // snapshot in the restored folder for reference instead of replacing a
    // live file that the host will overwrite when it closes.
    const auto deviceSettings = prefs.getChildFile("1.xml");
    if (deviceSettings.existsAsFile()) deviceSettings.copyFileTo(root.getChildFile("Restored Device Settings.xml"));
    session = root.getChildFile(sessionPath);
    // Retain the previous directory as a recovery point until the user removes it.
    return juce::Result::ok();
}
}
