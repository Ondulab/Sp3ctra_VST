#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "SlotGridComponent.h"
#include "SlotEditorComponent.h"
#include "SequencerComponent.h"
#include "TransportBarComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Master container for the LuxSampler sampler page.
 *
 * Layout — full-width zones stacked vertically:
 *   1. SlotGridComponent     (h=66,  fixed)  — 12 slot cells (sample bank)
 *   2. SlotEditorComponent   (h=210, fixed)  — edit panel:
 *        left  (~63 %) : REC/PLAY/CLEAR buttons + large timeline
 *        right (~37 %) : Speed / Loop / Resume controls
 *   3. Step sequencer        (grid + transport bar) — INTERNAL to this engine:
 *        one sequencer per sampler, addressing only its own banks (the global
 *        SEQUENCER rack module was retired).
 *
 * (The former per-sampler ".sp3s session" toolbar was retired with the
 * project-session model: banks persist through the SessionManager — sidecar
 * files in the working session or embedded in the DAW blob.)
 *
 * Manages selectedSlot state shared between SlotGrid and SlotEditor.
 */
class SamplerPageComponent : public juce::Component
{
public:
    explicit SamplerPageComponent(Sp3ctraAudioProcessor& proc);
    ~SamplerPageComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ── Layout constants (single source of truth for resized() + kPreferredH) ──
    static constexpr int kGridH = SlotGridComponent::kPreferredH; // bank tiles + per-bank mixer
    static constexpr int kEditH = 500;  // slot editor: 2 param cols + image + EQ
    static constexpr int kSeqH  = 180;  // step-sequencer grid (2 rows + header)

    // Natural height — must match the layout in resized(): pad + grid + gap +
    // editor + gap + sequencer + gap + transport + pad.
    // The transport reserves its two-row height so the page never clips it.
    // PluginEditor uses this to size the zone-3 viewport content so the whole
    // page scrolls into view at min size.
    static constexpr int kPreferredH = Sp3ctraTheme::kPad + kGridH + Sp3ctraTheme::kGap
                                     + kEditH + Sp3ctraTheme::kGap + kSeqH
                                     + Sp3ctraTheme::kGap
                                     + TransportBarComponent::kTwoRowH
                                     + Sp3ctraTheme::kPad;

    /** Bind this page (grid + editor + sequencer) to engine i. */
    void setSamplerIndex(int i)
    {
        samplerIndex_ = i;
        slotGrid.setSamplerIndex(i);
        slotEditor.setSamplerIndex(i);
        sequencer.setSamplerIndex(i);
        seqTransport.setSamplerIndex(i);
    }

    /** Select bank @p idx in the grid + editor (same path as a tile click).
     *  Used by the editor to restore the persisted selection. */
    void selectSlot(int idx) { onSlotSelected(idx); }

private:
    void onSlotSelected(int idx);

    int  samplerIndex_ = 0;   // 0 = engine A, 1 = engine B

    SlotGridComponent     slotGrid;
    SlotEditorComponent   slotEditor;
    SequencerComponent    sequencer;      // this engine's step grid
    TransportBarComponent seqTransport;   // this engine's seq transport bar

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPageComponent)
};
