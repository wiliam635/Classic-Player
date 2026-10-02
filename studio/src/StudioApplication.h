#pragma once

#include "Session.h"
#include "MixerState.h"
#include "TransportState.h"
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
    std::unique_ptr<MainWindow> window;
    Session session;
    TransportState transport;
    MixerState mixer;
};
}
