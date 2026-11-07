#pragma once
#include "NordProgram.h"
#include "NordBundlePolicy.h"
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
    int scene = 0;
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
    c.nordSplit = {};
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
        return;
    }
    const auto width = [&](int boundary)
    {
        const auto raw = p.get(std::string("common.splitWidth") + suffix[boundary]);
        return raw == 1 ? 6 : raw == 2 ? 12 : 0;
    };
    if (first > 0)
    {
        c.nordSplit.lowPoint = boundaries[first];
        c.nordSplit.lowWidth = width(first-1);
        c.lowNote = juce::jmax(0, c.lowNote - c.nordSplit.lowWidth);
    }
    if (last < 3)
    {
        c.nordSplit.highPoint = boundaries[last+1];
        c.nordSplit.highWidth = width(last);
        c.highNote = juce::jmin(127, c.highNote + c.nordSplit.highWidth);
    }
}
inline void effects(const NordProgram::Program& p, const std::string& prefix,
                    Sf2Engine::LayerConfig& c, juce::StringArray& warnings)
{
    const auto get = [&](const char* key, int64_t fallback = 0) { return p.get(prefix + "." + key, fallback); };
    const auto morph = [&](const char* key, float signedScale = 127.0f)
    {
        NordMorphDepth depth;
        const auto name = prefix + "." + key;
        depth.wheel = juce::jlimit(-signedScale, signedScale, float(p.get(name + ".morphWheel"))) / signedScale;
        depth.pressure = juce::jlimit(-signedScale, signedScale, float(p.get(name + ".morphAfterTouch"))) / signedScale;
        depth.pedal = juce::jlimit(-signedScale, signedScale, float(p.get(name + ".morphCtrlPedal"))) / signedScale;
        return depth;
    };
    c.nordMorph.tremoloAmount = morph("fx1Amt");
    c.nordMorph.tremoloRate = morph("fx1RateClkDiv");
    c.nordMorph.flangerAmount = morph("fx2Amt");
    c.nordMorph.flangerRate = morph("fx2Rate");
    c.nordMorph.chorusAmount = morph("fx2Amt");
    c.nordMorph.chorusRate = morph("fx2Rate");
    c.nordMorph.modulationAmount = morph("fx2Amt");
    c.nordMorph.modulationRate = morph("fx2Rate");
    c.nordMorph.delayMix = morph("dlyMix");
    c.nordMorph.delayFeedback = morph("dlyFbkAmt");
    c.nordMorph.delayRate = morph("dlyRateClkDiv", 16383.0f);
    c.nordMorph.ampDrive = morph("ampDrive");
    c.nordMorph.ampMidFrequency = morph("ampMidFreq");
    c.nordMorph.reverb = morph("rvbAmt");
    if (get("fx1Enable"))
    {
        if (NordBundlePolicy::isTremolo(get("fx1Type")))
        { c.nordFx.tremoloMix = percent(get("fx1Amt")); c.nordFx.tremoloRate = 0.1f + percent(get("fx1RateClkDiv")) * 0.12f; }
        else warnings.addIfNotAlreadyThere("Modulacao 1 diferente de tremolo: ajuste manual necessario.");
    }
    if (get("fx2Enable"))
    {
        const auto type = NordBundlePolicy::modulation2(get("fx2Type"));
        if (type == NordBundlePolicy::Modulation2::chorus)
        {
            c.nordFx.chorusMix = percent(get("fx2Amt"));
            c.nordFx.chorusDepth = NordBundlePolicy::secondVariant(get("fx2Type")) ? 10.0f : 6.0f;
        }
        else if (type == NordBundlePolicy::Modulation2::flanger)
            c.nordFx.flangerMix = percent(get("fx2Amt"));
        else
        {
            c.nordFx.modulationType = type == NordBundlePolicy::Modulation2::phaser ? 1
                : type == NordBundlePolicy::Modulation2::vibe ? 2
                : type == NordBundlePolicy::Modulation2::ensemble ? 3
                : type == NordBundlePolicy::Modulation2::spin ? 4 : 0;
            c.nordFx.modulationMix = percent(get("fx2Amt"));
            c.nordFx.modulationRate = 0.05f + percent(get("fx2Rate")) * 0.05f;
            if (c.nordFx.modulationType == 0)
                warnings.addIfNotAlreadyThere("Tipo de modulacao 2 desconhecido.");
        }
        c.nordFx.chorusRate = c.nordFx.flangerRate = 0.05f + percent(get("fx2Rate")) * 0.05f;
    }
    if (get("dlyEnable"))
    {
        c.nordFx.delayMix = percent(get("dlyMix"));
        c.nordFx.delayFeedback = juce::jmin(90.0f, percent(get("dlyFbkAmt")));
        c.nordFx.delayBpm = juce::jlimit(30.0f, 300.0f, float(p.get("common.masterClockRate", 120)));
        c.nordFx.delayRate = juce::jlimit(0.0f, 1.0f, float(get("dlyRateClkDiv")) / 16383.0f);
        c.nordFx.delaySync = get("dlyClkEnable") != 0;
        c.nordFx.delayPingPong = get("dlyFbkPingPong") != 0;
        c.nordFx.delayFeedbackFilter = juce::jlimit(0, 3, int(get("dlyFbkFilt")));
        c.nordFx.delayAnalog = get("dlyMode") != 0;
        if (get("dlyFbFxType") != 0)
            warnings.addIfNotAlreadyThere("Efeito adicional no feedback do delay não tem equivalente direto.");
        warnings.addIfNotAlreadyThere("Delay adaptado: sincronismo, ping-pong e filtros são aproximações da WX.");
    }
    if (get("rvbEnable"))
    {
        c.reverb = percent(get("rvbAmt"));
        c.reverbSize = juce::jlimit(15.0f, 95.0f, 25.0f + 12.0f * float(juce::jlimit<int64_t>(0, 11, get("rvbType")) / 2));
    }
    if (get("cmpEnable")) c.compressor = percent(get("cmpAmt"));
    if (get("ampEnable"))
    {
        c.nordFx.ampEnabled = true;
        c.nordFx.ampModel = juce::jlimit(0, 2, int(get("ampTypeNew")));
        c.nordFx.ampDrive = percent(get("ampDrive"));
        c.eqLow = NordBundlePolicy::eqDecibels(get("ampBass"));
        c.eqMid = NordBundlePolicy::eqDecibels(get("ampMid"));
        c.eqHigh = NordBundlePolicy::eqDecibels(get("ampTreble"));
        c.eqMidFrequency = 250.0f * std::pow(24.0f, float(get("ampMidFreq")) / 127.0f);
        warnings.addIfNotAlreadyThere("Simulação de amplificador aproximada; modelos e resposta não são idênticos à WX/Nord.");
    }
}

