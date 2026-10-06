#include "TimelineView.h"

#include <cmath>

namespace classicplayer
{
namespace
{
constexpr auto headerHeight = 24.0f;
constexpr auto nameWidth = 150.0f;
constexpr auto minimumSecondsPerScreen = 2.0;
constexpr auto maximumSecondsPerScreen = 600.0;
}

TimelineView::TimelineView(Session& sessionToUse, TransportState& transportToUse)
    : sessionState(sessionToUse), transportState(transportToUse)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
}

void TimelineView::setSelectedTrack(int trackIndex) noexcept
{
    const auto maximum = juce::jmax(0, sessionState.tracks.size() - 1);
    selectedTrackIndex = juce::jlimit(0, maximum, trackIndex);
    repaint();
}

bool TimelineView::hasSelectedClip() const noexcept
{
    return getClip(selectedClip) != nullptr;
}

juce::String TimelineView::selectedClipName() const
{
    if (const auto* clip = getClip(selectedClip); clip != nullptr)
        return clip->name;
    return {};
}

bool TimelineView::deleteSelectedClip()
{
    if (! selectedClip.isValid() || selectedClip.track >= sessionState.tracks.size())
        return false;

    auto& clips = sessionState.tracks.getReference(selectedClip.track).clips;
    if (selectedClip.clip >= clips.size())
        return false;

    clips.remove(selectedClip.clip);
    selectedClip = {};
    draggedClip = {};
    notifySessionEdited();
    return true;
}

void TimelineView::zoomIn()
{
    secondsPerScreen = juce::jmax(minimumSecondsPerScreen, secondsPerScreen / 1.6);
    repaint();
}

void TimelineView::zoomOut()
{
    secondsPerScreen = juce::jmin(maximumSecondsPerScreen, secondsPerScreen * 1.6);
    repaint();
}

void TimelineView::showAll()
{
    secondsPerScreen = juce::jlimit(minimumSecondsPerScreen, maximumSecondsPerScreen,
                                    totalDuration() * 1.10);
    viewStartSeconds = 0.0;
    repaint();
}

juce::Rectangle<float> TimelineView::getTimelineArea() const
{
    return getLocalBounds().toFloat()
        .withTrimmedTop(headerHeight)
        .withTrimmedLeft(nameWidth)
        .reduced(2.0f);
}

double TimelineView::visibleDuration() const noexcept
{
    return juce::jmax(minimumSecondsPerScreen, secondsPerScreen);
}

double TimelineView::totalDuration() const noexcept
{
    double duration = 8.0;
    for (const auto& track : sessionState.tracks)
        for (const auto& clip : track.clips)
            duration = juce::jmax(duration, clip.startSeconds + clip.lengthSeconds);
    return juce::jmax(duration, transportState.position() + 2.0);
}

double TimelineView::timeAtX(float x) const noexcept
{
    const auto timeline = getTimelineArea();
    if (timeline.getWidth() <= 0.0f)
        return viewStartSeconds;

    const auto normalised = (x - timeline.getX()) / timeline.getWidth();
    return juce::jmax(0.0, viewStartSeconds + static_cast<double>(normalised) * visibleDuration());
}

float TimelineView::xAtTime(double seconds) const noexcept
{
    const auto timeline = getTimelineArea();
    return timeline.getX() + timeline.getWidth() * static_cast<float>(
        (seconds - viewStartSeconds) / visibleDuration());
}

int TimelineView::trackAtY(float y) const noexcept
{
    const auto timeline = getTimelineArea();
    if (y < timeline.getY() || y >= timeline.getBottom() || sessionState.tracks.isEmpty())
        return -1;

    const auto rowHeight = timeline.getHeight() / static_cast<float>(sessionState.tracks.size());
    if (rowHeight <= 0.0f)
        return -1;

    return juce::jlimit(0, sessionState.tracks.size() - 1,
                         static_cast<int>((y - timeline.getY()) / rowHeight));
}

TimelineView::ClipLocation TimelineView::clipAt(juce::Point<float> point) const
{
    const auto timeline = getTimelineArea();
    const auto track = trackAtY(point.y);
    if (track < 0 || track >= sessionState.tracks.size())
        return {};

    const auto rowHeight = timeline.getHeight() / static_cast<float>(sessionState.tracks.size());
    const auto row = timeline.withY(timeline.getY() + rowHeight * static_cast<float>(track))
                             .withHeight(rowHeight);
    const auto& clips = sessionState.tracks[track].clips;
    for (int clipIndex = clips.size(); --clipIndex >= 0;)
    {
        const auto& clip = clips[clipIndex];
        auto clipArea = row.reduced(4.0f, 5.0f);
        clipArea.setX(xAtTime(clip.startSeconds));
        clipArea.setWidth(juce::jmax(8.0f, xAtTime(clip.startSeconds + clip.lengthSeconds)
                                           - clipArea.getX()));
        if (clipArea.contains(point))
            return { track, clipIndex };
    }
    return {};
}

