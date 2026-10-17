#include "NordBankEngine.h"
#include <algorithm>

namespace {
bool readProgram(const juce::File& file, NordProgramDecoder::Program& p, juce::String& reason)
{
    if (file.getSize() != 868) { reason = "Programa NS4 com tamanho nao suportado."; return false; }
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data)) { reason = "Nao foi possivel ler o programa."; return false; }
    const auto* first = static_cast<const std::uint8_t*>(data.getData());
    std::string error;
    const auto ok = NordProgramDecoder::decode({ first, first + data.getSize() }, p, error);
    reason = juce::String::fromUTF8(error.c_str());
    return ok;
}
juce::File bankOf(juce::File file)
{
    for (int i = 0; i < 8; ++i) {
        if (file.getChildFile(".classic-player-bank-complete").existsAsFile()
            && file.getChildFile("meta.xml").existsAsFile()) return file;
        const auto parent = file.getParentDirectory();
        if (parent == file) break;
        file = parent;
    }
    return {};
}
juce::File safeChild(const juce::File& bank, const juce::String& relative)
{
    if (relative.isEmpty() || juce::File::isAbsolutePath(relative) || relative.contains(":")
        || relative.contains("\\")) return {};
    const auto file = bank.getChildFile(relative);
    return file.isAChildOf(bank) ? file : juce::File{};
}
juce::String compact(const juce::String& s)
{
    auto tokens = juce::StringArray::fromTokens(s.toLowerCase(), " ", "");
    tokens.removeEmptyStrings();
    return tokens.joinIntoString(" ");
}
juce::File pianoDependency(const juce::File& bank, const juce::StringArray& deps,
                           const std::string& name)
{
    if (name.empty()) return {};
    const auto wanted = compact(juce::String::fromUTF8(name.c_str()));
    for (const auto& relative : deps) {
        const auto file = safeChild(bank, relative);
        if (!file.hasFileExtension("npno") || !file.existsAsFile()) continue;
        auto stem = compact(file.getFileNameWithoutExtension());
        if (stem == wanted) return file;
        if (stem.startsWith(wanted + " ")) {
            const auto version = stem.substring(wanted.length() + 1);
            if (version.isNotEmpty() && version.containsOnly("0123456789.")) return file;
        }
    }
    return {};
}
}

struct NordBankEngine::State
{
    juce::File file;
    int scene = 0;
    NordProgramDecoder::Program program;
    SampleEngine pianos;
    HammondEngine organs;
    std::array<Sf2Engine::LayerConfig, Sf2Engine::layerCount> pianoConfig {};
    std::array<HammondEngine::Config, Sf2Engine::layerCount> organConfig {};
    std::array<juce::MidiBuffer, Sf2Engine::layerCount> pianoMidi, organMidi;
    juce::MidiBuffer empty;
    juce::AudioBuffer<float> scratch;
    juce::String details;
};
NordBankEngine::Slot::Slot() = default;
NordBankEngine::Slot::~Slot() = default;