inline std::uint8_t arpPatternPair(std::int64_t packed, int step) noexcept
{
    const auto bits = static_cast<std::uint32_t>(packed);
    return static_cast<std::uint8_t>((bits >> (30 - 2 * juce::jlimit(0, 15, step))) & 0x3u);
}

inline void arpeggiator(const NordProgram::Program& p, const std::string& prefix,
                        Sf2Engine::LayerConfig& routing)
{
    auto& arp = routing.nordArp;
    arp = {};
    arp.enabled = p.get(prefix + ".arpRun") != 0;
    if (!arp.enabled) return;

    arp.mode = juce::jlimit(0, 2, static_cast<int>(p.get(prefix + ".arpMode")));
    arp.direction = juce::jlimit(0, 3, static_cast<int>(p.get(prefix + ".arpDir")));
    arp.zigZag = p.get(prefix + ".arpZigZag") != 0;
    arp.keyboardSync = p.get(prefix + ".arpKbdSync") != 0;
    arp.syncToClock = p.get(prefix + ".arpClkEnable") != 0;
    arp.rate = juce::jlimit(0.0f, 1.0f,
        static_cast<float>(p.get(prefix + ".arpRateClkDiv")) / 127.0f);
    arp.bpm = juce::jlimit(30.0f, 300.0f, static_cast<float>(p.get("common.masterClockRate", 120)));
    arp.rangeSemitones = juce::jlimit(0, 48, juce::roundToInt(
        static_cast<float>(p.get(prefix + ".arpRange")) * 48.0f / 127.0f));
    arp.patternEnabled = p.get(prefix + ".arpPtnEnable") != 0;
    arp.patternLength = juce::jlimit(1, 16, static_cast<int>(p.get(prefix + ".arpPtnLen", 16)));
    if (!arp.patternEnabled) return;

    const auto accents = p.get(prefix + ".arpPtnAcc");
    const auto durations = p.get(prefix + ".arpPtnDur");
    const auto pans = p.get(prefix + ".arpPtnPan");
    for (int step = 0; step < 16; ++step)
    {
        // WX tests only bit 0 of each packed accent value. Values 2 and 3
        // must not be treated as accented merely because they are nonzero.
        arp.patternAccent[(size_t) step] = (arpPatternPair(accents, step) & 1u) != 0 ? 1 : 0;
        arp.patternDuration[(size_t) step] = arpPatternPair(durations, step);
        const auto pan = arpPatternPair(pans, step);
        // NS4P stores each pan as a signed two-bit value; WX pattern text
        // serializes negative values as L, positive as R, and zero as C.
        arp.patternPan[(size_t) step] = pan == 0 ? 0 : pan == 1 ? 1 : -1;
    }
}

