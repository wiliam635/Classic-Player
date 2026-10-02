#pragma once

#include "Session.h"
#include "MixerState.h"
#include "TransportState.h"
#include "InstrumentHost.h"
#include "AudioEngine.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace classicplayer
{
class StudioApplication final : public juce::JUCEApplication
{
public:
    ~StudioApplication() override;
    const juce::String getApplicationName() override { return "Classic Player Studio"; }
    const juce::String getApplicationVersion() override { return CLASSIC_PLAYER_STUDIO_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String&) override;
    void shutdown() override;
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}
private:
    class MainWindow;
    // MainWindow is implemented privately in the .cpp file. Store it through
    // JUCE's complete base type so Clang does not instantiate the unique_ptr
    // deleter where the nested type is still incomplete.
    std::unique_ptr<juce::DocumentWindow> window;
    Session session;
    TransportState transport;
    MixerState mixer;
    InstrumentHost instrumentHost;
    AudioEngine audioEngine { transport, mixer, instrumentHost };
};
}
