#pragma once
#include "NordProgram.h"
#include "NordPianoDecoder.h"
#include "NordSampleDecoder.h"
#include "AnalogSynthEngine.h"
#include "HammondEngine.h"
#include "NordOrganEngine.h"
#include <set>

namespace NordBundleProgram
{
enum class Source { piano, sample, analog, hammond, organ };
struct Part
{
    Source source = Source::piano;
    juce::String name, category;
    juce::File file;
    Sf2Engine::LayerConfig routing;
    AnalogSynthEngine::Config analog;
    HammondEngine::Config hammond;
    NordOrganEngine::Config organ;
    std::shared_ptr<NordPianoDecoder::Instrument> piano;
    std::shared_ptr<NordSampleDecoder::Instrument> sample;
};
struct Prepared
{
    juce::String name, bank;
    juce::StringArray warnings;
    std::vector<Part> parts;
};
inline juce::File child(const juce::File& root, const juce::String& path)
{
    const auto normalized = path.replaceCharacter('\\', '/');
    const auto pieces = juce::StringArray::fromTokens(normalized, "/", "");
    if (normalized.isEmpty() || normalized.startsWithChar('/') || normalized.containsChar(':')
        || pieces.contains("..") || pieces.contains(".")) return {};
    const auto file = root.getChildFile(normalized);
    return file.isAChildOf(root) ? file : juce::File{};
}
inline float percent(int64_t v) { return juce::jlimit(0.0f, 100.0f, float(v) * 100.0f / 127.0f); }
inline float envelopeMs(int64_t v)
{
    if (v <= 0) return 0;
    return float(2.0 * std::pow(5000.0, juce::jlimit(0.0, 1.0, double(v) / 127.0)));
}
inline float attackControl(float ms)
{
    if (ms <= 50.0f) return juce::jmax(0.0f, ms);
    return juce::jlimit(50.0f, 100.0f, float(50.0 + 50.0 * std::log(ms / 50.0) / std::log(40.0)));
}
inline float releaseControl(float ms)
{
    if (ms <= 500) return juce::jlimit(0.0f, 50.0f, ms / 10.0f);
    return juce::jlimit(50.0f, 100.0f, float(50.0 + 50.0 * std::log(ms / 500.0) / std::log(20.0)));
}
inline unsigned zoneMask(const NordProgram::Program& p, int zone)
{
    static constexpr unsigned char masks[8][11] {
        {15,15,15,15,15,15,15,15,15,15,15}, {1,1,1,1,1,15,14,14,14,14,14},
        {3,3,3,3,3,15,12,12,12,12,12}, {1,2,3,3,3,15,14,14,14,2,12},
        {7,7,7,7,7,15,8,8,8,8,8}, {1,6,7,7,7,15,14,14,14,6,8},
        {3,4,7,7,7,15,12,12,12,4,8}, {1,2,3,6,7,15,14,6,12,4,8}
    };
    if (!p.get("common.splitEnabled")) return 15;
    const auto enabled = (p.get("common.splitEnabledLow") ? 1 : 0)
                       | (p.get("common.splitEnabledMid") ? 2 : 0)
                       | (p.get("common.splitEnabledHigh") ? 4 : 0);
    return zone >= 0 && zone <= 10 ? masks[enabled][zone] : 15;
}
inline void routing(const NordProgram::Program& p, const std::string& prefix,
                    Sf2Engine::LayerConfig& c, juce::StringArray& warnings)
{
    c.polyphony = 128; c.midiChannel = 0; c.pianoTimbre = -1;
    c.octave = juce::jlimit(-6, 6, int(p.get(prefix + ".octaveShift")));
    c.sustainEnabled = p.get(prefix + ".sustainEnable", 1) != 0;
    if (!p.get("common.splitEnabled")) return;
    const auto mask = zoneMask(p, int(p.get(prefix + ".kbZone", 5)));
    static constexpr int points[] {36,41,48,53,60,65,72,77,84,89,96};
    static constexpr const char* suffix[] {"Low", "Mid", "High"};
    int boundaries[] {0, 0, 0, 0, 128};
    for (int i = 0; i < 3; ++i)
        boundaries[i+1] = points[juce::jlimit(0, 10, int(p.get(std::string("common.splitPos") + suffix[i], 4)))];
    // Physical split points retain their positions even when one split is off;
    // zoneMask already merges inactive zones.
    int first = 0, last = 3;
    while (first < 3 && (mask & (1u << first)) == 0) ++first;
    while (last > first && (mask & (1u << last)) == 0) --last;
    c.lowNote = boundaries[first]; c.highNote = boundaries[last+1] - 1;
    if (c.lowNote > c.highNote)
    {
        c.lowNote = 0; c.highNote = 127;
        warnings.addIfNotAlreadyThere("Pontos de split fora de ordem: zona expandida para o teclado inteiro.");
    }
    for (const auto* s : suffix)
        if (p.get(std::string("common.splitWidth") + s) != 0)
            warnings.addIfNotAlreadyThere("Crossfade de split adaptado para limites de notas da Classic.");
}
inline void effects(const NordProgram::Program& p, const std::string& prefix,
                    Sf2Engine::LayerConfig& c, juce::StringArray& warnings)
{
    const auto get = [&](const char* key, int64_t fallback = 0) { return p.get(prefix + "." + key, fallback); };
    if (get("fx1Enable"))
    {
        if (get("fx1Type") == 0)
        { c.nordFx.tremoloMix = percent(get("fx1Amt")); c.nordFx.tremoloRate = 0.1f + percent(get("fx1RateClkDiv")) * 0.12f; }
        else warnings.addIfNotAlreadyThere("Modulacao 1 diferente de tremolo: ajuste manual necessario.");
    }
    if (get("fx2Enable"))
    {
        const auto type = get("fx2Type");
        if (type == 0) c.nordFx.chorusMix = percent(get("fx2Amt"));
        else if (type == 1) c.nordFx.flangerMix = percent(get("fx2Amt"));
        else warnings.addIfNotAlreadyThere("Modulacao 2 sem equivalente direto: ajuste manual necessario.");
        c.nordFx.chorusRate = c.nordFx.flangerRate = 0.05f + percent(get("fx2Rate")) * 0.05f;
    }
    if (get("dlyEnable"))
    {
        c.nordFx.delayMix = percent(get("dlyMix"));
        c.nordFx.delayFeedback = juce::jmin(90.0f, percent(get("dlyFbkAmt")));
        c.nordFx.delayBpm = juce::jlimit(30.0f, 300.0f, float(p.get("common.masterClockRate", 120)));
        warnings.addIfNotAlreadyThere("Delay adaptado para o tempo da Classic; filtros, ping-pong e divisoes podem diferir.");
    }
    if (get("rvbEnable"))
    {
        c.reverb = percent(get("rvbAmt"));
        c.reverbSize = juce::jlimit(15.0f, 95.0f, 25.0f + 12.0f * float(get("rvbType")));
    }
    if (get("cmpEnable")) c.compressor = percent(get("cmpAmt"));
    if (get("ampEnable"))
    {
        c.eqLow = juce::jlimit(-15.0f, 15.0f, float(get("ampBass")));
        c.eqMid = juce::jlimit(-15.0f, 15.0f, float(get("ampMid")));
        c.eqHigh = juce::jlimit(-15.0f, 15.0f, float(get("ampTreble")));
        if (get("ampDrive")) warnings.addIfNotAlreadyThere("Simulacao de amplificador adaptada aos motores da Classic.");
    }
}

inline juce::Result build(const juce::File& folder, const juce::String& path, Prepared& out)
{
    out = {};
    const auto file = child(folder, path);
    if (!file.existsAsFile() || !file.hasFileExtension(".ns4p") || file.getSize() > 65536)
        return juce::Result::fail("Programa Nord invalido ou ausente.");
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes)) return juce::Result::fail("Falha ao ler programa Nord.");
    NordProgram::Program p; std::string error;
    if (!NordProgram::read(static_cast<const uint8_t*>(bytes.getData()), bytes.getSize(), p, error))
        return juce::Result::fail(juce::String::fromUTF8(error.c_str()));
    const auto meta = juce::XmlDocument::parse(folder.getChildFile("meta.xml"));
    if (meta == nullptr || !meta->hasTagName("bundle")) return juce::Result::fail("Indice do banco Nord invalido.");
    juce::Array<juce::File> candidates;
    bool listed = false;
    for (const auto* entry : meta->getChildIterator())
        if (entry->hasTagName("file") && entry->getStringAttribute("name") == path)
        {
            listed = true;
            for (int i = 0; i < juce::jlimit(0, 256, entry->getIntAttribute("depCnt")); ++i)
            {
                const auto dependency = child(folder, entry->getStringAttribute("dep" + juce::String(i)));
                if (dependency.existsAsFile()) candidates.addIfNotAlreadyThere(dependency);
            }
        }
    if (!listed) return juce::Result::fail("Programa nao pertence ao indice deste banco.");
    juce::Array<juce::File> files;
    folder.findChildFiles(files, juce::File::findFiles, true, "*");
    for (const auto& f : files)
        if (f.hasFileExtension(".npno;.nsmp;.nsmp3;.nsmp4")) candidates.addIfNotAlreadyThere(f);
    // Hash only names. Listing a bank never decodes its audio or opens all samples.
    const auto resolve = [&](uint32_t id, bool piano)
    {
        for (const auto& candidate : candidates)
        {
            if (piano != candidate.hasFileExtension(".npno")) continue;
            auto name = candidate.getFileNameWithoutExtension();
            if (name.contains("__")) name = name.fromFirstOccurrenceOf("__", false, false);
            if (NordProgram::soundId(name.toStdString()) == id
                || (piano && NordProgram::soundId(name.replaceCharacter('_', ' ').toStdString()) == id)) return candidate;
        }
        return juce::File{};
    };
    out.name = file.getFileNameWithoutExtension(); out.bank = folder.getFileName();
    const std::string scene = p.get("common.layerScene") ? "_II" : "_I";
    if (scene == "_II") out.warnings.add("Cena II selecionada; a Classic carrega apenas a cena ativa.");
    if (p.get("common.transposeEnable") && p.get("common.transposeValue"))
        out.warnings.add("Transposicao global nao aplicada: ajuste as oitavas/notas na Classic.");
    out.warnings.add("Efeitos e envelopes adaptados; morphs, arpejador e modulacoes complexas nao sao reproduzidos integralmente.");
    for (int group = 0; group < 3; ++group)
    {
        const std::string section = group == 0 ? "piano" : group == 1 ? "synth" : "organ";
        const std::string token = group == 0 ? "Pno" : group == 1 ? "Syn" : "Org";
        if (!p.get("common.sectionEnable" + token + scene)) continue;
        const auto enabled = p.get(section + ".layerEnable" + token + scene);
        for (int index = 0; index < (group == 1 ? 3 : 2); ++index)
        {
            if ((enabled & (1 << index)) == 0) continue;
            const char letter = char('A' + index);
            const auto prefix = section + letter;
            Part part; part.name = juce::String(section) + " " + juce::String::charToString(letter);
            routing(p, prefix, part.routing, out.warnings);
            part.routing.gain = percent(p.get(section + ".layerLevel" + token + letter, 100)) / 100.0f;
            if (part.routing.gain <= 0.0f)
            {
                out.warnings.addIfNotAlreadyThere("Partes com volume zero nao carregadas; morphs de volume nao sao aplicados.");
                continue;
            }
            if (group == 1)
            {
                const auto sound = prefix + "Sound";
                part.routing.mono = p.get(prefix + ".voiceMode") != 0;
                part.routing.attack = attackControl(envelopeMs(p.get(sound + ".ampEnvAtk")));
                part.routing.release = releaseControl(envelopeMs(p.get(sound + ".ampEnvRls")));
                part.routing.cutoff = p.get(sound + ".fltEnable") ? percent(p.get(sound + ".fltFreq", 127)) : 100.0f;
                part.routing.decayMs = p.get(sound + ".ampEnvDcy") >= 127 ? 0.0f : envelopeMs(p.get(sound + ".ampEnvDcy"));
                part.routing.sustainLevel = part.routing.decayMs > 0 ? 70.0f : 100.0f;
                if (p.get(prefix + ".arpRun")) out.warnings.addIfNotAlreadyThere("Arpejador do preset nao aplicado: a layer toca as notas recebidas.");
            }
            const auto fxPrefix = group == 2 ? "organFx" : prefix + "Fx";
            effects(p, fxPrefix, part.routing, out.warnings);
            if (group == 0 || (group == 1 && p.get(prefix + ".mode") == 0))
            {
                part.source = group == 0 ? Source::piano : Source::sample;
                const auto id = uint32_t(p.get(prefix + (group == 0 ? ".nameHash" : ".smpNameHash")));
                part.file = resolve(id, group == 0);
                if (!part.file.existsAsFile())
                    return juce::Result::fail(part.name + ": timbre referenciado nao encontrado no banco (ID "
                                              + juce::String::toHexString((juce::int64)id) + ").");
                part.category = part.file.getFullPathName().containsIgnoreCase("/Upright/") ? "Upright Piano"
                    : part.file.getFullPathName().containsIgnoreCase("/Grand/") ? "Piano Acustico" : "Piano Eletrico";
                if (group == 0)
                {
                    part.routing.pianoDynamicCompression = juce::jlimit(0, 3, int(p.get(prefix + ".dynComp")));
                    const auto eq = int(p.get(prefix + ".eq"));
                    part.routing.pianoTimbre = eq == 0 ? -1 : eq == 1 ? 0 : eq == 2 ? 1 : 2;
                    // Pedal banks are opt-in; pedalNoise is not equivalent to string resonance.
                }
            }
            else if (group == 1)
            {
                part.source = Source::analog;
                part.routing.polyphony = 64;
                out.warnings.addIfNotAlreadyThere("Sintetizadores Nord adaptados ao motor Moog da Classic (aproximacao sonora).");
                auto& a = part.analog;
                const auto sound = prefix + "Sound";
                const auto bank = int(p.get(sound + ".oscBank"));
                a.nordSynthType = bank == 0 ? 0 : bank == 1 ? 3 : 1;
                a.nordTimbre = int(p.get(sound + ".oscWave")); a.nordRetriggerSameNote = true;
                a.oscillator1Wave = a.oscillator2Wave = static_cast<AnalogSynthEngine::Waveform>(juce::jlimit(0, 4, int(p.get(sound + ".oscCat"))));
                a.oscillator3Enabled = false; a.oscillator2Level = p.get(prefix + ".oscUnison") ? 0.3f : 0.0f;
                a.oscillator1Semitones = float(p.get(sound + ".oscTuneCoarse"));
                a.oscillator1FineCents = float(p.get(sound + ".oscTuneFine"));
                a.oscillator2Semitones = a.oscillator1Semitones;
                a.cutoff = p.get(sound + ".fltEnable") ? percent(p.get(sound + ".fltFreq", 127)) : 100.0f;
                a.resonance = percent(p.get(sound + ".fltRes")) / 100.0f;
                a.filterEnvelopeAmount = percent(p.get(sound + ".fltEnvAmt")) / 100.0f;
                a.ampAttackMs = envelopeMs(p.get(sound + ".ampEnvAtk"));
                a.ampDecayMs = envelopeMs(p.get(sound + ".ampEnvDcy"));
                a.ampSustain = p.get(sound + ".ampEnvDcy") >= 127 ? 1.0f : 0.7f;
                a.ampReleaseMs = envelopeMs(p.get(sound + ".ampEnvRls"));
                a.filterAttackMs = envelopeMs(p.get(sound + ".fltEnvAtk"));
                a.filterDecayMs = envelopeMs(p.get(sound + ".fltEnvDcy"));
                a.filterReleaseMs = envelopeMs(p.get(sound + ".fltEnvRls"));
                a.lfoRateHz = 0.05f + percent(p.get(sound + ".lfoRateClkDiv")) * 0.1f;
                a.lfoToFilter = percent(p.get(sound + ".lfoAmt")) / 100.0f;
                a.monophonic = p.get(prefix + ".voiceMode") != 0;
                part.routing.mono = a.monophonic;
                part.routing.attack = attackControl(a.ampAttackMs);
                part.routing.release = juce::jlimit(0.0f, 100.0f, a.ampReleaseMs / 10.0f);
                part.routing.cutoff = a.cutoff;
                if (p.get(prefix + ".arpRun")) out.warnings.addIfNotAlreadyThere("Arpejador do preset nao aplicado: a layer toca as notas recebidas.");
            }
            else
            {
                part.routing.polyphony = 64;
                const auto model = juce::jlimit(0, 5, int(p.get(prefix + ".model")));
                part.source = model == 0 || model == 5 ? Source::hammond : Source::organ;
                part.organ.model = static_cast<NordOrganEngine::Model>(model);
                for (int bar = 0; bar < 9; ++bar)
                {
                    const auto key = prefix + "Drawbars.org_Preset_kDb" + std::to_string(bar+1);
                    const int value = juce::jlimit(0, 8, int(p.get(key)));
                    part.hammond.bars[size_t(bar)] = value; part.organ.drawbars[size_t(bar)] = value;
                }
                const auto perc = prefix + "VibPerc.org_Preset_kB3Perc";
                part.hammond.percussion = p.get(perc + "Enable") ? (p.get(perc + "Harmonic") ? 3 : 2) : 0;
                part.organ.percussion = part.hammond.percussion;
                part.hammond.leslie = part.organ.rotary = !p.get("organ.effectsLeslieEnable") ? 0
                    : p.get("organ.effectsLeslieSpeed") ? 2 : 1;
                part.hammond.drive = part.organ.drive = percent(p.get("organ.effectsLeslieDrv")) / 100.0f;
                if (model == 5) out.warnings.addIfNotAlreadyThere("B3 Bass adaptado ao Hammond da Classic.");
            }
            out.parts.push_back(std::move(part));
        }
    }
    if (out.parts.empty()) return juce::Result::fail("O programa nao tem partes ativas na cena selecionada.");
    return juce::Result::ok();
}
}
