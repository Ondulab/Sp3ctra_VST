/**
 * @file LuxEchoTabComponent.h
 * @brief Tab — ECHO: echo on the image-line stream (regenerated repeats).
 *
 * The interactive graphic editor fills the page (EchoEditorComponent — the
 * repeat train over its whole time window, with a ruler in seconds, Delay /
 * Mix / Feedback handles + numeric boxes below the frame). The background pole is chain-owned (rack header selector), not a
 * module setting. Page skeleton (padding, "--- ECHO ---" caption, height) =
 * ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxecho{slot}_*
 * bank of the selected instance (same pattern as VideoScrollPage).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/EchoEditorComponent.h"

class LuxEchoTabComponent : public juce::Component
{
public:
    /** Accent colour for the ECHO page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Echo).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = EchoEditorComponent::kPreferredH;

    // top pad + editor + "--- ECHO ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxEchoTabComponent(Sp3ctraAudioProcessor& p)
        : editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive repeat-train editor (Delay / Feedback / Mix) ───
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        // The time ruler reads the measured line rate (the LFO bank's SCAN
        // clock — 0 while no stream runs, the ruler then counts lines).
        editor.setLineRateProvider([&p] { return p.getLfoBank().lineRate(); });
        addAndMakeVisible(editor);

        // ── Enable toggle ── rack LED + zone-3 header power switch

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the ECHO bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_, ecParam(slot_, "Delay"),
                           ecParam(slot_, "Feedback"), ecParam(slot_, "Mix"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "ECHO");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    int slot_ { 0 };   // pool slot of the bound instance

    EchoEditorComponent editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxEchoTabComponent)
};
