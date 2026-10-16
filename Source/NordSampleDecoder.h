#pragma once

#include "NordPianoDecoder.h"

namespace NordSampleDecoder
{
juce::Result decodeFile(const juce::File& file, NordPianoDecoder::Instrument& destination);
}
