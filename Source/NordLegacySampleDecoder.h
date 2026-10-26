#pragma once

#include "NordSampleDecoder.h"

namespace NordLegacySampleDecoder
{
// Decodes Nord Sample Library 2 (.nsmp) and 3 (.nsmp3) into the same
// instrument representation used by the NSMP4 playback engine.
juce::Result decodeFile(const juce::File& file, NordSampleDecoder::Instrument& destination);
}
