#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace ClassicChordDetector
{
juce::String detect(const std::vector<int>& midiNotes);
}
