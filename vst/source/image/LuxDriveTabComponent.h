/**
 * @file LuxDriveTabComponent.h
 * @brief Tab — LEVELS (internal type "Drive"): gain / saturation / floor stage
 *        on the image-line stream.
 *
 * Mirrors the LuxCentro page layout: an interactive graphic editor on top
 * (DriveEditorComponent — the live stream profile through the real transfer,
 * with the HORIZONTAL Floor line + Gain / Saturation handles + numeric
 * boxes), then the module's OUTPUT EQ curve (EqEditorComponent bound to the
 * luxdrive Band bank — the gain applied after the transfer), then the
 * remaining discrete controls below (Background).
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
#include "../ui/DriveEditorComponent.h"
#include "../ui/EqEditorComponent.h"
#include "../processing/lux_drive.h"   // live glow reads the pool instance

class LuxDriveTabComponent : public juce::Component
{
public:
    /** Accent colour for the LEVELS page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Drive).getARGB();   ///< inherited module colour

    /** Both graphic editors stacked: transfer on top, output EQ below. */
    static constexpr int kEditorsH =
        DriveEditorComponent::kPreferredH + 4 + EqEditorComponent::kPreferredH;

    static constexpr int kPreferredH = kEditorsH + 4 + 22 + 30 + 8;

    explicit LuxDriveTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB)),
          eqEditor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive transfer-curve editor (Gain / Saturation / Floor) ──
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        // ── Output EQ curve (Band0..Band8, applied after the transfer) ─────
        eqEditor.setMidiMap(&p.getMidiMap());
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
        initLabel(invLabel, "Invert");
        addAndMakeVisible(invCombo);
        invCombo.addItem("Off",       1);
        invCombo.addItem("Negative",  2);
        invCombo.addItem("Luminance", 3);

        // ── Background mode (which pole carries the material) ──────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Auto",  1);
        bgCombo.addItem("Black", 2);
        bgCombo.addItem("White", 3);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the DRIVE bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        bgAttach.reset();
        invAttach.reset();
        editor.setInstance(slot_,
                           dvParam(slot_, "Gamma"), dvParam(slot_, "Saturation"),
                           dvParam(slot_, "Floor"), dvParam(slot_, "ContrastMin"));
        juce::StringArray bandIds;
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
            bandIds.add(dvParam(slot_, ("Band" + juce::String(b)).toRawUTF8()));
        eqEditor.setInstance(slot_, bandIds, dvParam(slot_, "NumPoints"));
        invAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            processor.getAPVTS(), dvParam(slot_, "InvertMode"), invCombo));
        invLearn_ = std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), invCombo, dvParam(slot_, "InvertMode"));
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            processor.getAPVTS(), dvParam(slot_, "BackgroundMode"), bgCombo));
        bgLearn_ = std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), bgCombo, dvParam(slot_, "BackgroundMode"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("--- LEVELS ---", kPad,
                   kEditorsH + 6,
                   getWidth() - 2 * kPad, 12, juce::Justification::centred);
    }

    void resized() override
    {
        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;
        const int w      = getWidth() - 2 * kPad;

        editor.setBounds(kPad, 4, w, DriveEditorComponent::kPreferredH);
        eqEditor.setBounds(kPad, 4 + DriveEditorComponent::kPreferredH + 4,
                           w, EqEditorComponent::kPreferredH);

        const int rowY = kEditorsH + 4 + 22;
        invLabel.setBounds(kPad, rowY, labelW, ch);
        invCombo.setBounds(kPad + labelW + gap, rowY, 120, ch);
        const int bgX = kPad + labelW + gap + 120 + 16;
        bgLabel.setBounds(bgX, rowY, labelW, ch);
        bgCombo.setBounds(bgX + labelW + gap, rowY, 120, ch);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    DriveEditorComponent editor;   // the LEVELS transfer-curve editor
    EqEditorComponent    eqEditor; // output EQ — luxdrive Band bank

    juce::Label    invLabel;
    juce::ComboBox invCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> invAttach;
    std::unique_ptr<MidiLearnAttachment> invLearn_;

    juce::Label    bgLabel;
    juce::ComboBox bgCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bgAttach;
    std::unique_ptr<MidiLearnAttachment> bgLearn_;

    static constexpr int kPad = 8;

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxDriveTabComponent)
};
