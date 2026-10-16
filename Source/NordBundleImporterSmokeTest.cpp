#include "NordBundleImporter.h"

#include <cstring>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

juce::File makeFile(const juce::File& root, const juce::String& name, const char* contents)
{
    const auto file = root.getChildFile(name);
    file.getParentDirectory().createDirectory();
    file.replaceWithData(contents, std::strlen(contents));
    return file;
}

bool createBundle(const juce::File& bundle,
                  const juce::File& meta,
                  const juce::File& piano,
                  const juce::File& program,
                  const juce::File& sample,
                  const juce::String& pianoStoredPath)
{
    juce::ZipFile::Builder builder;
    builder.addFile(meta, 0, "meta.xml");
    builder.addFile(piano, 0, pianoStoredPath);
    builder.addFile(program, 0, "Program/Bank H/Test.ns4p");
    builder.addFile(sample, 0, "Samp Lib/Test.nsmp4");

    auto output = bundle.createOutputStream();
    return output != nullptr && output->openedOk()
        && builder.writeToStream(*output, nullptr) && output->getStatus().wasOk();
}
}

int main()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("classic-player-nord-bundle-test-"
                      + juce::String(juce::Random::getSystemRandom().nextInt()));
    if (!require(root.createDirectory().wasOk(), "create temporary test folder")) return 1;

    const auto sourceFiles = root.getChildFile("source");
    sourceFiles.createDirectory();
    const auto meta = makeFile(sourceFiles, "meta.xml", "<bundle version=\"1\"/>");
    const auto piano = makeFile(sourceFiles, "piano.bin", "npno-payload");
    const auto program = makeFile(sourceFiles, "program.bin", "ns4p-payload");
    const auto sample = makeFile(sourceFiles, "sample.bin", "nsmp4-payload");
    const auto bundle = root.getChildFile("Test Bank.ns4pbundle");
    const auto library = root.getChildFile("Nord Banks");

    auto passed = require(createBundle(bundle, meta, piano, program, sample,
                                       "Piano/Grand/Test Piano.npno"), "create valid ZIP bundle");

    NordBundleImporter::ImportSummary summary;
    auto result = NordBundleImporter::importBundle(bundle, library, summary);
    passed &= require(result.wasOk(), "import supported bundle");
    passed &= require(summary.pianoCount == 1 && summary.programCount == 1
                      && summary.sampleLibraryCount == 1, "report bundled asset counts");
    auto pianos = NordBundleImporter::findNordPianos(library);
    passed &= require(pianos.size() == 1, "catalogue imported Nord piano");
    if (pianos.size() == 1)
        passed &= require(pianos.getReference(0).loadFileAsString() == "npno-payload",
                          "preserve piano file bytes");

    NordBundleImporter::ImportSummary duplicateSummary;
    result = NordBundleImporter::importBundle(bundle, library, duplicateSummary);
    passed &= require(result.wasOk() && duplicateSummary.bankDirectory != summary.bankDirectory,
                      "avoid overwriting a previously imported bank");
    passed &= require(NordBundleImporter::findNordPianos(library).size() == 2,
                      "catalogue both imported copies");

    const auto unsafeBundle = root.getChildFile("Unsafe.ns4pbundle");
    passed &= require(createBundle(unsafeBundle, meta, piano, program, sample,
                                   "../escape.npno"), "create traversal-path ZIP fixture");
    NordBundleImporter::ImportSummary unsafeSummary;
    result = NordBundleImporter::importBundle(unsafeBundle, library, unsafeSummary);
    passed &= require(result.failed(), "reject path traversal before extracting");
    passed &= require(!root.getChildFile("escape.npno").exists(), "do not write outside bank folder");

    root.deleteRecursively(false);
    std::cout << (passed ? "Nord bundle importer smoke test passed\n"
                         : "Nord bundle importer smoke test failed\n");
    return passed ? 0 : 1;
}
