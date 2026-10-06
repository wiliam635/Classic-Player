#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <mutex>

class ClassicPlayerUpdateChecker
{
public:
    struct Result
    {
        bool succeeded = false, available = false;
        juce::String version, downloadUrl, notesUrl, notes;
    };
    bool start();
    bool isChecking() const { return state->checking.load(); }
    bool takeResult(Result&);
    static Result parseManifest(const juce::var&, const juce::String& installedVersion,
                                const juce::String& platform);
private:
    // Workers retain only this state. Closing the editor never waits for
    // networking and no callback can point to a destroyed UI component.
    struct State
    {
        std::atomic<bool> checking { false }, completed { false };
        std::mutex mutex;
        Result result;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
};
