#pragma once

#include <juce_core/juce_core.h>

class LicenseVerifier
{
public:
    static bool verify(const juce::String& activationToken);
    static bool isActivated();
    static juce::String storedToken();
    static bool activateAndStore(const juce::String& activationToken);
    static juce::File licenseFile();
};
