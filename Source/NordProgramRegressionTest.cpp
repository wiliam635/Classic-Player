#include "NordProgramDecoder.h"
#include "NordProgramSchema.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cmath>

namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
void le(std::vector<std::uint8_t>& b, int offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b[size_t(offset + i)] = std::uint8_t(value >> (8 * i));
}
void checksum(std::vector<std::uint8_t>& b) {
    std::uint32_t crc = 0xffffffffu;
    for (size_t i = 44; i < b.size(); ++i) {
        crc ^= b[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    le(b, 24, crc ^ 0xffffffffu);
}
void field(std::vector<std::uint8_t>& b, const std::string& group, const std::string& name,
           std::uint32_t value, int part = 0) {
    for (const auto& f : nord_program_schema::fields) if (group == f.group && name == f.name) {
        check(f.first[part] >= 0, "missing part");
        for (int bit = f.last[part]; bit >= f.first[part]; --bit) {
            const auto mask = std::uint8_t(1 << (7 - bit % 8));
            b[size_t(bit / 8)] = std::uint8_t((b[size_t(bit / 8)] & ~mask) | ((value & 1) ? mask : 0));
            value >>= 1;
        }
        return;
    }
    throw std::runtime_error("Unknown field: " + group + " " + name);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(868);
    for (int i = 0; i < 4; ++i) { b[size_t(i)] = std::uint8_t("CBIN"[i]); b[size_t(i + 8)] = std::uint8_t("ns4p"[i]); }
    le(b, 4, 1); le(b, 20, 308); b[46] = 1; b[47] = 52;
    field(b, "m", "piano section on/off", 1);
    field(b, "p", "layer on/off", 1);
    field(b, "p", "volume", 127);
    field(b, "p", "KB zones", 15);
    field(b, "p", "susped on/off", 1);
    field(b, "p", "piano model ID/name", 3917934714u);
    checksum(b); return b;
}
}
int main(int argc, char** argv) {
    try {
        auto b = fixture(); NordProgramDecoder::Program p; std::string error;
        check(NordProgramDecoder::decode(b, p, error), error.c_str());
        check(p.pianos[0].pianoName == "Royal Grand 3D XL", "piano identity");
        check(p.pianos[0].gain == 1 && p.pianos[0].sustain, "gain/sustain");
        check(p.playable(0, error) && !p.playable(1, error), "scene isolation");
        check(!p.pianos[1].enabled[0], "inactive dependency must not play");
        p.split = true; p.splitEnabled = {true, false, true}; p.splitNotes = {48, 60, 72};
        auto part = p.pianos[0]; part.zones = 4;
        check(!p.accepts(part, 47) && p.accepts(part, 48) && p.accepts(part, 71) && !p.accepts(part, 72), "split ranges");
        p.sections[0][2] = true; p.synths[0].enabled[0]=true;p.synths[0].gain=1;p.synths[0].synthType=1;p.synths[0].sample=false;
        check(!p.playable(0, error), "unsupported FM must reject partial playback");
        b[800] ^= 1; check(!NordProgramDecoder::decode(b, p, error), "corrupt body accepted");
        b = fixture(); b.pop_back(); check(!NordProgramDecoder::decode(b, p, error), "truncated body accepted");
        b = fixture(); le(b, 20, 399); check(!NordProgramDecoder::decode(b, p, error), "unknown version accepted");
        b = fixture(); b[8] = 'x'; check(!NordProgramDecoder::decode(b, p, error), "wrong format accepted");
        if (argc > 1) {
            int count = 0, playable = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
                if (entry.path().extension() != ".ns4p") continue;
                std::ifstream file(entry.path(), std::ios::binary);
                std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), {});
                check(NordProgramDecoder::decode(data, p, error), error.c_str());
                const auto ok = p.playable(0, error);
                ++count; if (ok) ++playable;
                std::cout << entry.path().filename().string() << " | " << (ok ? "PIANO/B3" : error) << '\n';
                if (entry.path().filename() == "Gordo Pno FLP.ns4p") {
                    check(p.pianos[0].pianoId == 3917934714u && p.pianos[1].pianoId == 3565051066u, "Gordo IDs");
                    check(p.pianos[0].gain == 1 && std::abs(20 * std::log10(p.pianos[1].gain) + 8.5) < 0.15, "Gordo levels");
                }
            }
            std::cout<<"Compatible scene I programs: "<<playable<<'\n';
            check(count == 30 && playable > 7 && playable < 30, "fixture catalog count differs");
        }
        std::cout << "Nord program regression passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
