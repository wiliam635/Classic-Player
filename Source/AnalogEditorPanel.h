#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
class ClassicPlayerAudioProcessor;

// Shared by the actual Analog dialog and its lifecycle regression test.
std::unique_ptr<juce::Component> createAnalogCommonControls(ClassicPlayerAudioProcessor&, int layer);
