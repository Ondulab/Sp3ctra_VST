/**
 * @file SynthOutPageComponent.h
 * @brief OUT (send) page — synth-split P2.
 *
 * Shown in ZONE 3 when a rack OUT block (→ LuxStral / → LuxSynth / → LuxWave)
 * is selected. Purge 2026-08-05: the per-OUT conditioning knobs (Negative /
 * DC Blocking / Gamma / Intensity / Range dB) are gone — flux conditioning is
 * the chain's business (LEVELS + DC modules), the decode window (Range dB)
 * is a machine setting on the LUXSTRAL SETUP face, and the OUT stage applies
 * the canonical decode. Purge 2026-08-13: Contrast Min followed — the
 * variance-driven dimming is the LEVELS module's CONTRAST knob now (visual
 * domain, so the sound follows the picture); no per-OUT knob remains and the
 * page is empty (the explanatory footnote was removed per user request).
 *
 * One instance is hosted by the editor and rebound per selection via
 * setTarget(type, slot) — the same rebind pattern as the pooled inserts.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "ModuleCatalog.h"

class SynthOutPageComponent : public juce::Component
{
public:
    explicit SynthOutPageComponent(Sp3ctraAudioProcessor& p)
    {
        juce::ignoreUnused(p);   // no per-OUT parameter left to bind
        setTarget(ModuleType::LuxStral, 0);
    }

    /** Rebind the page to the OUT bank of (type, slot).
     *  slot: LuxStral engine index (0 = A, 1 = B); 0 for the others. */
    void setTarget(ModuleType t, int slot)
    {
        type_ = t;
        slot_ = juce::jlimit(0, 7, slot);
        repaint();
    }

    ModuleType targetType() const noexcept { return type_; }
    int        targetSlot() const noexcept { return slot_; }

    // The page is deliberately EMPTY (2026-08-13, per user): no knob left,
    // no footnote — selecting an OUT block just shows the zone-3 header.
    static constexpr int kPreferredH = 8;

private:
    ModuleType type_ { ModuleType::LuxStral };
    int        slot_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthOutPageComponent)
};