inline juce::Result build(const juce::File& folder, const juce::String& path, Prepared& out, int requestedScene = -1)
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
    out.scene = requestedScene < 0 ? int(p.get("common.layerScene") != 0) : juce::jlimit(0, 1, requestedScene);
    const std::string scene = out.scene ? "_II" : "_I";
    if (p.get("common.transposeEnable") && p.get("common.transposeValue"))
        out.warnings.add("Transposicao global nao aplicada: ajuste as oitavas/notas na Classic.");
    out.warnings.add("Morphs de volume, filtro, efeitos, LFO, arpejador e drawbars adaptados; destinos discretos continuam aproximados.");
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
            const auto readMorph = [&](const std::string& key)
            {
                NordMorphDepth depth;
                depth.wheel = juce::jlimit(-127.0f, 127.0f, float(p.get(key + ".morphWheel"))) / 127.0f;
                depth.pressure = juce::jlimit(-127.0f, 127.0f, float(p.get(key + ".morphAfterTouch"))) / 127.0f;
                depth.pedal = juce::jlimit(-127.0f, 127.0f, float(p.get(key + ".morphCtrlPedal"))) / 127.0f;
                return depth;
            };
            part.routing.nordMorph.volume = readMorph(section + ".layerLevel" + token + letter);
            if (group == 1) part.routing.nordMorph.cutoff = readMorph(prefix + "Sound.fltFreq");
            if (part.routing.gain <= 0.0f)
            {
                const auto levelKey = section + ".layerLevel" + token + letter;
                const bool hasVolumeMorph = p.get(levelKey + ".morphWheel") != 0
                    || p.get(levelKey + ".morphAfterTouch") != 0
                    || p.get(levelKey + ".morphCtrlPedal") != 0;
                if (!hasVolumeMorph) continue;
                // Keep zero-level sources: volume morph can make them audible.
            }
            if (group == 1)
            {
                const auto sound = prefix + "Sound";
                part.routing.nordMorph.synthOscControl = readMorph(sound + ".oscCtrl");
                part.routing.nordMorph.synthOscEnvelope = readMorph(sound + ".oscEnvAmt");
                arpeggiator(p, prefix, part.routing);
                part.routing.nordMorph.synthResonance = readMorph(sound + ".fltRes");
                part.routing.nordMorph.synthFilterEnvelope = readMorph(sound + ".fltEnvAmt");
                part.routing.nordMorph.synthLfoAmount = readMorph(sound + ".lfoAmt");
                part.routing.nordMorph.synthLfoRate = readMorph(sound + ".lfoRateClkDiv");
                part.routing.nordMorph.arpRate = readMorph(prefix + ".arpRateClkDiv");
                part.routing.nordMorph.arpRange = readMorph(prefix + ".arpRange");
                for (int external = 1; external <= 2; ++external)
                {
                    const auto externalPrefix = prefix + ".externCCValue" + std::to_string(external);
                    if (p.get(externalPrefix + ".morphWheel") != 0
                        || p.get(externalPrefix + ".morphAfterTouch") != 0
                        || p.get(externalPrefix + ".morphCtrlPedal") != 0)
                    {
                        out.warnings.addIfNotAlreadyThere(
                            "Morph de CC externo reconhecido, mas o Classic ainda nao transmite MIDI CC de saida.");
                        break;
                    }
                }
                part.routing.mono = p.get(prefix + ".voiceMode") != 0;
                part.routing.attack = attackControl(envelopeMs(p.get(sound + ".ampEnvAtk")));
                part.routing.release = releaseControl(envelopeMs(p.get(sound + ".ampEnvRls")));
                part.routing.cutoff = p.get(sound + ".fltEnable") ? percent(p.get(sound + ".fltFreq", 127)) : 100.0f;
                part.routing.decayMs = p.get(sound + ".ampEnvDcy") >= 127 ? 0.0f : envelopeMs(p.get(sound + ".ampEnvDcy"));
                part.routing.sustainLevel = part.routing.decayMs > 0 ? 70.0f : 100.0f;
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
                    part.routing.pianoReleaseSamplesEnabled = part.category == "Upright Piano";
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
                a.nordOscControl = juce::jlimit(0.0f, 127.0f, float(p.get(sound + ".oscCtrl", 64)));
                a.nordOscEnvelopeAmount = juce::jlimit(-1.0f, 1.0f,
                    float(p.get(sound + ".oscEnvAmt")) / 60.0f);
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
                    auto& depth = part.routing.nordMorph.drawbars[(size_t) bar];
                    depth.wheel = juce::jlimit(-8.0f, 8.0f, float(p.get(key + ".morphWheel"))) / 8.0f;
                    depth.pressure = juce::jlimit(-8.0f, 8.0f, float(p.get(key + ".morphAfterTouch"))) / 8.0f;
                    depth.pedal = juce::jlimit(-8.0f, 8.0f, float(p.get(key + ".morphCtrlPedal"))) / 8.0f;
                }
                const auto perc = prefix + "VibPerc.org_Preset_kB3Perc";
                part.hammond.percussion = p.get(perc + "Enable") ? (p.get(perc + "Harmonic") ? 3 : 2) : 0;
                part.organ.percussion = part.hammond.percussion;
                part.hammond.leslie = part.organ.rotary = !p.get("organ.effectsLeslieEnable") ? 0
                    : p.get("organ.effectsLeslieSpeed") ? 2 : 1;
                part.routing.nordMorph.leslieSpeed.wheel = juce::jlimit(-1.0f, 1.0f,
                    float(p.get("organ.effectsLeslieSpeed.morphWheel")) / 2.0f);
                part.routing.nordMorph.leslieSpeed.pressure = juce::jlimit(-1.0f, 1.0f,
                    float(p.get("organ.effectsLeslieSpeed.morphAfterTouch")) / 2.0f);
                part.routing.nordMorph.leslieSpeed.pedal = juce::jlimit(-1.0f, 1.0f,
                    float(p.get("organ.effectsLeslieSpeed.morphCtrlPedal")) / 2.0f);
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
