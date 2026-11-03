#pragma once
#include "ClassicBackup.h"
#include <stdexcept>
#include <iostream>
inline void runClassicBackupRegression()
{
    const auto check = [](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); };
    ClassicBackup::TemporaryFolder fixture(juce::File::getSpecialLocation(juce::File::tempDirectory));
    const auto oldRoot = fixture.file.getChildFile("old/Classic Player");
    const auto newRoot = fixture.file.getChildFile("new/Classic Player");
    const auto asset = fixture.file.getChildFile(juce::String::fromUTF8("externo/pad ç.wav"));
    check(oldRoot.createDirectory().wasOk() && asset.getParentDirectory().createDirectory().wasOk(), "fixture folders");
    check(asset.replaceWithText("external sample bytes"), "fixture asset");
    const auto internal = oldRoot.getChildFile("NordPianos/piano.npno");
    check(internal.getParentDirectory().createDirectory().wasOk() && internal.replaceWithText("piano bytes"), "internal fixture");
    juce::XmlElement state("PARAMETERS");
    state.setAttribute("soundFont1", internal.getFullPathName());
    state.setAttribute("padFile", asset.getFullPathName());
    state.setAttribute("uiVirtualKeyboardVisible", false);
    state.setAttribute("uiDisplaySpectrum", true);
    juce::MemoryBlock binary;
    juce::AudioProcessor::copyXmlToBinary(state, binary);
    const auto oldPrefs = fixture.file.getChildFile("old/ui-settings");
    juce::XmlElement prefs("PROPERTIES"); prefs.setAttribute("saved", "yes");
    check(prefs.writeTo(oldPrefs), "preferences fixture");
    const auto zip = fixture.file.getChildFile(juce::String::fromUTF8("backup ç.zip"));
    const auto packed = ClassicBackup::create(oldRoot, oldPrefs, binary, zip);
    check(packed.wasOk(), packed.getErrorMessage().toRawUTF8());
    {
        juce::FileInputStream input(zip);
        ClassicBackup::Archive archive; archive.zip.m_pRead = ClassicBackup::read; archive.zip.m_pIO_opaque = &input;
        check(mz_zip_reader_init(&archive.zip, static_cast<mz_uint64>(zip.getSize()), 0) != 0, "read zip");
        check(mz_zip_is_zip64(&archive.zip) != 0, "backup must support ZIP64");
    }
    newRoot.createDirectory();
    newRoot.getChildFile("previous.txt").replaceWithText("preserve me");
    juce::File session;
    const auto newPrefs = fixture.file.getChildFile("new/ui-settings");
    const auto restored = ClassicBackup::restore(zip, newRoot, newPrefs, session);
    check(restored.wasOk(), restored.getErrorMessage().toRawUTF8());
    bool isBinary = false;
    auto loaded = ClassicBackup::readState(session, isBinary);
    check(loaded != nullptr && isBinary, "restored session");
    check(loaded->getStringAttribute("soundFont1") == newRoot.getChildFile("NordPianos/piano.npno").getFullPathName(), "internal path remap");
    const juce::File restoredAsset(loaded->getStringAttribute("padFile"));
    check(restoredAsset.isAChildOf(newRoot) && restoredAsset.loadFileAsString() == "external sample bytes", "external asset remap");
    check(newPrefs.existsAsFile(), "preferences restored");
    juce::Array<juce::File> previous;
    newRoot.getParentDirectory().findChildFiles(previous, juce::File::findDirectories, false, "Classic Player antes do backup*");
    check(previous.size() == 1 && previous[0].getChildFile("previous.txt").existsAsFile(), "previous library retained");
    const auto bad = fixture.file.getChildFile("bad.zip");
    {
        juce::FileOutputStream output(bad);
        ClassicBackup::Archive archive; archive.writing = true; archive.zip.m_pWrite = ClassicBackup::write; archive.zip.m_pIO_opaque = &output;
        check(mz_zip_writer_init_v2(&archive.zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64) != 0, "bad zip init");
        check(mz_zip_writer_add_mem(&archive.zip, "../escape.txt", "bad", 3, MZ_BEST_SPEED) != 0, "unsafe fixture");
        check(mz_zip_writer_finalize_archive(&archive.zip) != 0, "bad zip finalise");
    }
    check(ClassicBackup::restore(bad, newRoot, newPrefs, session).failed(), "path traversal rejected");
    check(newRoot.getChildFile("NordPianos/piano.npno").loadFileAsString() == "piano bytes", "failed restore preserves library");
    check(!fixture.file.getChildFile("escape.txt").exists(), "no extraction outside stage");
    std::cout << "ZIP64 backup, external assets, path remapping, preferences and rollback safety passed\n";
}
