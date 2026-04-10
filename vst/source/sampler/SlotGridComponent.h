#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../framesampler/FrameSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal 12-slot display for the FrameSampler sampler page.
 *
 * - 12 cells drawn via paint(), one per MIDI note C1..B1 (playback trigger).
 * - Click → selects slot for editing (fires onSlotSelected callback).
 * - White bar drawn below the cell whose bank is currently active in the
 *   sequencer or playing back (getActivePlaySlot()).
 * - Cell colour encodes slot state:
 *     IDLE(empty)=dark grey  IDLE(content)=dark green  ARMED=orange blink
 *     RECORDING=red blink    PLAYING=bright green       SELECTED=white border
 *
 * RT safety: reads only atomic state via FrameSampler public API.
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

    // juce::Component
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;

    /** Bounding rectangle for slot cell i (excludes bottom underline area). */
    juce::Rectangle<int> cellBounds(int i) const noexcept;

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot = 0;
    bool blinkOn      = false;

    // MIDI note names displayed inside each cell (C1..B1)
    static const char* const kNoteNames[FrameSamplerConstants::NUM_SLOTS];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotGridComponent)
};
