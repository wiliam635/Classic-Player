#pragma once

#include "Session.h"
#include "TransportState.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace classicplayer
{
/**
    Interactive arrangement view for session audio clips.

    It intentionally edits only SessionClip placement metadata. The owning UI
    is notified after an edit so the audio engine can safely rebuild its
    immutable playback snapshot outside mouse-drag callbacks.
*/
class TimelineView final : public juce::Component
{
public:
    TimelineView(Session&, TransportState&);

    void refresh() noexcept { repaint(); }
    void setSelectedTrack(int trackIndex) noexcept;
    int selectedTrack() const noexcept { return selectedTrackIndex; }
    bool hasSelectedClip() const noexcept;
    juce::String selectedClipName() const;
    bool deleteSelectedClip();
    void zoomIn();
    void zoomOut();
    void showAll();

    // Called on the message thread after the user completes an edit.
    std::function<void()> onSessionEdited;
    std::function<void(int)> onTrackSelected;
    std::function<void(const juce::String&)> onClipSelected;
    // Raised after the timeline's compact M/S controls alter a track.
    std::function<void(int)> onTrackStateChanged;

private:
    struct ClipLocation
    {
        int track { -1 };
        int clip { -1 };

        bool isValid() const noexcept { return track >= 0 && clip >= 0; }
        bool operator==(const ClipLocation& other) const noexcept
        {
            return track == other.track && clip == other.clip;
        }
    };

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    juce::Rectangle<float> getTimelineArea() const;
    double visibleDuration() const noexcept;
    double totalDuration() const noexcept;
    double timeAtX(float x) const noexcept;
    float xAtTime(double seconds) const noexcept;
    int trackAtY(float y) const noexcept;
    ClipLocation clipAt(juce::Point<float>) const;
    void selectTrack(int);
    void selectClip(ClipLocation);
    SessionClip* getClip(ClipLocation) noexcept;
    const SessionClip* getClip(ClipLocation) const noexcept;
    void notifySessionEdited();

    Session& sessionState;
    TransportState& transportState;
    int selectedTrackIndex { 0 };
    ClipLocation selectedClip;
    ClipLocation draggedClip;
    double dragClipStartSeconds { 0.0 };
    double dragMouseStartSeconds { 0.0 };
    double viewStartSeconds { 0.0 };
    double secondsPerScreen { 16.0 };
    bool dragMovedClip { false };
};
}