void TimelineView::selectTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= sessionState.tracks.size())
        return;

    if (selectedTrackIndex != trackIndex)
    {
        selectedTrackIndex = trackIndex;
        if (onTrackSelected)
            onTrackSelected(trackIndex);
    }
}

void TimelineView::selectClip(ClipLocation location)
{
    selectedClip = location;
    if (location.isValid())
    {
        selectTrack(location.track);
        if (onClipSelected)
            onClipSelected(selectedClipName());
    }
    repaint();
}

SessionClip* TimelineView::getClip(ClipLocation location) noexcept
{
    if (! location.isValid() || location.track >= sessionState.tracks.size())
        return nullptr;

    auto& clips = sessionState.tracks.getReference(location.track).clips;
    return location.clip < clips.size() ? &clips.getReference(location.clip) : nullptr;
}

const SessionClip* TimelineView::getClip(ClipLocation location) const noexcept
{
    if (! location.isValid() || location.track >= sessionState.tracks.size())
        return nullptr;

    const auto& clips = sessionState.tracks.getReference(location.track).clips;
    return location.clip < clips.size() ? &clips.getReference(location.clip) : nullptr;
}

void TimelineView::notifySessionEdited()
{
    if (onSessionEdited)
        onSessionEdited();
    repaint();
}

void TimelineView::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    const auto point = event.position;
    const auto timeline = getTimelineArea();
    if (point.y < headerHeight)
        return;

    const auto track = trackAtY(point.y);
    if (track < 0)
        return;

    selectTrack(track);
    if (point.x < timeline.getX())
    {
        repaint();
        return;
    }

    const auto location = clipAt(point);
    if (location.isValid())
    {
        selectClip(location);
        draggedClip = location;
        dragMouseStartSeconds = timeAtX(point.x);
        if (const auto* clip = getClip(location); clip != nullptr)
            dragClipStartSeconds = clip->startSeconds;
        dragMovedClip = false;
        return;
    }

    selectedClip = {};
    draggedClip = {};
    transportState.setPositionSeconds(timeAtX(point.x));
    repaint();
}

void TimelineView::mouseDrag(const juce::MouseEvent& event)
{
    if (! draggedClip.isValid())
        return;

    auto* clip = getClip(draggedClip);
    if (clip == nullptr)
        return;

    const auto delta = timeAtX(event.position.x) - dragMouseStartSeconds;
    const auto newStart = juce::jmax(0.0, dragClipStartSeconds + delta);
    dragMovedClip = dragMovedClip || std::abs(newStart - clip->startSeconds) > 0.001;
    clip->startSeconds = newStart;
    repaint();
}

void TimelineView::mouseUp(const juce::MouseEvent&)
{
    if (dragMovedClip)
        notifySessionEdited();
    draggedClip = {};
    dragMovedClip = false;
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& event,
                                  const juce::MouseWheelDetails& wheel)
{
    if (std::abs(wheel.deltaY) < 0.001f)
        return;

    if (event.mods.isShiftDown())
    {
        viewStartSeconds = juce::jmax(0.0, viewStartSeconds
                                           - static_cast<double>(wheel.deltaY)
                                                 * visibleDuration() * 0.18);
        repaint();
        return;
    }

    const auto focusTime = timeAtX(event.position.x);
    const auto scale = wheel.deltaY > 0.0f ? 0.8 : 1.25;
    secondsPerScreen = juce::jlimit(minimumSecondsPerScreen, maximumSecondsPerScreen,
                                    secondsPerScreen * scale);
    const auto timeline = getTimelineArea();
    const auto pointerFraction = timeline.getWidth() > 0.0f
        ? (event.position.x - timeline.getX()) / timeline.getWidth() : 0.5f;
    viewStartSeconds = juce::jmax(0.0, focusTime - pointerFraction * visibleDuration());
    repaint();
}

bool TimelineView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return deleteSelectedClip();
    if (key == juce::KeyPress::homeKey)
    {
        viewStartSeconds = 0.0;
        repaint();
        return true;
    }
    if (key.getTextCharacter() == '+' || key.getTextCharacter() == '=')
    {
        zoomIn();
        return true;
    }
    if (key.getTextCharacter() == '-')
    {
        zoomOut();
        return true;
    }
    return false;
}

