/**
 * @file LuxGainTabComponent.h
 * @brief Tab — GAIN: per-line energy gain on the image-line stream.
 *
 * The graphic view fills the page (GainEditorComponent — the live stream
 * profile with the gained output preview; Gain is set from the box below).
 * The background pole is chain-owned (rack header selector), not a module
 * setting. Page skeleton (padding, "--- GAIN ---" caption, height) =
 * ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxgain{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxDcBlock).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/GainEditorComponent.h"

class LuxGainTabComponent : public juce::Component
{
public:
    /** Accent colour for the GAIN page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Gain).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = GainEditorComponent::kPreferredH;

    // top pad + editor + "--- GAIN ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxGainTabComponent(Sp3ctraAudioProcessor& p)
        : editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive gain editor (Gain dB) ──────────────────────────────
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the GAIN bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_, gnParam(slot_, "Gain"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "GAIN");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    int slot_ { 0 };   // pool slot of the bound instance

    GainEditorComponent editor;   // the GAIN editor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxGainTabComponent)
};
