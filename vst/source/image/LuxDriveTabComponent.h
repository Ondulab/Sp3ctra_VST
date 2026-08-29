/**
 * @file LuxDriveTabComponent.h
 * @brief Tab — LEVELS (internal type "Drive"): gain / saturation / floor stage
 *        on the image-line stream.
 *
 * Mirrors the LuxCentro page layout (ModuleChrome skeleton): an interactive
 * graphic editor on top (DriveEditorComponent — the live stream profile
 * through the real transfer, with the HORIZONTAL Floor line, the saturation
 * ramp and the numeric boxes), then the module's OUTPUT EQ curve
 * (ShapeEqComponent bound to the luxdrive Sh* bank — the gain applied after
 * the transfer), the "--- LEVELS ---" section caption, then the remaining
 * discrete control as a label-above row (Invert combo). The background pole
 * is chain-owned (rack header selector), not a module setting.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxdrive{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxCentro).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/DriveEditorComponent.h"
#include "../ui/ShapeEqComponent.h"
#include "../processing/lux_drive.h"   // live glow reads the pool instance

class LuxDriveTabComponent : public juce::Component
{
public:
    /** Accent colour for the LEVELS page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Drive).getARGB();   ///< inherited module colour

    /** Both graphic editors stacked: transfer on top, output EQ below. */
    static constexpr int kEditorsH =
        DriveEditorComponent::kPreferredH + ModuleChrome::kEditorGap
        + ShapeEqComponent::kPreferredH;

    /** Editors + section caption + one control row (Invert). */
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 1);

    explicit LuxDriveTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB)),
          eqEditor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive transfer-curve editor (Gain / Saturation / Floor) ──
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        // ── Output EQ curve (Sh* handles, applied after the transfer) ──────
        eqEditor.setMidiMap(&p.getMidiMap());
        eqEditor.setTitle("OUTPUT EQ");
        eqEditor.liveProvider = [](int slot)
        {
            // Glow while the LEVELS instance runs AND its curve shapes the
            // output (a stale LUT means the curve is flat — see lux_drive.h).
            const LuxDriveState& st = *lux_drive_instance(slot);
            return st.config.enabled != 0 && st.drive_active != 0
                && st.eq_lut_px != 0;
        };
        addAndMakeVisible(eqEditor);

        // ── Output inversion (Off / Negative / Luminance) ──────────────
        addAndMakeVisible(invCombo);
        invCombo.addItem("Off",       1);
        invCombo.addItem("Negative",  2);
        invCombo.addItem("Luminance", 3);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the DRIVE bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        invAttach.reset();
        editor.setInstance(slot_,
                           dvParam(slot_, "Gamma"), dvParam(slot_, "Saturation"),
                           dvParam(slot_, "Floor"), dvParam(slot_, "ContrastMin"));
        constexpr int fam = EqHandleMidiTargets::FamilyDrive;
        eqEditor.selectionSink     = [this](int h)
        { processor.setEqSelectedHandle(fam, slot_, h); };
        eqEditor.selectionProvider = [this]
        { return processor.getEqSelectedHandle(fam, slot_); };
        eqEditor.setInstance(fam, slot_, [this](const juce::String& sfx)
                             { return dvParam(slot_, sfx.toRawUTF8()); });
        invAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            processor.getAPVTS(), dvParam(slot_, "InvertMode"), invCombo));
        invLearn_ = std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), invCombo, dvParam(slot_, "InvertMode"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH,
                                         getWidth(), accent, "LEVELS");
        ModuleChrome::drawBoxLabel(g, invCombo, accent, "Invert");
    }

    void resized() override
    {
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth() - 2 * pad;

        int y = ModuleChrome::kPageTop;
        editor.setBounds(pad, y, w, DriveEditorComponent::kPreferredH);
        y += DriveEditorComponent::kPreferredH + ModuleChrome::kEditorGap;
        eqEditor.setBounds(pad, y, w, ShapeEqComponent::kPreferredH);
        y += ShapeEqComponent::kPreferredH;          // = kPageTop + kEditorsH

        // Control row under the section caption — label-above idiom; the
        // combo takes the left part of the row only.
        y += ModuleChrome::kSectionCaptionH;
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &invCombo });
        invCombo.setBounds(invCombo.getBounds().withWidth(juce::jmin(kComboW, w)));
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    DriveEditorComponent editor;   // the LEVELS transfer-curve editor
    ShapeEqComponent     eqEditor; // output EQ — luxdrive Sh* bank

    juce::ComboBox invCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> invAttach;
    std::unique_ptr<MidiLearnAttachment> invLearn_;

    static constexpr int kComboW = 160;   // a combo never stretches to the page width

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxDriveTabComponent)
};
