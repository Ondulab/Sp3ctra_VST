/**
 * @file LuxDiffTabComponent.h
 * @brief Tab — DIFF: per-pixel reference subtraction on the image-line stream.
 *
 * The graphic view fills the page (DiffEditorComponent — the live stream
 * profile, the captured reference and the differential preview; Amount /
 * Mode / Follow / Time / CAPTURE / CLEAR live in the row below). The background pole is
 * chain-owned (rack header selector), not a module setting. Page skeleton
 * (padding, "--- DIFF ---" caption, height) = ModuleChrome.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxdiff{slot}_*
 * bank of the selected instance (same pattern as LuxGain/LuxDcBlock). The two
 * action buttons reach the processor (requestDiffCapture / requestDiffClear),
 * which owns the request path to the image thread; their MIDI targets are the
 * virtual ids luxdiff{slot}_Capture / luxdiff{slot}_Clear.
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/DiffEditorComponent.h"

class LuxDiffTabComponent : public juce::Component
{
public:
    /** Accent colour for the DIFF page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Diff).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH = DiffEditorComponent::kPreferredH;

    // top pad + editor + "--- DIFF ---" + bottom pad
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH);

    explicit LuxDiffTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p), editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive diff editor (Amount / Mode / CAPTURE / CLEAR) ──────
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        editor.onCapture = [this] { processor.requestDiffCapture(slot_); };
        editor.onClear   = [this] { processor.requestDiffClear(slot_);   };
        addAndMakeVisible(editor);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the DIFF bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_,
                           dfParam(slot_, "Amount"), dfParam(slot_, "Mode"),
                           dfParam(slot_, "Follow"), dfParam(slot_, "Time"),
                           Sp3ctraAudioProcessor::diffCaptureMidiId(slot_),
                           Sp3ctraAudioProcessor::diffClearMidiId(slot_));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         juce::Colour(kAccentARGB), "DIFF");
    }

    void resized() override
    {
        editor.setBounds(ModuleChrome::kPagePad, ModuleChrome::kPageTop,
                         getWidth() - 2 * ModuleChrome::kPagePad, kEditorsH);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    DiffEditorComponent editor;   // the DIFF editor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxDiffTabComponent)
};
