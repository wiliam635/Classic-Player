#pragma once

#include <juce_core/juce_core.h>

// Imports the portable resources from a Nord Sound Manager program bundle.
// The original bundle is never modified. Imported files live in the user's
// Classic Player data directory and can be catalogued again on next launch.
class NordBundleImporter
{
public:
    struct ImportSummary
    {
        juce::File bankDirectory;
        int pianoCount = 0;
        int programCount = 0;
        int sampleLibraryCount = 0;
    };

    static juce::Result importBundle(const juce::File& source,
                                     const juce::File& libraryRoot,
                                     ImportSummary& summary);

    static juce::Array<juce::File> findNordPianos(const juce::File& libraryRoot);
};
