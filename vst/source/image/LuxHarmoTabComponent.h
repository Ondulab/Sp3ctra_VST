/**
 * @file LuxHarmoTabComponent.h
 * @brief Tab — SCALE: musical quantizer on the image-line stream.
 *
 * Mirrors the LuxEq page layout: an interactive scale-grid view on top
 * (HarmoEditorComponent — the comb over the frequency axis, click = root),
 * then the discrete controls below: Root / Scale / Mode combos, the
 * Strength / Width / Slope / Glide sliders and the Background combo.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxharmo{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxEq).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/HarmoEditorComponent.h"

class LuxHarmoTabComponent : public juce::Component
{
public:
    /** Accent colour for the SCALE page (matches the catalogue chip). */
    static constexpr uint32_t kAccentARGB = 0xff8fb84f;

    static constexpr int kPreferredH =
        HarmoEditorComponent::kPreferredH + 4 + 22 + 3 * 30 + 30 + 8;

    explicit LuxHarmoTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive scale-grid view (click a key = root) ───────────
        editor.setMidiMap(&p.getMidiMap());   // right-click canvas → learn Root
        addAndMakeVisible(editor);

        // ── Musical grid: Root / Scale / Mode ──────────────────────────
        initLabel(rootLabel, "Root");
        addAndMakeVisible(rootCombo);
        for (int i = 0; i < 12; ++i)
            rootCombo.addItem(juce::StringArray{"C", "C#", "D", "D#", "E", "F",
                                                "F#", "G", "G#", "A", "A#", "B"}[i],
                              i + 1);

        initLabel(scaleLabel, "Scale");
        addAndMakeVisible(scaleCombo);
        {
            const juce::StringArray scales{"Chromatic", "Major", "Minor",
                                           "Harm Minor", "Penta Maj", "Penta Min",
                                           "Blues", "Whole Tone", "Dorian",
                                           "Phrygian", "Lydian", "Mixolydian",
                                           "Fifths", "Octaves"};
            for (int i = 0; i < scales.size(); ++i)
                scaleCombo.addItem(scales[i], i + 1);
        }

        initLabel(modeLabel, "Mode");
        addAndMakeVisible(modeCombo);
        modeCombo.addItem("Mask", 1);   // comb: off-grid material fades out
        modeCombo.addItem("Warp", 2);   // reassign: material slides to the grid

        // ── Morph + comb shape + chord-change glide ────────────────────
        initSlider(strengthSlider, strengthLabel, "Strength");
        initSlider(widthSlider,    widthLabel,    "Width");
        initSlider(slopeSlider,    slopeLabel,    "Slope");
        initSlider(glideSlider,    glideLabel,    "Glide");

        // ── Background mode (which pole carries the material) ──────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Auto",  1);
        bgCombo.addItem("Black", 2);
        bgCombo.addItem("White", 3);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the SCALE bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);

        // Reset-first, then rebind (JUCE attachment rebind contract — a live
        // attachment must never see the new param through the old binding).
        rootAttach.reset();  scaleAttach.reset(); modeAttach.reset();
        strengthAttach.reset(); widthAttach.reset();
        slopeAttach.reset(); glideAttach.reset(); bgAttach.reset();

        editor.setInstance(slot_,
                           hmParam(slot_, "Root"),  hmParam(slot_, "Scale"),
                           hmParam(slot_, "Width"), hmParam(slot_, "Strength"));

        auto& ap = processor.getAPVTS();
        using CB = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using SL = juce::AudioProcessorValueTreeState::SliderAttachment;
        rootAttach    .reset(new CB(ap, hmParam(slot_, "Root"),     rootCombo));
        scaleAttach   .reset(new CB(ap, hmParam(slot_, "Scale"),    scaleCombo));
        modeAttach    .reset(new CB(ap, hmParam(slot_, "Mode"),     modeCombo));
        strengthAttach.reset(new SL(ap, hmParam(slot_, "Strength"), strengthSlider));
        widthAttach   .reset(new SL(ap, hmParam(slot_, "Width"),    widthSlider));
        slopeAttach   .reset(new SL(ap, hmParam(slot_, "Slope"),    slopeSlider));
        glideAttach   .reset(new SL(ap, hmParam(slot_, "Glide"),    glideSlider));
        bgAttach      .reset(new CB(ap, hmParam(slot_, "BackgroundMode"), bgCombo));

        auto& mm = processor.getMidiMap();
        scaleLearn_    = std::make_unique<MidiLearnAttachment>(mm, scaleCombo,     hmParam(slot_, "Scale"));
        modeLearn_     = std::make_unique<MidiLearnAttachment>(mm, modeCombo,      hmParam(slot_, "Mode"));
        strengthLearn_ = std::make_unique<MidiLearnAttachment>(mm, strengthSlider, hmParam(slot_, "Strength"));
        widthLearn_    = std::make_unique<MidiLearnAttachment>(mm, widthSlider,    hmParam(slot_, "Width"));
        slopeLearn_    = std::make_unique<MidiLearnAttachment>(mm, slopeSlider,    hmParam(slot_, "Slope"));
        glideLearn_    = std::make_unique<MidiLearnAttachment>(mm, glideSlider,    hmParam(slot_, "Glide"));
        bgLearn_       = std::make_unique<MidiLearnAttachment>(mm, bgCombo,        hmParam(slot_, "BackgroundMode"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("--- SCALE ---", kPad,
                   HarmoEditorComponent::kPreferredH + 6,
                   getWidth() - 2 * kPad, 12, juce::Justification::centred);
    }

    void resized() override
    {
        const int labelW = 58;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;
        const int w      = getWidth() - 2 * kPad;

        editor.setBounds(kPad, 4, w, HarmoEditorComponent::kPreferredH);

        int y = HarmoEditorComponent::kPreferredH + 4 + 22;

        // Row 1 — Root | Scale | Mode.
        {
            const int comboW = juce::jmax(64, (w - 3 * (labelW + gap) - 2 * gap) / 3);
            int x = kPad;
            rootLabel .setBounds(x, y, labelW, ch); x += labelW + gap;
            rootCombo .setBounds(x, y, comboW, ch); x += comboW + gap;
            scaleLabel.setBounds(x, y, labelW, ch); x += labelW + gap;
            scaleCombo.setBounds(x, y, comboW, ch); x += comboW + gap;
            modeLabel .setBounds(x, y, labelW, ch); x += labelW + gap;
            modeCombo .setBounds(x, y, comboW, ch);
        }
        y += 30;

        // Rows 2/3 — the four sliders, two per row.
        const int half = (w - gap) / 2;
        auto sliderRow = [&](juce::Label& l1, juce::Slider& s1,
                             juce::Label& l2, juce::Slider& s2)
        {
            l1.setBounds(kPad, y, labelW, ch);
            s1.setBounds(kPad + labelW + gap, y, half - labelW - gap, ch);
            l2.setBounds(kPad + half + gap, y, labelW, ch);
            s2.setBounds(kPad + half + gap + labelW + gap, y,
                         half - labelW - gap, ch);
            y += 30;
        };
        sliderRow(strengthLabel, strengthSlider, widthLabel, widthSlider);
        sliderRow(slopeLabel,    slopeSlider,    glideLabel, glideSlider);

        // Row 4 — Background.
        bgLabel.setBounds(kPad, y, labelW, ch);
        bgCombo.setBounds(kPad + labelW + gap, y, 120, ch);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    HarmoEditorComponent editor;

    juce::Label    rootLabel, scaleLabel, modeLabel, bgLabel;
    juce::Label    strengthLabel, widthLabel, slopeLabel, glideLabel;
    juce::ComboBox rootCombo, scaleCombo, modeCombo, bgCombo;
    juce::Slider   strengthSlider, widthSlider, slopeSlider, glideSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        rootAttach, scaleAttach, modeAttach, bgAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        strengthAttach, widthAttach, slopeAttach, glideAttach;
    std::unique_ptr<MidiLearnAttachment>
        scaleLearn_, modeLearn_, strengthLearn_, widthLearn_,
        slopeLearn_, glideLearn_, bgLearn_;

    static constexpr int kPad = 8;

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& sld, juce::Label& lbl, const juce::String& text)
    {
        initLabel(lbl, text);
        sld.setSliderStyle(juce::Slider::LinearHorizontal);
        sld.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52,
                            Sp3ctraTheme::kControlH);
        addAndMakeVisible(sld);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxHarmoTabComponent)
};
