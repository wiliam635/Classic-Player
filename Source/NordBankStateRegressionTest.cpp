#include "PluginProcessor.h"
#include "NordProgramSchema.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
std::vector<std::uint8_t> b3Program() {
    std::vector<std::uint8_t> b(868);
    for (int i = 0; i < 4; ++i) { b[size_t(i)] = std::uint8_t("CBIN"[i]); b[size_t(i + 8)] = std::uint8_t("ns4p"[i]); }
    b[4] = 1; b[20] = 52; b[21] = 1; b[46] = 1; b[47] = 52;
    const auto field = [&](const std::string& group, const std::string& name, std::uint32_t value) {
        for (const auto& f : nord_program_schema::fields) if (group == f.group && name == f.name) {
            for (int bit = f.last[0]; bit >= f.first[0]; --bit) {
                b[size_t(bit / 8)] |= std::uint8_t((value & 1) << (7 - bit % 8)); value >>= 1;
            }
            return;
        }
        throw std::runtime_error("Missing fixture field");
    };
    field("m", "organ section on/off", 1);
    field("m", "organ section on/off (layer scene II)", 1);
    field("o", "layer on/off", 1); field("o", "layer on/off (scene II)", 1);
    field("o", "volume", 127); field("o", "KB zones", 15); field("o", "drawbar 3", 8);
    std::uint32_t crc = 0xffffffffu;
    for (size_t i = 44; i < b.size(); ++i) {
        crc ^= b[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    crc ^= 0xffffffffu;
    for (int i = 0; i < 4; ++i) b[size_t(24 + i)] = std::uint8_t(crc >> (8 * i));
    return b;
}
}
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    try {
        juce::TemporaryFile temp;
        const auto bank = temp.getFile(); check(bank.createDirectory().wasOk(), "temp bank");
        check(bank.getChildFile(".classic-player-bank-complete").replaceWithText("synthetic test"), "marker");
        check(bank.getChildFile("meta.xml").replaceWithText("<bundle><file name=\"Test.ns4p\" depCnt=\"0\"/></bundle>"), "manifest");
        const auto file = bank.getChildFile("Test.ns4p"); const auto data = b3Program();
        check(file.replaceWithData(data.data(), data.size()), "program fixture");
        using Processor = ClassicPlayerAudioProcessor;
        auto p = std::make_unique<Processor>(bank.getChildFile("settings"));
        p->prepareToPlay(44100, 128); p->resetToNewProgram();
        check(p->addLayer(), "SF2 neighbor");
        check(p->addLayer(Processor::LayerType::nordBank), "dedicated bank layer");
        const auto loaded = p->loadNordBankProgram(1, file, 1);
        check(loaded.wasOk(), loaded.getErrorMessage().toRawUTF8());
        auto config = p->layerConfig(1); config.midiChannel = 3; config.lowNote = 40; config.highNote = 90;
        p->setLayerConfig(1, config);
        juce::MemoryBlock state; p->getStateInformation(state);
        p->resetToNewProgram(); p->setStateInformation(state.getData(), int(state.getSize()));
        check(p->layerType(1) == Processor::LayerType::nordBank && p->soundFontPath(1) == file.getFullPathName(), "restored bank path/type");
        check(p->nordBanks().scene(1) == 1 && p->layerConfig(1).midiChannel == 3 && p->layerConfig(1).lowNote == 40, "restored scene/routing");
        check(p->layerType(0) == Processor::LayerType::sf2 && p->soundFontPath(0).isEmpty(), "SF2 neighbor changed");
        check(p->removeLayer(0), "remove preceding layer");
        check(p->layerType(0) == Processor::LayerType::nordBank && p->nordBanks().scene(0) == 1, "bank not compacted");
        check(!p->nordBanks().isLoaded(1), "hidden bank after compaction");
        p->setLayerType(0, Processor::LayerType::sf2);
        check(!p->nordBanks().isLoaded(0), "bank remained loaded after source switch");
        p.reset(); bank.deleteRecursively(false);
        std::cout << "Nord bank state regression passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
