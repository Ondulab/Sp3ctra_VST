/**
 * @file SplitterBar.h
 * @brief Minimal draggable vertical divider used between the M4 zones.
 *
 * Chosen over juce::StretchableLayoutManager: with only two persisted
 * widths (zone 2 / zone 4) and a collapsible right column, a direct drag
 * callback into the editor's layout math is simpler and more robust.
 *
 * The owner wires:
 *   onDragStart  — snapshot the width being edited
 *   onDragged    — delta-x from drag start (owner clamps + relayouts)
 *   onDragEnd    — persist the final value
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class SplitterBar : public juce::Component
{
public:
    SplitterBar()
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity(true);
    }

    std::function<void()>    onDragStart;
    std::function<void(int)> onDragged;   ///< delta-x from drag start (px)
    std::function<void()>    onDragEnd;

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff151518));

        const bool hot = isMouseOverOrDragging();
        g.setColour(hot ? juce::Colour(0xff6a7a96) : juce::Colour(0xff34343f));

        // Grip dots (centred)
        const float cx = getWidth() * 0.5f;
        const float cy = getHeight() * 0.5f;
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse(cx - 1.5f, cy + (float)i * 9.f - 1.5f, 3.f, 3.f);

        // Edge lines
        g.setColour(juce::Colour(0xff101014));
        g.fillRect(0, 0, 1, getHeight());
        g.fillRect(getWidth() - 1, 0, 1, getHeight());
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onDragStart) onDragStart();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (onDragged) onDragged(e.getDistanceFromDragStartX());
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (onDragEnd) onDragEnd();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitterBar)
};
