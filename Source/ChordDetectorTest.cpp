#include "ChordDetector.h"
#include <iostream>

int main()
{
    struct Test { const char* name; std::vector<int> notes; const char* expected; };
    const std::vector<Test> tests {
        {"A/D duplicated D", {38,50,57,61,64}, "A/D"},
        {"A/D D-A", {38,45,57,61,64}, "A/D"},
        {"Bm7(9)", {35,47,50,54,57,61}, "Bm7(9)"},
        {"B7(#5)", {35,42,57,63,67}, "B7(#5)"},
        {"F#m7(11)", {42,54,57,59,64}, "F#m7(11)"},
        {"Gaug7(#9)/F", {41,47,63,67,70}, "Gaug7(#9)/F"},
        {"Fm9", {41,48,56,60,67}, "Fm9"},
        {"Fm6(9)", {41,48,56,60,62,67}, "Fm6(9)"},
        {"Fm6(9)/Ab", {44,56,60,62,67}, "Fm6(9)/Ab"},
        {"Bm7", {47,50,54,57}, "Bm7"},
        {"Csus2", {48,50,55}, "Csus2"},
        {"Csus4", {48,53,55}, "Csus4"},
        {"Gsus7(9)", {43,50,53,57,60}, "Gsus7(9)"},
        {"F/G", {43,53,57,60}, "F/G"},
        {"Dm7(9/11)", {50,53,57,60,64,67}, "Dm7(9/11)"}
    };
    auto failures = 0;
    for (const auto& test : tests)
    {
        const auto actual = ClassicChordDetector::detect(test.notes);
        if (actual != test.expected)
        {
            std::cerr << test.name << ": expected " << test.expected
                      << ", got " << actual << "\n";
            ++failures;
        }
    }
    std::cout << (tests.size() - static_cast<size_t>(failures)) << "/" << tests.size()
              << " chord tests passed\n";
    return failures == 0 ? 0 : 1;
}
