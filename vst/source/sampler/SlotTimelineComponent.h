#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesampler/FrameSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal spectral-timeline for a FrameSampler slot.
 *
 * Visualization (symmetric around the horizontal centre line):
 *   Upper half  — treble energy (right-pixel half, high freq) rising upward.
 *   Lower half  — bass energy  (left-pixel half, low  freq) falling downward.
 *   Gamma γ=0.4 compresses the scale so fine patterns are as visible as masses.
 *
 * Draggable handles (all kept at the top or bottom edge to avoid overlap):
 *
 *   ── Time handles (horizontal drag) ──────────────────────────────────────
 *   Start bar  — green vertical bar, drag anywhere on the bar.
 *   End   bar  — orange vertical bar, same.
 *   Attack ▷  — white triangle at top (y ≤ 16), drags right from Start.
 *   Decay  ◁  — white triangle at top (y ≤ 16), drags left  from End.
 *
 *   ── Frequency-cut handles (vertical drag) ───────────────────────────────
 *   TrebleCut ▼ — small tab at the TOP edge (y ≤ kEdge) right side.
 *                  Drag downward → fade the treble (upper) bars toward white.
 *   BassCut   ▲ — small tab at the BOTTOM edge (y ≥ h-kEdge) right side.
 *                  Drag upward  → fade the bass  (lower) bars toward white.
 *
 * Cursor feedback on hover (LeftRight for time handles, UpDown for freq cuts).
 */
class SlotTimelineComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit SlotTimelineComponent(Sp3ctraAudioProcessor& proc);
    ~SlotTimelineComponent() override;

    void setSelectedSlot(int idx);
    void markDirty() noexcept { thumbnailDirty = true; repaint(); }

    std::function<void(float)> onStartChanged;
    std::function<void(float)> onEndChanged;

    void paint    (juce::Graphics& g)         override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp  (const juce::MouseEvent& e) override;

private:
    Sp3ctraAudioProcessor& processor;
    int selectedSlot = 0;

    // Spectral thumbnail — bass [0..1] (lower half) and treble [0..1] (upper half).
    static constexpr int kMaxSamples = 512;
    float bass[kMaxSamples]   {};
    float treble[kMaxSamples] {};
    int   numSamples     = 0;
    bool  thumbnailDirty = true;
    void  rebuildThumbnail();

    enum class DragTarget { None, Start, End, Attack, Decay, TrebleCut, BassCut };
    DragTarget dragging = DragTarget::None;

    // Hit-test zones
    static constexpr int kEdge   = 8;   // px from top/bottom for freq-cut tabs
    static constexpr int kSnap   = 12;  // px tolerance for time handles
    static constexpr int kAtkH   = 16;  // y zone height for attack/decay handles

    void updateCursor(const juce::MouseEvent& e);

    float xToFrac(int x)   const noexcept;
    int   fracToX(float f) const noexcept;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTimelineComponent)
};
