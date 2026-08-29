/**
 * @file LuxCentroTabComponent.h
 * @brief Tab — CENTROID: mass-to-barycentre simplifier on the image-line stream.
 *
 * Page layout (ModuleChrome skeleton), top to bottom: the live stream editor
 * (CentroEditorComponent — the real flux through the real algorithm with the
 * floor line, its LINE SHAPE and WIDTH LAW child views and the numeric
 * boxes), then the module's OUTPUT EQ curve (ShapeEqComponent bound to the
 * luxcentro Sh* bank — the gain applied after the barycentre redraw), then
 * the "--- CENTROID ---" section caption. The width law selector (PX / ERB)
 * lives on the SETUP face (CentroSetupPanel); the background pole is
 * chain-owned (rack header selector), not a module setting.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxcentro{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxHarmo).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/CentroEditorComponent.h"
#include "../ui/ShapeEqComponent.h"
#include "../processing/lux_centro.h"   // live glow reads the pool instance

class LuxCentroTabComponent : public juce::Component
{
public:
    /** Accent colour for the CENTROID page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Centroid).getARGB();   ///< inherited module colour

    /** The graphic editors stacked: stream (with its LINE SHAPE / WIDTH LAW
     *  children and box row), output EQ. */
    static constexpr int kEditorsH =
        CentroEditorComponent::kPreferredH + ModuleChrome::kEditorGap
        + ShapeEqComponent::kPreferredH;

    /** Editors + section caption (no extra control rows). */
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 0);

    explicit LuxCentroTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB)),
          eqEditor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Live stream editor (Floor) + LINE SHAPE + WIDTH LAW + boxes ─
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        // ── Output EQ curve (Sh* handles, applied after the redraw) ────
        eqEditor.setMidiMap(&p.getMidiMap());
        eqEditor.setTitle("OUTPUT EQ");
        eqEditor.liveProvider = [](int slot)
        {
            // Glow while the CENTROID instance runs AND its curve shapes the
            // output (a stale LUT means the curve is flat — see lux_centro.h).
            const LuxCentroState& st = *lux_centro_instance(slot);
            return st.config.enabled != 0 && st.centro_active != 0
                && st.eq_lut_px != 0;
        };
        addAndMakeVisible(eqEditor);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the CENTROID bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        editor.setInstance(slot_,
                           ctParam(slot_, "Floor"), ctParam(slot_, "Thickness"),
                           ctParam(slot_, "Edge"), ctParam(slot_, "WidthTilt"));
        constexpr int fam = EqHandleMidiTargets::FamilyCentro;
        eqEditor.selectionSink     = [this](int h)
        { processor.setEqSelectedHandle(fam, slot_, h); };
        eqEditor.selectionProvider = [this]
        { return processor.getEqSelectedHandle(fam, slot_); };
        eqEditor.setInstance(fam, slot_, [this](const juce::String& sfx)
                             { return ctParam(slot_, sfx.toRawUTF8()); });
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH,
                                         getWidth(), accent, "CENTROID");
    }

    void resized() override
    {
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth() - 2 * pad;

        int y = ModuleChrome::kPageTop;
        editor.setBounds(pad, y, w, CentroEditorComponent::kPreferredH);
        y += CentroEditorComponent::kPreferredH + ModuleChrome::kEditorGap;
        eqEditor.setBounds(pad, y, w, ShapeEqComponent::kPreferredH);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    CentroEditorComponent editor;     // stream + LINE SHAPE + WIDTH LAW + boxes
    ShapeEqComponent      eqEditor;   // output EQ — luxcentro Sh* bank

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxCentroTabComponent)
};
