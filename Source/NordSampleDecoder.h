#pragma once

#include "NordPianoDecoder.h"
#include <juce_core/juce_core.h>

namespace NordSampleDecoder
{
// Reads Nord Sample Library instruments generated as NSMP2 (.nsmp) or NSMP3
// (.nsmp3). NSMP4 is deliberately rejected until its different stereo codec is
// implemented and verified against a real instrument.
juce::Result decodeFile(const juce::File& file, NordPianoDecoder::Instrument& destination);
}
