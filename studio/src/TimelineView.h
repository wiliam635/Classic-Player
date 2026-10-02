#pragma once

#include "Session.h"
#include "TransportState.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace classicplayer
{
/** A small, paint-only timeline preview for the session's recorded clips. */
class TimelineView final : public juce::Component
{
public:
    TimelineView(Session&, TransportState&);

    void refresh() noexcept { repaint(); }

private:
    void paint(juce::Graphics&) override;

    Session& sessionState;
    TransportState& transportState;
};
}
