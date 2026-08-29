/**
 * @file LuxReverbTabComponent.h
 * @brief Tab — REVERB: visual reverberation on the image-line stream.
 *
 * The interactive graphic editor fills the page (ReverbEditorComponent —
 * Decay / Diffusion / Mix handles + live tail fill, numeric boxes below the
 * frame). The background pole is chain-owned (rack header selector), not a
 * module setting. Page skeleton (padding, "--- REVERBERATION ---" caption,
 * height) = ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxreverb{slot}_*
 * bank of the selected instance (same pattern as VideoScrollPage).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/ReverbEditorComponent.h"

class LuxReverbTabComponent : public juce::Component
{
public:
    /** Accent colour for the REVERB page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Reverb).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = ReverbEditorComponent::kPreferredH;

    // top pad + editor + "--- REVERBERATION ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxReverbTabComponent(Sp3ctraAudioProcessor& p)
        : editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive tail editor (Decay / Diffusion / Mix + live fill) ─
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        // ── Enable toggle ── rack LED + zone-3 header power switch

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the REVERB bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_, rvParam(slot_, "Decay"),
                           rvParam(slot_, "Diffusion"), rvParam(slot_, "Mix"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "REVERBERATION");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    int slot_ { 0 };   // pool slot of the bound instance

    ReverbEditorComponent editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxReverbTabComponent)
};
