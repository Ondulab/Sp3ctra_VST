/**
 * @file LuxDcBlockTabComponent.h
 * @brief Tab — DC BLOCK: per-line mean removal on the image-line stream.
 *
 * The graphic view fills the page (DcBlockEditorComponent — the live stream
 * profile with the measured DC reference line and the display-only BLOCK
 * line; Amount is set from the box below). The background pole is chain-owned
 * (rack header selector), not a module setting. Page skeleton (padding,
 * "--- DC BLOCK ---" caption, height) = ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxdcblock{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxDrive).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/DcBlockEditorComponent.h"

class LuxDcBlockTabComponent : public juce::Component
{
public:
    /** Accent colour for the DC BLOCK page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::DcBlock).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = DcBlockEditorComponent::kPreferredH;

    // top pad + editor + "--- DC BLOCK ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxDcBlockTabComponent(Sp3ctraAudioProcessor& p)
        : editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive DC-removal editor (Amount) ─────────────────────────
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the DC BLOCK bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_, dcbParam(slot_, "Amount"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "DC BLOCK");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    int slot_ { 0 };   // pool slot of the bound instance

    DcBlockEditorComponent editor;   // the DC BLOCK editor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxDcBlockTabComponent)
};
