#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "../luxsampler/LuxSampler.h"
#include "../midi/MidiLearnAttachment.h"
#include "../ui/Sp3ctraBarSlider.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Horizontal bank display for the LuxSampler sampler page.
 *
 * - N cells (SETUP "Banks" 1..8) drawn via paint(), labelled "Bank 1..N"
 *   (the former C1..B1 note addressing was removed — several banks can play
 *   simultaneously).
 * - Click → selects bank for editing (fires onSlotSelected callback).
 * - Under each tile: the bank MIXER — a level fader (fade to white, mirrors
 *   the VIDEO MIX faders; engine param = 1 − brightnessLift) and a mix-mode
 *   box (MIX / ADD / DARK) choosing how this bank composites into the master
 *   frame when several banks play at once. Both are right-click MIDI-Learn
 *   targets (fixed bank per control).
 * - White bar drawn below the cell whose bank is currently active in the
 *   sequencer or the primary playing voice (getActivePlaySlot()).
 * - Cell colour encodes bank state:
 *     IDLE(empty)=dark grey  IDLE(content)=dark green
 *     RECORDING=red blink    PLAYING=bright green       SELECTED=brightened
 *
 * RT safety: reads only atomic state via LuxSampler public API.
 * Internal 10 Hz Timer drives blink animation, mixer resync and repaints.
 */
class SlotGridComponent : public juce::Component,
                          private juce::Timer
{
public:
    explicit SlotGridComponent(Sp3ctraAudioProcessor& proc);
    ~SlotGridComponent() override;

    /** Fired when the user clicks a bank cell. */
    std::function<void(int slotIndex)> onSlotSelected;

    /** Push the selected bank from the parent (does NOT fire onSlotSelected). */
    void setSelectedSlot(int idx) noexcept;
    int  getSelectedSlot() const noexcept { return selectedSlot; }

    /** Bind this grid to sampler engine 0 (A) or 1 (B) — rebinds the mixer
     *  rows' values and MIDI-Learn targets to that engine. */
    void setSamplerIndex(int i);

    // Layout metrics — shared with SamplerPageComponent::kGridH.
    static constexpr int kMixRowH   = 16;  // one mixer row (fader / mode box)
    static constexpr int kMixRowGap = 2;
    static constexpr int kUnderH    = 5;   // sequencer underline strip
    static constexpr int kTileH     = 58;  // bank tile
    static constexpr int kPreferredH =
        kTileH + kMixRowGap + kMixRowH + kMixRowGap + kMixRowH + kUnderH;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;

    /** Number of banks currently exposed (SETUP "Banks", 1..MAX_UI_BANKS). */
    int numBanks() const;

    /** Bounding rectangle for bank tile i (above the mixer rows). */
    juce::Rectangle<int> cellBounds(int i) const noexcept;

    /** Reposition the mixer widgets + clamp the selection to numBanks(). */
    void layoutMixerRow();

    /** Pull level/mode values from the engine into the widgets (silent). */
    void refreshMixerValues();

    /** Recreate the mixer rows' MIDI-Learn attachments for the bound engine. */
    void rebindMidiLearn();

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot  = 0;
    int  samplerIndex_ = 0;   // 0 = engine A, 1 = engine B
    bool blinkOn       = false;
    int  clipboardSlot = -1;  // index of the bank last copied, -1 = empty
    int  lastNumBanks_ = -1;  // relayout trigger when the SETUP count changes

    // ── Per-bank mixer rows (level fader + mix-mode box) ─────────────────────
    Sp3ctraBarSlider levelSlider[LuxSamplerConstants::MAX_UI_BANKS];
    juce::ComboBox modeBox    [LuxSamplerConstants::MAX_UI_BANKS];
    std::unique_ptr<MidiLearnAttachment> levelLearn[LuxSamplerConstants::MAX_UI_BANKS];
    std::unique_ptr<MidiLearnAttachment> modeLearn [LuxSamplerConstants::MAX_UI_BANKS];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotGridComponent)
};
