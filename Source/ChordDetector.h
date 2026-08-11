#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace ClassicChordDetector
{
enum class AccidentalStyle { mixed, sharp, flat };

juce::String detect(const std::vector<int>& midiNotes);
juce::String formatAccidentals(const juce::String& chord, AccidentalStyle style);
}