void TimelineView::paint(juce::Graphics& graphics)
{
    const auto area = getLocalBounds().toFloat();
    const auto timeline = getTimelineArea();
    graphics.fillAll(juce::Colour(0xff0a151d));
    graphics.setColour(juce::Colour(0xff31566a));
    graphics.drawRoundedRectangle(area.reduced(0.5f), 5.0f, 1.0f);

    const auto trackCount = juce::jmax(1, sessionState.tracks.size());
    const auto rowHeight = timeline.getHeight() / static_cast<float>(trackCount);
    const auto end = viewStartSeconds + visibleDuration();

    graphics.setColour(juce::Colour(0xff132833));
    graphics.fillRect(area.withHeight(headerHeight));
    graphics.setColour(juce::Colour(0xffe4f2f5));
    graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    graphics.drawText("TIMELINE", 10, 2, static_cast<int>(nameWidth - 20.0f),
                      static_cast<int>(headerHeight - 2.0f), juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(0xff79bac4));
    graphics.setFont(juce::FontOptions(10.0f));
    graphics.drawText("roda: zoom - Shift+roda: navegar - clique: cursor - arraste: mover - Del: apagar",
                      static_cast<int>(nameWidth), 2,
                      static_cast<int>(timeline.getWidth()), static_cast<int>(headerHeight - 2.0f),
                      juce::Justification::centredRight, true);

    const auto step = visibleDuration() <= 8.0 ? 1.0
                    : visibleDuration() <= 24.0 ? 2.0
                    : visibleDuration() <= 60.0 ? 5.0
                    : visibleDuration() <= 180.0 ? 15.0 : 30.0;
    const auto firstGrid = std::floor(viewStartSeconds / step) * step;
    for (auto second = firstGrid; second <= end + step; second += step)
    {
        const auto x = xAtTime(second);
        if (x < timeline.getX() - 1.0f || x > timeline.getRight() + 1.0f)
            continue;
        graphics.setColour(std::fmod(second, step * 5.0) < 0.001
                               ? juce::Colour(0xff33576a) : juce::Colour(0xff1d3645));
        graphics.drawVerticalLine(static_cast<int>(x), timeline.getY(), timeline.getBottom());
        graphics.setColour(juce::Colour(0xff9bbec7));
        graphics.setFont(juce::FontOptions(9.0f));
        graphics.drawText(juce::String(second, 0) + " s", static_cast<int>(x) + 3,
                          2, 48, static_cast<int>(headerHeight - 2.0f),
                          juce::Justification::centredLeft);
    }

    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto row = timeline.withY(timeline.getY() + rowHeight * static_cast<float>(trackIndex))
                                 .withHeight(rowHeight);
        const auto selected = trackIndex == selectedTrackIndex;
        graphics.setColour(selected ? juce::Colour(0xff173a49) : juce::Colour(0xff0f222d));
        graphics.fillRect(area.withY(row.getY()).withHeight(row.getHeight()).withWidth(nameWidth));
        graphics.setColour(juce::Colour(0xff203d4d));
        graphics.drawHorizontalLine(static_cast<int>(row.getY()), 0.0f, area.getRight());

        const auto name = trackIndex < sessionState.tracks.size()
                              ? sessionState.tracks[trackIndex].name : juce::String("Track");
        graphics.setColour(selected ? juce::Colour(0xffeaffff) : juce::Colour(0xffb2cbd1));
        graphics.setFont(juce::FontOptions(11.0f, selected ? juce::Font::bold : juce::Font::plain));
        graphics.drawText(name, 10, static_cast<int>(row.getY()),
                          static_cast<int>(nameWidth - 14.0f), static_cast<int>(row.getHeight()),
                          juce::Justification::centredLeft, true);

        if (trackIndex >= sessionState.tracks.size())
            continue;

        const auto& clips = sessionState.tracks[trackIndex].clips;
        for (int clipIndex = 0; clipIndex < clips.size(); ++clipIndex)
        {
            const auto& clip = clips[clipIndex];
            if (clip.startSeconds + clip.lengthSeconds < viewStartSeconds || clip.startSeconds > end)
                continue;

            auto clipArea = row.reduced(5.0f, 6.0f);
            clipArea.setX(xAtTime(clip.startSeconds));
            clipArea.setWidth(juce::jmax(8.0f, xAtTime(clip.startSeconds + clip.lengthSeconds)
                                               - clipArea.getX()));
            const auto isSelected = selectedClip == ClipLocation { trackIndex, clipIndex };
            graphics.setColour(isSelected ? juce::Colour(0xff16cbd2) : juce::Colour(0xff087f8c));
            graphics.fillRoundedRectangle(clipArea, 4.0f);
            graphics.setColour(isSelected ? juce::Colours::white : juce::Colour(0xffd8f7f8));
            graphics.drawRoundedRectangle(clipArea, 4.0f, isSelected ? 2.0f : 1.0f);
            graphics.setColour(juce::Colours::white);
            graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            graphics.drawText(clip.name, clipArea.toNearestInt().reduced(7, 0),
                              juce::Justification::centredLeft, true);
        }
    }

    if (const auto playheadX = xAtTime(transportState.position());
        playheadX >= timeline.getX() && playheadX <= timeline.getRight())
    {
        graphics.setColour(juce::Colour(0xffffd166));
        graphics.drawVerticalLine(static_cast<int>(playheadX), timeline.getY(), timeline.getBottom());
        juce::Path marker;
        marker.startNewSubPath(playheadX - 4.0f, timeline.getY());
        marker.lineTo(playheadX + 4.0f, timeline.getY());
        marker.lineTo(playheadX, timeline.getY() + 7.0f);
        marker.closeSubPath();
        graphics.fillPath(marker);
    }
}
}
