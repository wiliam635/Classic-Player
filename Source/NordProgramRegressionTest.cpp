#include "NordProgram.h"
#include <iostream>
#include <stdexcept>
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static std::vector<uint8_t> fixture()
{
    std::vector<uint8_t> b(48, 0);
    std::memcpy(b.data(), "CBIN", 4); std::memcpy(b.data()+8, "ns4p", 4);
    for (int block = 0; block < 24; ++block)
    { b.push_back(128); b.insert(b.end(), 128, 0); }
    return b;
}
static void bits(std::vector<uint8_t>& b, size_t block, size_t offset, unsigned width, uint32_t value)
{
    for (unsigned i = 0; i < width; ++i)
    {
        const auto at = (49 + 129 * block) * 8 + offset + i;
        const uint8_t mask = uint8_t(1u << (7 - at % 8));
        if ((value >> (width-1-i)) & 1u) b[at/8] |= mask;
        else b[at/8] &= uint8_t(~mask);
    }
}
int main()
{
    try
    {
        auto b = fixture();
        // Independent format coordinates exercise bit crossings, signed values,
        // 32-bit resource IDs and the length-prefixed block boundaries.
        bits(b, 0, 27, 8, 90); bits(b, 0, 282, 1, 1);
        bits(b, 10, 4, 4, 14); bits(b, 10, 20, 32, 0xf7654321u);
        bits(b, 14, 2, 3, 5);
        NordProgram::Program p; std::string error;
        require(NordProgram::read(b.data(), b.size(), p, error), "valid layout refused");
        require(p.get("common.masterClockRate") == 120, "tempo bias or crossing wrong");
        require(p.get("common.layerScene") == 1, "scene decoded incorrectly");
        require(p.get("pianoA.octaveShift") == -2, "signed octave is not sign-extended");
        require(p.get("pianoA.nameHash") == 0xf7654321LL, "unsigned 32-bit identifier lost");
        require(p.get("synth.layerEnableSyn_I") == 5, "part enable mask lost");
        require(p.get("missing", 27) == 27, "missing-field default lost");
        require(NordProgram::soundId("Amber Upright XL 6.2") == 320440865u, "resource hash golden mismatch");
        auto bad = b; bad.pop_back();
        require(!NordProgram::read(bad.data(),bad.size(),p,error) && p.values.empty(), "truncated program accepted");
        bad = b; bad.resize(bad.size()-129);
        require(!NordProgram::read(bad.data(),bad.size(),p,error), "23-block layout accepted");
        bad = b; bad.push_back(0);
        require(!NordProgram::read(bad.data(),bad.size(),p,error), "25-block layout accepted");
        bad = b; bad[8] = 'x';
        require(!NordProgram::read(bad.data(),bad.size(),p,error), "wrong program format accepted");
        require(!NordProgram::read(nullptr,0,p,error), "null program accepted");
        std::cout << "Nord program regression checks passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
