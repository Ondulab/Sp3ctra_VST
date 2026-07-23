#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../luxsampler/LuxSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal spectral-timeline for a LuxSampler slot.
 *
 * Visualization (symmetric around the horizontal centre line):
 *   Upper half  — treble energy (right-pixel half, high freq) rising upward.
 *   Lower half  — bass energy  (left-pixel half, low  freq) falling downward.
 *   Gamma γ=0.4 compresses the scale so fine patterns are as visible as masses.
 *
 * Draggable handles — TIME domain only (frequency shaping lives in the dedicated
 * SpectralCurveComponent):
 *   Start bar  — green vertical bar, drag anywhere on the bar.
 *   End   bar  — orange vertical bar, same.
 *   Attack ▷  — white triangle centred at h/2, drags right from Start.
 *   Decay  ◁  — white triangle centred at h/2, drags left  from End.
 *
 * While the slot is PLAYING, a click/drag anywhere OUTSIDE those handles
 * scrubs the play head (requestSlotSeek) — the primary gesture with speed 0.
 * Bounds/fades keep priority near their own handles; the loose-click
 * fallback that moved the nearest bound only applies when NOT playing.
 *
 * Cursor feedback on hover (LeftRight for the bars, PointingHand for the fades).
 */
class SlotTimelineComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit SlotTimelineComponent(Sp3ctraAudioProcessor& proc);
    ~SlotTimelineComponent() override;

    void setSelectedSlot(int idx);
    void markDirty() noexcept { thumbnailDirty = true; repaint(); }

    /** Bind this timeline to sampler engine 0 (A) or 1 (B). */
    void setSamplerIndex(int i) noexcept { samplerIndex_ = i; thumbnailDirty = true; repaint(); }

    std::function<void(float)> onStartChanged;
    std::function<void(float)> onEndChanged;

    void paint    (juce::Graphics& g)         override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp  (const juce::MouseEvent& e) override;

private:
    Sp3ctraAudioProcessor& processor;
    int selectedSlot  = 0;
    int samplerIndex_ = 0;   // 0 = engine A, 1 = engine B

    // Spectral thumbnail — bass [0..1] (lower half) and treble [0..1] (upper half).
    static constexpr int kMaxSamples = 512;
    float bass[kMaxSamples]   {};
    float treble[kMaxSamples] {};
    int   numSamples     = 0;
    bool  thumbnailDirty = true;
    void  rebuildThumbnail();

    enum class DragTarget { None, Start, End, Attack, Decay, Playhead };
    DragTarget dragging = DragTarget::None;

    // Hit-test zones
    static constexpr int kSnap   = 12;  // px tolerance for time handles
    static constexpr int kAtkH   = 16;  // half-height of the central fade band

    void updateCursor(const juce::MouseEvent& e);

    float xToFrac(int x)   const noexcept;
    int   fracToX(float f) const noexcept;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTimelineComponent)
};
