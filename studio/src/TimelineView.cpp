#include "TimelineView.h"

#include <cmath>

namespace classicplayer
{
TimelineView::TimelineView(Session& sessionToUse, TransportState& transportToUse)
    : sessionState(sessionToUse), transportState(transportToUse)
{
    setOpaque(true);
}

void TimelineView::paint(juce::Graphics& graphics)
{
    const auto area = getLocalBounds().toFloat();
    graphics.fillAll(juce::Colour(0xff101820));
    graphics.setColour(juce::Colour(0xff304556));
    graphics.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);

    const auto headerHeight = 18.0f;
    const auto nameWidth = 112.0f;
    const auto timeline = area.withTrimmedTop(headerHeight).withTrimmedLeft(nameWidth).reduced(2.0f);
    const auto trackCount = juce::jmax(1, sessionState.tracks.size());
    const auto rowHeight = timeline.getHeight() / static_cast<float>(trackCount);

    double duration = 8.0;
    for (const auto& track : sessionState.tracks)
        for (const auto& clip : track.clips)
            duration = juce::jmax(duration, clip.startSeconds + clip.lengthSeconds);
    duration = juce::jmax(duration, transportState.position() + 2.0);

    graphics.setColour(juce::Colours::lightgrey);
    graphics.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    graphics.drawText("TIMELINE", 8, 2, static_cast<int>(nameWidth - 12.0f),
                      static_cast<int>(headerHeight - 2.0f), juce::Justification::centredLeft);

    for (int second = 0; second <= static_cast<int>(std::ceil(duration)); ++second)
    {
        const auto x = timeline.getX() + timeline.getWidth()
                                      * static_cast<float>(static_cast<double>(second) / duration);
        graphics.setColour(second % 5 == 0 ? juce::Colour(0xff506577)
                                           : juce::Colour(0xff293d4d));
        graphics.drawVerticalLine(static_cast<int>(x), timeline.getY(), timeline.getBottom());
        if (second % 5 == 0)
        {
            graphics.setColour(juce::Colours::lightgrey);
            graphics.setFont(juce::FontOptions(9.0f));
            graphics.drawText(juce::String(second) + " s", static_cast<int>(x) + 2,
                              2, 42, static_cast<int>(headerHeight - 2.0f),
                              juce::Justification::centredLeft);
        }
    }

    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto row = timeline.withY(timeline.getY() + rowHeight * static_cast<float>(trackIndex))
                                  .withHeight(rowHeight);
        graphics.setColour(juce::Colour(0xff20313e));
        graphics.drawHorizontalLine(static_cast<int>(row.getY()), row.getX(), row.getRight());

        const auto name = trackIndex < sessionState.tracks.size()
                              ? sessionState.tracks[trackIndex].name
                              : juce::String("Track");
        graphics.setColour(juce::Colours::lightgrey);
        graphics.setFont(juce::FontOptions(10.0f));
        graphics.drawText(name, 8, static_cast<int>(row.getY()),
                          static_cast<int>(nameWidth - 14.0f), static_cast<int>(row.getHeight()),
                          juce::Justification::centredLeft, true);

        if (trackIndex >= sessionState.tracks.size())
            continue;

        for (const auto& clip : sessionState.tracks[trackIndex].clips)
        {
            const auto start = static_cast<float>(clip.startSeconds / duration);
            const auto width = static_cast<float>(clip.lengthSeconds / duration);
            auto clipArea = row.reduced(2.0f);
            clipArea.setX(timeline.getX() + timeline.getWidth() * start);
            clipArea.setWidth(juce::jmax(4.0f, timeline.getWidth() * width));
            graphics.setColour(juce::Colour(0xff087f8c));
            graphics.fillRoundedRectangle(clipArea, 3.0f);
            graphics.setColour(juce::Colours::white);
            graphics.setFont(juce::FontOptions(10.0f));
            graphics.drawText(clip.name, clipArea.toNearestInt().reduced(5, 0),
                              juce::Justification::centredLeft, true);
        }
    }

    const auto playheadX = timeline.getX() + timeline.getWidth()
                                             * static_cast<float>(transportState.position() / duration);
    graphics.setColour(juce::Colour(0xffffd166));
    graphics.drawVerticalLine(static_cast<int>(playheadX), timeline.getY(), timeline.getBottom(), 2.0f);
}
}
