#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../luxsampler/LuxSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal 12-slot display for the LuxSampler sampler page.
 *
 * - 12 cells drawn via paint(), one per MIDI note C1..B1 (playback trigger).
 * - Click → selects slot for editing (fires onSlotSelected callback).
 * - White bar drawn below the cell whose bank is currently active in the
 *   sequencer or playing back (getActivePlaySlot()).
 * - Cell colour encodes slot state:
 *     IDLE(empty)=dark grey  IDLE(content)=dark green
 *     RECORDING=red blink    PLAYING=bright green       SELECTED=white border
 *
 * RT safety: reads only atomic state via LuxSampler public API.
 * Internal 10 Hz Timer drives blink animation and repaints.
 */
class SlotGridComponent : public juce::Component,
                          private juce::Timer
{
public:
    explicit SlotGridComponent(Sp3ctraAudioProcessor& proc);
    ~SlotGridComponent() override;

    /** Fired when the user clicks a slot cell. */
    std::function<void(int slotIndex)> onSlotSelected;

    /** Push the selected slot from the parent (does NOT fire onSlotSelected). */
    void setSelectedSlot(int idx) noexcept;
    int  getSelectedSlot() const noexcept { return selectedSlot; }

    /** Bind this grid to sampler engine 0 (A) or 1 (B). */
    void setSamplerIndex(int i) noexcept { samplerIndex_ = i; repaint(); }

    // juce::Component
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;

    /** Bounding rectangle for slot cell i (excludes bottom underline area). */
    juce::Rectangle<int> cellBounds(int i) const noexcept;

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot  = 0;
    int  samplerIndex_ = 0;   // 0 = engine A, 1 = engine B
    bool blinkOn       = false;
    int  clipboardSlot = -1; // index of the slot last copied, -1 = empty

    // MIDI note names displayed inside each cell (C1..B1)
    static const char* const kNoteNames[LuxSamplerConstants::NUM_SLOTS];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotGridComponent)
};
