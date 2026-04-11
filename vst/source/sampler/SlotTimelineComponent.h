#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesampler/FrameSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal timeline / waveform visualizer for a FrameSampler slot.
 *
 * Displays a brightness thumbnail computed from the recorded frames.
 * Two draggable handles let the user set the playback Start and End points
 * directly on the timeline, updating FrameSampler slotParams atomics on
 * the fly (reflected immediately in the SlotEditorComponent sliders via
 * the onStartChanged / onEndChanged callbacks).
 *
 * A white playhead cursor tracks the current read position while PLAYING.
 *
 * Non-RT safety:
 *   - rebuildThumbnail() reads slot frames from the message thread while
 *     the slot is not recording (safe — only FramePlayerThread reads them).
 *   - getSlotPlayHead() reads a per-slot atomic updated by FramePlayerThread.
 */
class SlotTimelineComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit SlotTimelineComponent(Sp3ctraAudioProcessor& proc);
    ~SlotTimelineComponent() override;

    /** Switch to displaying a different slot (0–11). Invalidates thumbnail. */
    void setSelectedSlot(int idx);

    /** Invalidate the cached thumbnail (call after a new recording). */
    void markDirty() noexcept { thumbnailDirty = true; repaint(); }

    // Fired when the user drags Start / End handles on the timeline.
    // Caller (SlotEditorComponent) uses these to sync the sliders.
    std::function<void(float)> onStartChanged;
    std::function<void(float)> onEndChanged;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    Sp3ctraAudioProcessor& processor;
    int selectedSlot  = 0;

    // Cached brightness waveform (computed lazily in paint())
    static constexpr int kMaxSamples = 512;
    float brightness[kMaxSamples] {};
    int   numSamples    = 0;
    bool  thumbnailDirty = true;

    void rebuildThumbnail();

    // Drag state
    enum class DragTarget { None, Start, End };
    DragTarget dragging = DragTarget::None;

    // Coordinate helpers
    float xToFrac(int x)    const noexcept;
    int   fracToX(float f)  const noexcept;

    void timerCallback() override; // repaint when playing (playhead moves)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTimelineComponent)
};
