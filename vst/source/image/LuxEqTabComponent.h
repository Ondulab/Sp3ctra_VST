/**
 * @file LuxEqTabComponent.h
 * @brief Tab — EQ: typed-handle equalizer on the image-line stream.
 *
 * The interactive editor fills the page (ShapeEqComponent — typed handles
 * on the frequency axis). The background pole is chain-owned (rack header
 * selector), not a module setting. Page skeleton (padding, "--- EQ ---"
 * caption, height) = ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxeq{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho pages).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/ShapeEqComponent.h"

class LuxEqTabComponent : public juce::Component
{
public:
    /** Accent colour for the EQ page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Equalizer).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = ShapeEqComponent::kPreferredH;

    // top pad + editor + "--- EQ ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxEqTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p), editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive handle-curve editor (Sh0..Sh3 banks) ───────────
        editor.setMidiMap(&p.getMidiMap());   // right-click → MIDI Learn menu
        editor.setTitle("GAIN CURVE");
        addAndMakeVisible(editor);

        // ── Enable toggle ── rack LED + zone-3 header power switch

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the EQ bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        // Selection plumbing BEFORE the rebind so setInstance seeds the UI
        // from the processor's last CC-steered handle.
        constexpr int fam = EqHandleMidiTargets::FamilyEq;
        editor.selectionSink     = [this](int h)
        { processor.setEqSelectedHandle(fam, slot_, h); };
        editor.selectionProvider = [this]
        { return processor.getEqSelectedHandle(fam, slot_); };
        editor.setInstance(fam, slot_, [this](const juce::String& sfx)
                           { return eqParam(slot_, sfx.toRawUTF8()); });
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "EQ");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    ShapeEqComponent editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxEqTabComponent)
};
