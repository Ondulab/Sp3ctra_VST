#pragma once

#include <juce_graphics/juce_graphics.h>
#include <cstdint>

/**
 * @brief What the VIDEO SCROLL page's VIEWPORT pad needs from the video
 *        mixer — the latest image of ONE output rendered alone (the live
 *        thumbnail the vignette is drawn on: the output being edited, not
 *        the VIDEO MIX composite), a counter to know when it changed, and
 *        the size of the view being rendered (the pad reproduces the output
 *        window's aspect).
 *
 * Implemented by VideoMixerComponent (zone 4); the pages only see this
 * interface, so video/ never depends on the mixer. Message thread only.
 * The pad is polled (previewTick) by the editor's timer — no listener
 * registration, so the source may die before or after the pages.
 *
 * Solo frames are produced on demand: a pad calls requestOutputPreview(slot)
 * on every tick while it is showing, and the mixer keeps rendering that
 * output alone as long as the requests keep coming (they expire a few
 * hundred ms after the last one). Nothing to unregister when a page hides
 * or dies.
 */
struct VideoScrollPreviewSource
{
    virtual ~VideoScrollPreviewSource() = default;

    /** Keep the solo render of `slot` alive (call every tick while showing). */
    virtual void requestOutputPreview(int slot) = 0;

    /** Latest published image of that output ALONE — full level, no blend,
     *  its own paper / frame colour (ref-copy). Invalid until the first solo
     *  frame after a request — the pad then paints the frame colour. */
    virtual juce::Image outputFrame(int slot) const = 0;

    /** Bumped on every published frame — the pad repaints only on change. */
    virtual uint32_t frameCounter() const = 0;

    /** Size (logical px) of the view the renderer is targeting: the detached
     *  window content when open, else the column preview. {0, 0} = unknown
     *  (the pad falls back to a square window). */
    virtual juce::Point<int> viewSize() const = 0;
};
