#include "NordProgramDecoder.h"
#include "NordProgramSchema.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
std::uint32_t little32(const std::uint8_t* b)
{ return b[0] | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24); }
std::uint32_t crc(const std::uint8_t* bytes, size_t size)
{
    std::uint32_t result = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        result ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ ((result & 1u) ? 0xedb88320u : 0u);
    }
    return result ^ 0xffffffffu;
}
std::uint32_t read(const std::vector<std::uint8_t>& bytes, const char* group,
                   const std::string& name, int part = 0)
{
    for (const auto& field : nord_program_schema::fields)
        if (name == field.name && std::strcmp(group, field.group) == 0) {
            if (part < 0 || part >= 3 || field.first[part] < 0) return 0;
            std::uint32_t value = 0;
            for (int bit = field.first[part]; bit <= field.last[part]; ++bit)
                value = (value << 1) | ((bytes[size_t(bit / 8)] >> (7 - bit % 8)) & 1);
            return value;
        }
    return 0;
}
float eq(std::uint32_t v) { return float(v < 64 ? int(v) : int(v) - 128) * 0.25f; }
}

bool NordProgramDecoder::Program::playable(int selectedScene, std::string& reason) const
{
    if (selectedScene < 0 || selectedScene > 1) { reason = "Cena invalida."; return false; }
    const auto& active = sections[size_t(selectedScene)];
    if (active[2]) { reason = "Este programa usa synth/NSMP4; a reproducao dessa secao ainda nao esta disponivel."; return false; }
    bool any = false;
    for (const auto& p : pianos) any |= active[0] && p.enabled[size_t(selectedScene)] && p.gain > 0;
    for (const auto& o : organs) if (active[1] && o.enabled[size_t(selectedScene)] && o.gain > 0) {
        if (o.model != 0) { reason = "Este programa usa um modelo de orgao diferente de B3, ainda nao suportado."; return false; }
        any = true;
    }
    reason = any ? "" : "Esta cena nao possui partes de piano ou B3 ativas.";
    return any;
}

bool NordProgramDecoder::Program::accepts(const Part& part, int note) const
{
    if (!split) return true;
    int first = 0, last = 3;
    for (size_t i = 0; i < splitNotes.size(); ++i)
        if (splitEnabled[i]) {
            if (note >= splitNotes[i]) first = int(i) + 1;
            else { last = int(i); break; }
        }
    int mask = 0;
    for (int zone = first; zone <= last; ++zone) mask |= 1 << (3 - zone);
    return (part.zones & mask) != 0;
}

