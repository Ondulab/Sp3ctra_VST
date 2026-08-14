/**
 * @file LuxEqTabComponent.h
 * @brief Tab — EQ: graphic equalizer on the image-line stream.
 *
 * Mirrors the LuxEcho page layout: an interactive graphic editor on top
 * (EqEditorComponent — draggable gain nodes on the frequency axis), then the
 * remaining discrete controls below (Background).
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
#include "../ui/EqEditorComponent.h"

class LuxEqTabComponent : public juce::Component
{
public:
    /** Accent colour for the EQ page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Equalizer).getARGB();   ///< inherited module colour

    static constexpr int kPreferredH =
        EqEditorComponent::kPreferredH + 4 + 22 + 30 + 8;

    explicit LuxEqTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive gain-curve editor (Band0..Band8) ───────────────
        editor.setMidiMap(&p.getMidiMap());   // right-click a node → MIDI Learn
        addAndMakeVisible(editor);

        // ── Enable toggle ── rack LED + zone-3 header power switch

        // ── Background mode (which pole carries the material) ──────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Auto",  1);
        bgCombo.addItem("Black", 2);
        bgCombo.addItem("White", 3);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the EQ bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        bgAttach.reset();
        juce::StringArray bandIds;
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
            bandIds.add(eqParam(slot_, ("Band" + juce::String(b)).toRawUTF8()));
        editor.setInstance(slot_, bandIds, eqParam(slot_, "NumPoints"));
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            processor.getAPVTS(), eqParam(slot_, "BackgroundMode"), bgCombo));
        bgLearn_ = std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), bgCombo, eqParam(slot_, "BackgroundMode"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("--- EQ ---", kPad,
                   EqEditorComponent::kPreferredH + 6,
                   getWidth() - 2 * kPad, 12, juce::Justification::centred);
    }

    void resized() override
    {
        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;
        const int w      = getWidth() - 2 * kPad;

        editor.setBounds(kPad, 4, w, EqEditorComponent::kPreferredH);

        const int rowY = EqEditorComponent::kPreferredH + 4 + 22;
        bgLabel.setBounds(kPad, rowY, labelW, ch);
        bgCombo.setBounds(kPad + labelW + gap, rowY, 120, ch);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    EqEditorComponent editor;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxEqTabComponent)
};
