#pragma once

#include <juce_core/juce_core.h>

class LicenseVerifier
{
public:
    static bool verify(const juce::String& activationToken);
    static bool isActivated();
    static juce::String storedToken();
    static bool activateAndStore(const juce::String& activationToken);
    static bool loginOnline(const juce::String& email, const juce::String& password,
                            juce::String& errorMessage);
    static bool hasOnlineSession();
    static bool validateOnlineSession(juce::String& errorMessage);
    static void clearOnlineSession();
    static juce::String storedUserName();
    static juce::String storedUserEmail();
    static juce::File licenseFile();
};
