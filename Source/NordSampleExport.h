#pragma once

#include <juce_core/juce_core.h>

namespace NordSampleExport
{
// Exports each decoded keyboard/velocity zone as a separate, uncompressed WAV.
// Existing files are never overwritten.
juce::Result exportZonesAsWav(const juce::File& source, const juce::File& destinationFolder,
                              int& exportedCount);
}
