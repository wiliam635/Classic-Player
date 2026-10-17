#include "NordBankEngine.h"
#include "NordBundleImporter.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const juce::String& reason) { if (!ok) throw std::runtime_error(reason.toStdString()); }
}
int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Pass the local .ns4pbundle fixture (not shipped).");
        juce::TemporaryFile temporary;
        const auto root = temporary.getFile();
        require(root.createDirectory().wasOk(), "temporary directory");
        NordBundleImporter::ImportSummary summary;
        const auto imported = NordBundleImporter::importBundle(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]), root, summary);
        require(imported.wasOk(), imported.getErrorMessage());
        require(summary.programCount == 30 && summary.pianoCount == 10 && summary.sampleLibraryCount == 1, "catalog counts");
        const auto programs = NordBankEngine::programs(summary.bankDirectory);
        require(programs.size() == 30, "manifest enumeration");
        auto engine = std::make_unique<NordBankEngine>();
        engine->prepare(44100, 256);
        int played = 0;
        juce::String previous;
        for (const auto& program : programs) {
            if (!program.playable[0]) continue;
            std::cout << "Load " << program.name << std::endl;
            auto result = engine->load(0, program.file, 0);
            require(result.wasOk(), result.getErrorMessage());
            previous = engine->path(0);
            Sf2Engine::LayerConfig config; config.release = 30;
            juce::AudioBuffer<float> output(2, 256); juce::MidiBuffer midi, empty;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
            float peak = 0;
            for (int block = 0; block < 30; ++block) {
                output.clear(); engine->process(0, output, midi, empty, config); midi.clear();
                const auto magnitude = output.getMagnitude(0, 256);
                require(std::isfinite(magnitude), "nonfinite audio"); peak = juce::jmax(peak, magnitude);
            }
            require(peak > 0.00001f, "silent compatible program: " + program.name);
            ++played;
        }
        require(played == 7, "compatible scene I count");
        for (const auto& program : programs) if (!program.playable[0]) {
            require(engine->load(0, program.file, 0).failed(), "unsupported program accepted");
            require(engine->path(0) == previous, "failed selection replaced previous sound"); break;
        }
        engine->stopAllSounds(); engine->unload(0);
        require(!engine->isLoaded(0), "unload");
        require(NordBankEngine::programs(summary.bankDirectory).size() == 30, "persistent catalog");
        std::cout << "Nord bank import/playback regression passed" << std::endl;
        // Only our generated temporary test copy is deleted; original bundle is untouched.
        root.deleteRecursively(false);
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