juce::File NordBankEngine::libraryRoot()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Classic Player").getChildFile("Nord Banks");
}
juce::Array<juce::File> NordBankEngine::banks()
{
    juce::Array<juce::File> result;
    for (const auto& file : libraryRoot().findChildFiles(juce::File::findDirectories, false))
        if (bankOf(file) == file) result.add(file);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    return result;
}
std::vector<NordBankEngine::ProgramInfo> NordBankEngine::programs(const juce::File& bank)
{
    std::vector<ProgramInfo> result;
    if (bankOf(bank) != bank) return result;
    auto xml = juce::parseXML(bank.getChildFile("meta.xml"));
    if (xml == nullptr || !xml->hasTagName("bundle")) return result;
    for (const auto* child : xml->getChildIterator()) {
        const auto file = safeChild(bank, child->getStringAttribute("name"));
        if (!file.hasFileExtension("ns4p") || !file.existsAsFile()) continue;
        ProgramInfo info;
        info.file = file;
        info.name = file.getFileNameWithoutExtension();
        NordProgramDecoder::Program p;
        juce::String error;
        if (readProgram(file, p, error)) {
            info.scene = p.scene;
            for (int scene = 0; scene < 2; ++scene) {
                std::string reason;
                info.playable[size_t(scene)] = p.playable(scene, reason);
                info.reason[size_t(scene)] = juce::String::fromUTF8(reason.c_str());
            }
        } else info.reason.fill(error);
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.name.compareNatural(b.name) < 0;
    });
    return result;
}
void NordBankEngine::prepare(double sampleRate, int blockSize)
{
    rate.store(sampleRate); block.store(blockSize);
    for (auto& slot : slots) {
        const juce::ScopedLock lock(slot.lock);
        if (auto* s = slot.state.get()) {
            s->pianos.prepare(sampleRate, blockSize);
            s->organs.prepare(sampleRate, blockSize);
            s->scratch.setSize(2, blockSize);
        }
    }
}
juce::Result NordBankEngine::load(int layer, const juce::File& file, int selectedScene)
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return juce::Result::fail("Layer invalida.");
    const auto bank = bankOf(file.getParentDirectory());
    if (!bank.exists()) return juce::Result::fail("Importe o bundle para a biblioteca antes de abrir um programa.");
    auto s = std::make_unique<State>();
    juce::String error;
    if (!readProgram(file, s->program, error)) return juce::Result::fail(error);
    s->scene = selectedScene < 0 ? s->program.scene : selectedScene;
    std::string reason;
    if (!s->program.playable(s->scene, reason)) return juce::Result::fail(juce::String::fromUTF8(reason.c_str()));
    auto xml = juce::parseXML(bank.getChildFile("meta.xml"));
    if (xml == nullptr) return juce::Result::fail("Manifesto do banco invalido.");
    juce::StringArray dependencies;
    bool found = false;
    for (const auto* child : xml->getChildIterator())
        if (safeChild(bank, child->getStringAttribute("name")) == file) {
            found = true;
            const auto count = child->getIntAttribute("depCnt");
            if (count < 0 || count > 32) return juce::Result::fail("Dependencias do programa invalidas.");
            for (int i = 0; i < count; ++i) dependencies.add(child->getStringAttribute("dep" + juce::String(i)));
        }
    if (!found) return juce::Result::fail("Programa ausente no manifesto do banco.");
    s->file = file;
    s->pianos.prepare(rate.load(), block.load());
    s->organs.prepare(rate.load(), block.load());
    s->scratch.setSize(2, block.load());
    for (auto& c : s->pianoConfig) c.enabled = false;
    for (auto& c : s->organConfig) c.routing.enabled = false;
    const auto& sections = s->program.sections[size_t(s->scene)];
    juce::StringArray parts;
    for (int part = 0; part < 2; ++part) {
        const auto& p = s->program.pianos[size_t(part)];
        if (sections[0] && p.enabled[size_t(s->scene)] && p.gain > 0) {
            const auto piano = pianoDependency(bank, dependencies, p.pianoName);
            if (!piano.existsAsFile()) return juce::Result::fail("Piano requerido nao localizado: "
                + (p.pianoName.empty() ? juce::String(p.pianoId) : juce::String::fromUTF8(p.pianoName.c_str())));
            const auto loaded = s->pianos.loadSample(part, piano);
            if (loaded.failed()) return loaded;
            s->pianoConfig[size_t(part)].enabled = true;
            parts.add("Piano " + juce::String(part == 0 ? "A: " : "B: ") + juce::String::fromUTF8(p.pianoName.c_str()));
        }
        const auto& o = s->program.organs[size_t(part)];
        if (sections[1] && o.enabled[size_t(s->scene)] && o.gain > 0) {
            auto& c = s->organConfig[size_t(part)];
            c.routing.enabled = true; c.bars = o.drawbars; c.leslie = s->program.leslie;
            c.percussion = o.percussion; c.drive = s->program.organDrive; c.level = 1; c.preset = -1;
            parts.add("B3 " + juce::String(part == 0 ? "A" : "B"));
        }
    }
    s->details = parts.joinIntoString(" | ") + "\nPianos/B3 adaptados; efeitos e morphs Nord nao sao identicos.";
    std::unique_ptr<State> old;
    { const juce::ScopedLock lock(slots[size_t(layer)].lock);
      old = std::move(slots[size_t(layer)].state); slots[size_t(layer)].state = std::move(s); }
    return juce::Result::ok();
}
void NordBankEngine::unload(int layer)
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return;
    std::unique_ptr<State> old;
    { const juce::ScopedLock lock(slots[size_t(layer)].lock); old = std::move(slots[size_t(layer)].state); }
    slots[size_t(layer)].peak.store(0);
}
void NordBankEngine::stopAllSounds()
{
    for (auto& slot : slots) { const juce::ScopedLock lock(slot.lock);
        if (slot.state) { slot.state->pianos.stopAllSounds(); slot.state->organs.stopAllSounds(); } }
}
bool NordBankEngine::isLoaded(int layer) const { return path(layer).isNotEmpty(); }
juce::String NordBankEngine::path(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return {};
    const juce::ScopedLock lock(slots[size_t(layer)].lock);
    return slots[size_t(layer)].state ? slots[size_t(layer)].state->file.getFullPathName() : juce::String{};
}
juce::String NordBankEngine::description(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return {};
    const juce::ScopedLock lock(slots[size_t(layer)].lock);
    return slots[size_t(layer)].state ? slots[size_t(layer)].state->details : juce::String{};
}
int NordBankEngine::scene(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return 0;
    const juce::ScopedLock lock(slots[size_t(layer)].lock);
    return slots[size_t(layer)].state ? slots[size_t(layer)].state->scene : 0;
}
float NordBankEngine::peak(int layer) const
{ return juce::isPositiveAndBelow(layer, Sf2Engine::layerCount) ? slots[size_t(layer)].peak.load() : 0; }
void NordBankEngine::process(int layer, juce::AudioBuffer<float>& output, const juce::MidiBuffer& host,
                            const juce::MidiBuffer& routed, const Sf2Engine::LayerConfig& outer)
{
    if (!juce::isPositiveAndBelow(layer, Sf2Engine::layerCount)) return;
    auto& slot = slots[size_t(layer)];
    const juce::ScopedLock lock(slot.lock);
    auto* s = slot.state.get();
    if (!s) { slot.peak.store(0); return; }
    s->scratch.setSize(2, output.getNumSamples(), false, false, true);
    s->scratch.clear();
    const auto configure = [&](const NordProgramDecoder::Part& p, Sf2Engine::LayerConfig& c, bool piano) {
        const auto enabled = c.enabled;
        c = outer; c.enabled = enabled;
        c.gain = outer.enabled ? outer.gain * p.gain : 0;
        c.octave = juce::jlimit(-5, 5, outer.octave + p.octave);
        c.lowNote = 0; c.highNote = 127;
        c.sustainEnabled = outer.sustainEnabled && p.sustain;
        c.pianoTimbre = p.timbre;
        c.eqLow = juce::jlimit(-15.0f, 15.0f, outer.eqLow + p.eqLow);
        c.eqMid = juce::jlimit(-15.0f, 15.0f, outer.eqMid + p.eqMid);
        c.eqHigh = juce::jlimit(-15.0f, 15.0f, outer.eqHigh + p.eqHigh);
        c.reverb = juce::jlimit(0.0f, 100.0f, outer.reverb + p.reverb);
        if (piano) c.release = juce::jlimit(0.0f, 100.0f, outer.release + ((p.model == 0 || p.model == 1) ? 0.0f : -25.0f));
    };
    for (int part = 0; part < 2; ++part) {
        configure(s->program.pianos[size_t(part)], s->pianoConfig[size_t(part)], true);
        configure(s->program.organs[size_t(part)], s->organConfig[size_t(part)].routing, false);
        s->pianoMidi[size_t(part)].clear(); s->organMidi[size_t(part)].clear();
    }
    const auto collect = [&](const juce::MidiBuffer& events) {
        for (const auto metadata : events) {
            auto message = metadata.getMessage();
            if (message.isNoteOnOrOff()) {
                const auto note = message.getNoteNumber();
                if ((!outer.enabled && message.isNoteOn()) || note < outer.lowNote || note > outer.highNote) continue;
                const auto transposed = note + s->program.transpose;
                if (!juce::isPositiveAndBelow(transposed, 128)) continue;
                message.setNoteNumber(transposed);
                for (int part = 0; part < 2; ++part) {
                    if (s->program.accepts(s->program.pianos[size_t(part)], note))
                        s->pianoMidi[size_t(part)].addEvent(message, metadata.samplePosition);
                    if (s->program.accepts(s->program.organs[size_t(part)], note))
                        s->organMidi[size_t(part)].addEvent(message, metadata.samplePosition);
                }
            } else for (int part = 0; part < 2; ++part) {
                s->pianoMidi[size_t(part)].addEvent(message, metadata.samplePosition);
                s->organMidi[size_t(part)].addEvent(message, metadata.samplePosition);
            }
        }
    };
    collect(host); collect(routed);
    s->pianos.process(s->scratch, s->empty, s->pianoMidi, s->pianoConfig);
    s->organs.process(s->scratch, s->empty, s->organConfig, &s->organMidi);
    slot.peak.store(outer.enabled ? s->scratch.getMagnitude(0, s->scratch.getNumSamples()) : 0);
    if (outer.enabled) for (int channel = 0; channel < output.getNumChannels(); ++channel)
        output.addFrom(channel, 0, s->scratch, juce::jmin(channel, 1), 0, output.getNumSamples());
}
