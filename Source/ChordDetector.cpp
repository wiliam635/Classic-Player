#include "ChordDetector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <set>

namespace
{
constexpr std::array<const char*, 12> names { "C", "C#", "D", "Eb", "E", "F",
                                              "F#", "G", "Ab", "A", "Bb", "B" };

int pitchClass(int note) { return ((note % 12) + 12) % 12; }

struct Pattern
{
    std::vector<int> intervals;
    const char* suffix;
};

const std::vector<Pattern> chordPatterns {
    {{0,2,3,5,7,9,10}, "m7(9/11/13)"}, {{0,2,4,5,7,9,10}, "7(9/11/13)"},
    {{0,2,3,5,7,10}, "m7(9/11)"}, {{0,2,4,5,7,10}, "7(9/11)"},
    {{0,2,4,5,7,11}, "7M(9/11)"}, {{0,3,4,8,10}, "aug7(#9)"},
    {{0,4,7,8,10}, "7(#5)"}, {{0,2,3,7,9}, "m6(9)"},
    {{0,2,5,7,10}, "sus7(9)"}, {{0,2,4,7,10}, "9"},
    {{0,2,4,7,11}, "7M(9)"}, {{0,2,3,7,10}, "m7(9)"},
    {{0,3,5,10}, "m7(11)"}, {{0,4,7,10}, "7"}, {{0,4,7,11}, "7M"},
    {{0,3,7,10}, "m7"}, {{0,3,7,11}, "m(7M)"}, {{0,3,6,10}, "m7(b5)"},
    {{0,3,6,9}, "dim7"}, {{0,4,8,10}, "+7"}, {{0,5,7,10}, "sus7"},
    {{0,2,7,10}, "sus2(7)"}, {{0,2,5,7}, "sus4(9)"},
    {{0,4,7,9}, "6"}, {{0,3,7,9}, "m6"}, {{0,2,3,7}, "m9"},
    {{0,2,4,7}, "add9"}, {{0,4,5,7}, "add11"}, {{0,3,5,7}, "m(add11)"},
    {{0,4,7}, ""}, {{0,3,7}, "m"}, {{0,3,6}, "dim"}, {{0,4,8}, "+"},
    {{0,2,7}, "sus2"}, {{0,5,7}, "sus4"}, {{0,7}, "5"},
    {{0,2}, "sus2"}, {{0,5}, "sus4"}
};

const std::vector<Pattern> triadPatterns {
    {{0,4,7}, ""}, {{0,3,7}, "m"}, {{0,3,6}, "dim"},
    {{0,4,8}, "+"}, {{0,2,7}, "sus2"}, {{0,5,7}, "sus4"}
};

std::vector<int> pitchClasses(const std::vector<int>& notes, bool sorted = true)
{
    std::vector<int> result;
    for (const auto note : notes)
    {
        const auto pc = pitchClass(note);
        if (std::find(result.begin(), result.end(), pc) == result.end()) result.push_back(pc);
    }
    if (sorted) std::sort(result.begin(), result.end());
    return result;
}

std::vector<int> intervalsFor(const std::vector<int>& pcs, int root)
{
    std::vector<int> result;
    for (const auto pc : pcs) result.push_back((pc - root + 12) % 12);
    std::sort(result.begin(), result.end());
    return result;
}

struct Candidate { int root = 0; juce::String suffix; int score = 0; };

std::vector<Candidate> candidatesFor(const std::vector<int>& pcs, int bass)
{
    std::vector<Candidate> result;
    for (const auto root : pcs)
    {
        const auto intervals = intervalsFor(pcs, root);
        for (size_t index = 0; index < chordPatterns.size(); ++index)
        {
            const auto& pattern = chordPatterns[index];
            const auto exact = intervals == pattern.intervals;
            auto impliedFifth = false;
            if (!exact && std::find(intervals.begin(), intervals.end(), 7) == intervals.end())
            {
                auto completed = intervals;
                completed.push_back(7);
                std::sort(completed.begin(), completed.end());
                const auto hasThird = std::find(pattern.intervals.begin(), pattern.intervals.end(), 3) != pattern.intervals.end()
                                   || std::find(pattern.intervals.begin(), pattern.intervals.end(), 4) != pattern.intervals.end();
                const auto hasSeventh = std::find(pattern.intervals.begin(), pattern.intervals.end(), 10) != pattern.intervals.end()
                                     || std::find(pattern.intervals.begin(), pattern.intervals.end(), 11) != pattern.intervals.end();
                const auto alteredFifth = std::find(pattern.intervals.begin(), pattern.intervals.end(), 6) != pattern.intervals.end()
                                       || std::find(pattern.intervals.begin(), pattern.intervals.end(), 8) != pattern.intervals.end();
                impliedFifth = hasThird && hasSeventh && !alteredFifth && completed == pattern.intervals;
            }
            if (exact || impliedFifth)
                result.push_back({ root, pattern.suffix,
                    (root == bass ? 1000 : 500) + static_cast<int>(chordPatterns.size() - index)
                    - (impliedFifth ? 120 : 0) });
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    return result;
}

std::optional<Candidate> identifyTriad(const std::vector<int>& notes)
{
    const auto pcs = pitchClasses(notes);
    if (pcs.size() != 3 || notes.empty()) return std::nullopt;
    const auto bass = pitchClass(*std::min_element(notes.begin(), notes.end()));
    std::optional<Candidate> best;
    for (const auto root : pcs)
    {
        const auto intervals = intervalsFor(pcs, root);
        for (size_t index = 0; index < triadPatterns.size(); ++index)
            if (intervals == triadPatterns[index].intervals)
            {
                Candidate candidate { root, triadPatterns[index].suffix,
                                      (root == bass ? 100 : 0) + static_cast<int>(triadPatterns.size() - index) };
                if (!best || candidate.score > best->score) best = candidate;
            }
    }
    return best;
}

std::optional<juce::String> impliedMinorSixNine(const std::vector<int>& pcs, int bass)
{
    if (pcs.size() != 4) return std::nullopt;
    const auto root = (bass + 9) % 12;
    if (intervalsFor(pcs, root) == std::vector<int> {2,3,7,9})
        return juce::String(names[(size_t) root]) + "m6(9)/" + names[(size_t) bass];
    return std::nullopt;
}

std::optional<juce::String> upperStructure(const std::vector<int>& notes, int bass)
{
    std::vector<int> upper;
    std::set<int> seen;
    for (auto iterator = notes.rbegin(); iterator != notes.rend() && upper.size() < 3; ++iterator)
        if (seen.insert(pitchClass(*iterator)).second) upper.push_back(*iterator);
    std::sort(upper.begin(), upper.end());
    const auto triad = identifyTriad(upper);
    if (!triad || upper.empty()) return std::nullopt;

    const auto split = upper.front();
    std::vector<int> lower;
    std::copy_if(notes.begin(), notes.end(), std::back_inserter(lower), [split](int note) { return note < split; });
    const auto lowerPcs = pitchClasses(lower);
    const auto upperPcs = pitchClasses(upper);
    if (std::find(lowerPcs.begin(), lowerPcs.end(), bass) == lowerPcs.end() || triad->root == bass)
        return std::nullopt;
    const auto triadName = juce::String(names[(size_t) triad->root]) + triad->suffix;
    const auto handGap = lower.empty() ? 0 : split - lower.back();
    if (lowerPcs.size() == 1 && upperPcs.size() == 3)
        return handGap >= 7 ? std::optional<juce::String>(triadName + "/" + names[(size_t) bass])
                            : std::nullopt;

    const auto allPcs = pitchClasses(notes);
    const auto lowerIntervalsVector = intervalsFor(lowerPcs, bass);
    const auto allIntervalsVector = intervalsFor(allPcs, bass);
    const std::set<int> lowerIntervals(lowerIntervalsVector.begin(), lowerIntervalsVector.end());
    const std::set<int> allIntervals(allIntervalsVector.begin(), allIntervalsVector.end());
    if (lowerIntervals.size() == 2 && lowerIntervals.contains(0) && lowerIntervals.contains(7)
        && triad->root == (bass + 7) % 12)
        return triadName + "/" + names[(size_t) bass];
    if (!lowerIntervals.contains(7) && !lowerIntervals.contains(3) && !lowerIntervals.contains(4))
        return std::nullopt;

    const auto quality = lowerIntervals.contains(3) ? juce::String("m")
                       : lowerIntervals.contains(4) ? juce::String{}
                       : allIntervals.contains(5) ? juce::String("sus") : juce::String{};
    const auto seventh = allIntervals.contains(10) ? juce::String("7")
                       : allIntervals.contains(11) ? juce::String("7M") : juce::String{};
    const std::array<std::pair<int, const char*>, 7> tensionMap {{
        {1,"b9"}, {2,"9"}, {3,"#9"}, {5,"11"}, {6,"#11"}, {8,"b13"}, {9,"13"}
    }};
    juce::StringArray tensions;
    for (const auto& [interval, label] : tensionMap)
        if (allIntervals.contains(interval) && !(quality == "m" && interval == 3)
            && !(quality == "sus" && interval == 5)) tensions.add(label);
    return juce::String(names[(size_t) bass]) + quality + seventh
         + (tensions.isEmpty() ? juce::String{} : "(" + tensions.joinIntoString("/") + ")");
}
}

juce::String ClassicChordDetector::formatAccidentals(const juce::String& chord,
                                                   AccidentalStyle style)
{
    auto result = chord;
    switch (style)
    {
        case AccidentalStyle::mixed:
            break;
        case AccidentalStyle::sharp:
            result = result.replace("Eb", "D#").replace("Ab", "G#").replace("Bb", "A#");
            break;
        case AccidentalStyle::flat:
            result = result.replace("C#", "Db").replace("F#", "Gb");
            break;
    }
    return result;
}

juce::String ClassicChordDetector::detect(const std::vector<int>& midiNotes)
{
    auto notes = midiNotes;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](int note) { return note < 0 || note > 127; }), notes.end());
    std::sort(notes.begin(), notes.end());
    if (notes.empty()) return "-";
    const auto bass = pitchClass(notes.front());
    const auto pcs = pitchClasses(notes);
    if (pcs.size() == 1) return names[(size_t) bass];
    if (const auto implied = impliedMinorSixNine(pcs, bass)) return *implied;
    const auto upper = upperStructure(notes, bass);
    const auto candidates = candidatesFor(pcs, bass);
    if (!candidates.empty())
    {
        const auto& chord = candidates.front();
        if ((chord.suffix == "add9" || chord.suffix == "add11") && upper) return *upper;
        return juce::String(names[(size_t) chord.root]) + chord.suffix
             + (chord.root != bass ? "/" + juce::String(names[(size_t) bass]) : juce::String{});
    }
    if (upper) return *upper;
    const auto orderedPcs = pitchClasses(notes, false);
    juce::StringArray fallback;
    for (const auto pc : orderedPcs) fallback.add(names[(size_t) pc]);
    return fallback.joinIntoString(" + ");
}
