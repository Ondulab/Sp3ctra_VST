/**
 * @file LuxCentroTabComponent.h
 * @brief Tab — CENTROID: mass-to-barycentre simplifier on the image-line stream.
 *
 * Page layout, top to bottom: the live stream editor (CentroEditorComponent
 * — the real flux through the real algorithm with the floor line, its LINE
 * SHAPE child view and the numeric boxes), the module's OUTPUT EQ curve
 * (EqEditorComponent bound to the luxcentro Band bank — the gain applied
 * after the barycentre redraw), then the remaining discrete controls below
 * (Background).
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
#include "../ui/CentroEditorComponent.h"
#include "../ui/EqEditorComponent.h"
#include "../processing/lux_centro.h"   // live glow reads the pool instance

class LuxCentroTabComponent : public juce::Component
{
public:
    /** Accent colour for the CENTROID page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Centroid).getARGB();   ///< inherited module colour

    /** The graphic editors stacked: stream (with its LINE SHAPE child and
     *  box row), output EQ. */
    static constexpr int kEditorsH =
        CentroEditorComponent::kPreferredH + 4
        + EqEditorComponent::kPreferredH;

    static constexpr int kPreferredH = kEditorsH + 4 + 22 + 30 + 8;

    explicit LuxCentroTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB)),
          eqEditor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Live stream editor (Floor) + LINE SHAPE + numeric boxes ────
        editor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(editor);

        // ── Output EQ curve (Band0..Band8, applied after the redraw) ───
        eqEditor.setMidiMap(&p.getMidiMap());
        eqEditor.liveProvider = [](int slot)
        {
            // Glow while the CENTROID instance runs AND its curve shapes the
            // output (a stale LUT means the curve is flat — see lux_centro.h).
            const LuxCentroState& st = *lux_centro_instance(slot);
            return st.config.enabled != 0 && st.centro_active != 0
                && st.eq_lut_px != 0;
        };
        addAndMakeVisible(eqEditor);

        // ── Background mode (which pole carries the material) ──────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Auto",  1);
        bgCombo.addItem("Black", 2);
        bgCombo.addItem("White", 3);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the CENTROID bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        bgAttach.reset();
        editor.setInstance(slot_,
                           ctParam(slot_, "Floor"), ctParam(slot_, "Thickness"),
                           ctParam(slot_, "Edge"));
        juce::StringArray bandIds;
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
            bandIds.add(ctParam(slot_, ("Band" + juce::String(b)).toRawUTF8()));
        eqEditor.setInstance(slot_, bandIds, ctParam(slot_, "NumPoints"));
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            processor.getAPVTS(), ctParam(slot_, "BackgroundMode"), bgCombo));
        bgLearn_ = std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), bgCombo, ctParam(slot_, "BackgroundMode"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("--- CENTROID ---", kPad,
                   kEditorsH + 6,
                   getWidth() - 2 * kPad, 12, juce::Justification::centred);
    }

    void resized() override
    {
        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;
        const int w      = getWidth() - 2 * kPad;

        int y = 4;
        editor.setBounds(kPad, y, w, CentroEditorComponent::kPreferredH);
        y += CentroEditorComponent::kPreferredH + 4;
        eqEditor.setBounds(kPad, y, w, EqEditorComponent::kPreferredH);

        const int rowY = kEditorsH + 4 + 22;
        bgLabel.setBounds(kPad, rowY, labelW, ch);
        bgCombo.setBounds(kPad + labelW + gap, rowY, 120, ch);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    CentroEditorComponent editor;     // stream + LINE SHAPE + numeric boxes
    EqEditorComponent     eqEditor;   // output EQ — luxcentro Band bank

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxCentroTabComponent)
};