bool NordProgramDecoder::decode(const std::vector<std::uint8_t>& bytes,
                               Program& result, std::string& error)
{
    result = {};
    error.clear();
    const auto fail = [&](const char* text) { error = text; return false; };
    if (bytes.size() != 868 || std::memcmp(bytes.data(), "CBIN", 4) != 0
        || std::memcmp(bytes.data() + 8, "ns4p", 4) != 0 || little32(bytes.data() + 4) != 1)
        return fail("Programa NS4 invalido: cabecalho ou tamanho nao suportado.");
    const auto version = int(little32(bytes.data() + 20));
    if (version != 308)
        return fail("Versao do programa NS4 ainda nao validada (esperado 3.08).");
    const auto bodyVersion = (std::uint32_t(bytes[44]) << 24) | (std::uint32_t(bytes[45]) << 16)
                           | (std::uint32_t(bytes[46]) << 8) | bytes[47];
    if (bodyVersion != std::uint32_t(version) || crc(bytes.data() + 44, bytes.size() - 44) != little32(bytes.data() + 24))
        return fail("O checksum ou a versao interna do programa NS4 nao confere.");
    result.version = version;
    result.scene = int(read(bytes, "m", "which layer scene is active"));
    result.split = read(bytes, "m", "split on/off") != 0;
    for (int i = 0; i < 3; ++i) {
        const auto prefix = "KB zones " + std::to_string(i + 1) + "-" + std::to_string(i + 2) + " split point";
        const auto position = int(read(bytes, "m", prefix));
        result.splitEnabled[size_t(i)] = read(bytes, "m", prefix + " on/off") != 0;
        if (position > 10) return fail("Posicao de split NS4 invalida.");
        result.splitNotes[size_t(i)] = 36 + (position / 2) * 12 + (position % 2) * 5;
    }
    if (read(bytes, "m", "program transpose on/off")) {
        const auto v = int(read(bytes, "m", "program transpose amount"));
        result.transpose = v < 8 ? v : v - 16;
    }
    for (int scene = 0; scene < 2; ++scene)
        for (int section = 0; section < 3; ++section) {
            const char* names[] { "piano", "organ", "synth" };
            result.sections[size_t(scene)][size_t(section)] = read(bytes, "m",
                std::string(names[section]) + " section on/off" + (scene ? " (layer scene II)" : "")) != 0;
        }
    result.leslie = !read(bytes, "m", "organ rotary spkr on/off") || read(bytes, "m", "rotary spkr stop on/off")
        ? 0 : read(bytes, "m", "rotary spkr slow/fast") ? 2 : 1;
    result.organDrive = float(read(bytes, "m", "rotary spkr drive")) / 127.0f;
    for (int part = 0; part < 2; ++part)
        for (int group = 0; group < 2; ++group) {
            const auto* key = group == 0 ? "p" : "o";
            auto& p = group == 0 ? result.pianos[size_t(part)] : result.organs[size_t(part)];
            for (int scene = 0; scene < 2; ++scene)
                p.enabled[size_t(scene)] = read(bytes, key, scene ? "layer on/off (scene II)" : "layer on/off", part) != 0;
            const auto volume = float(read(bytes, key, "volume", part)) / 127.0f;
            p.gain = volume * volume; // Nord volume is 40*log10(raw/127) dB.
            const auto oct = int(read(bytes, key, "octave shift", part));
            p.octave = oct < 8 ? oct : oct - 16;
            p.zones = int(read(bytes, key, "KB zones", part));
            p.sustain = read(bytes, key, group == 0 ? "susped on/off" : "organ susped on/off", part) != 0;
            const auto* fxGroup = group == 0 ? "p" : "m";
            const auto fxPart = group == 0 ? part : 0;
            const auto fxPrefix = std::string(group == 0 ? "FX " : "organ FX ");
            if (read(bytes, fxGroup, fxPrefix + "reverb on/off", fxPart))
                p.reverb = float(read(bytes, fxGroup, fxPrefix + "reverb amount", fxPart)) / 127.0f * 100.0f;
            if (read(bytes, fxGroup, fxPrefix + "amp sim/EQ on/off", fxPart)) {
                p.eqLow = eq(read(bytes, fxGroup, fxPrefix + "amp sim/EQ bass", fxPart));
                p.eqMid = eq(read(bytes, fxGroup, fxPrefix + "amp sim/EQ mid", fxPart));
                p.eqHigh = eq(read(bytes, fxGroup, fxPrefix + "amp sim/EQ treb", fxPart));
            }
            if (group == 0) {
                p.model = int(read(bytes, "p", "piano type", part));
                p.pianoId = read(bytes, "p", "piano model ID/name", part);
                for (const auto& piano : nord_program_schema::pianos)
                    if (piano.id == p.pianoId) { p.pianoName = piano.name; break; }
                const auto timbre = int(read(bytes, "p", "timbre", part));
                const auto acoustic = p.model != 2;
                p.timbre = acoustic ? ((timbre % 4) - 1)
                    : timbre == 7 ? 0 : timbre >= 1 && timbre <= 3 ? timbre - 1 : -1;
                p.touch = int(read(bytes, "p", "touch", part));
            } else {
                p.model = int(read(bytes, "o", "organ model", part));
                for (int bar = 0; bar < 9; ++bar)
                    p.drawbars[size_t(bar)] = std::clamp(int(read(bytes, "o", "drawbar " + std::to_string(bar + 1), part)), 0, 8);
                p.percussion = !read(bytes, "o", "percussion on/off", part) ? 0
                    : read(bytes, "o", "perc harm 3rd on/off", part) ? 3 : 2;
            }
        }
    result.warnings.push_back("Pianos/B3 e efeitos adaptados aos motores Classic Player; DSP, morphs e efeitos Nord nao sao identicos.");
    return true;
}
