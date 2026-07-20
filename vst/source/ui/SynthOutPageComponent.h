/**
 * @file SynthOutPageComponent.h
 * @brief OUT (send) page — synth-split P2.
 *
 * Shown in ZONE 3 when a rack OUT block (→ LuxStral / → LuxSynth / → LuxWave)
 * is selected: the per-chain conditioning of the flux sent to the global
 * engine. The ENGINE parameters live on the engine pages, reached from the
 * ZONE-5 synth dock — this page only hosts the per-OUT bank:
 *
 *   Negative · DC Blocking · Gamma (1.0 = off) · Intensity (mix weight)
 *   + Contrast Min · Range dB for LuxStral sends.
 *
 * One instance is hosted by the editor and rebound per selection via
 * setTarget(type, slot) — the same rebind pattern as the pooled inserts.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "AudioPanelWidgets.h"   // AudioPanelUI::drawSectionBg / drawBadge
#include "ModuleCatalog.h"
#include <memory>
#include <vector>

class SynthOutPageComponent : public juce::Component
{
public:
    explicit SynthOutPageComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        negativeToggle.setButtonText("Negative");
        addAndMakeVisible(negativeToggle);
        dcBlockToggle.setButtonText("DC Blocking");
        addAndMakeVisible(dcBlockToggle);

        initLabel(gammaLabel, "Gamma");
        initSlider(gammaSlider);
        initLabel(contrastMinLabel, "Contrast Min");
        initSlider(contrastMinSlider);
        initLabel(rangeDbLabel, "Range dB");
        initSlider(rangeDbSlider);
        initLabel(intensityLabel, "Intensity");
        initSlider(intensitySlider);

        setTarget(ModuleType::LuxStral, 0);
    }

    /** Rebind every control to the OUT bank of (type, slot).
     *  slot: LuxStral engine index (0 = A, 1 = B); 0 for the others. */
    void setTarget(ModuleType t, int slot)
    {
        type_ = t;
        slot_ = juce::jlimit(0, 7, slot);

        auto& apvts = processor.getAPVTS();
        auto bank = [this](const char* suffix) -> juce::String
        {
            switch (type_)
            {
                case ModuleType::LuxSynth: return lxOutParam(slot_, suffix);
                case ModuleType::LuxWave:  return lwOutParam(slot_, suffix);
                case ModuleType::LuxGrain: return lgOutParam(slot_, suffix);
                default:                   return lsOutParam(slot_, suffix);
            }
        };

        // Detach EVERYTHING before rebinding: the new attachment's constructor
        // pushes the target bank's value into the shared widget with a SYNC
        // notification — an old attachment still listening would write that
        // value into the bank being left, silently converging the two banks
        // (the "linked OUT params across chains" bug).
        negativeAttach.reset();
        dcBlockAttach.reset();
        gammaAttach.reset();
        intensityAttach.reset();
        contrastMinAttach.reset();
        rangeDbAttach.reset();

        negativeAttach.reset(new BtnAttach(apvts, bank("negative"),   negativeToggle));
        dcBlockAttach .reset(new BtnAttach(apvts, bank("dcBlocking"), dcBlockToggle));
        gammaAttach   .reset(new SldAttach(apvts, bank("gamma"),      gammaSlider));
        intensityAttach.reset(new SldAttach(apvts, bank("intensity"), intensitySlider));

        const bool isStral = (type_ == ModuleType::LuxStral);
        if (isStral)
        {
            contrastMinAttach.reset(new SldAttach(apvts, bank("contrastMin"), contrastMinSlider));
            rangeDbAttach    .reset(new SldAttach(apvts, bank("rangeDb"),     rangeDbSlider));
        }
        contrastMinLabel .setVisible(isStral);
        contrastMinSlider.setVisible(isStral);
        rangeDbLabel     .setVisible(isStral);
        rangeDbSlider    .setVisible(isStral);

        // Right-click MIDI Learn — follows the bound bank.
        learnAtts_.clear();
        auto& mm = processor.getMidiMap();
        auto learn = [&](juce::Component& c, const char* suffix)
        {
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, c, bank(suffix)));
        };
        learn(negativeToggle,   "negative");
        learn(dcBlockToggle,    "dcBlocking");
        learn(gammaSlider,      "gamma");
        learn(intensitySlider,  "intensity");
        if (isStral)
        {
            learn(contrastMinSlider, "contrastMin");
            learn(rangeDbSlider,     "rangeDb");
        }

        resized();
        repaint();
    }

    ModuleType targetType() const noexcept { return type_; }
    int        targetSlot() const noexcept { return slot_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto L = computeGeom(getWidth());
        using namespace AudioPanelUI;

        // (No identity header — the selected rack block already names the
        //  send; the section badge is enough context.)
        drawSectionBg(g, L.imgBg.getX(), L.imgBg.getY(), L.imgBg.getWidth(), L.imgBg.getHeight());
        drawBadge(g, L.imgBadge.getX(), L.imgBadge.getY(), L.imgBadge.getWidth(),
                  0xff20303c, 0xff7aade0, "IMAGE  --  CONDITIONING");

        // Footnote: where the engine parameters live now.
        g.setColour(juce::Colour(0xff5a5a66));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText("Engine parameters live in the AUDIO MIX panel (click the engine strip).",
                   L.footer, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        const auto L = computeGeom(getWidth());
        negativeToggle.setBounds(L.negToggle);
        dcBlockToggle.setBounds(L.dcToggle);
        gammaLabel.setBounds(L.gammaLabel);             gammaSlider.setBounds(L.gammaSlider);
        contrastMinLabel.setBounds(L.contrastLabel);    contrastMinSlider.setBounds(L.contrastSlider);
        rangeDbLabel.setBounds(L.rangeLabel);           rangeDbSlider.setBounds(L.rangeSlider);
        intensityLabel.setBounds(L.intensityLabel);     intensitySlider.setBounds(L.intensitySlider);
    }

    // Natural height — sized for the tallest variant (LuxStral: 5 rows).
    static constexpr int kPreferredH = 6                                  // pad
                                     + Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + 5 * Sp3ctraTheme::kControlH + 4 * Sp3ctraTheme::kRowGap
                                     + 8                                  // section bottom pad
                                     + 6 + 16 + 8;                        // gap + footer + pad

private:
    static constexpr int kTopPad  = 6;
    static constexpr int kBadgeH  = Sp3ctraTheme::kSectionH;
    static constexpr int kBadgeGap= Sp3ctraTheme::kSectionGap;
    static constexpr int kRowH    = Sp3ctraTheme::kControlH;
    static constexpr int kRowGap  = Sp3ctraTheme::kRowGap;
    static constexpr int kSecPadB = 8;
    static constexpr int kInsetX  = 8;
    static constexpr int kLabelW  = 96;
    static constexpr int kMaxW    = 560;

    struct Geom
    {
        juce::Rectangle<int> imgBg, imgBadge,
                             negToggle, dcToggle,
                             gammaLabel, gammaSlider,
                             contrastLabel, contrastSlider,
                             rangeLabel, rangeSlider,
                             intensityLabel, intensitySlider,
                             footer;
    };

    Geom computeGeom(int w) const
    {
        Geom L{};
        const int colW = juce::jmin(kMaxW, juce::jmax(240, w - 2 * Sp3ctraTheme::kHPad));
        const int x    = Sp3ctraTheme::kHPad;
        const int cx   = x + kInsetX;
        const int cw   = colW - 2 * kInsetX;
        const int gap  = Sp3ctraTheme::kGap;
        int y = kTopPad;

        const bool isStral = (type_ == ModuleType::LuxStral);
        const int rows = isStral ? 5 : 3;
        const int secH = kBadgeH + kBadgeGap + rows * kRowH + (rows - 1) * kRowGap + kSecPadB;

        L.imgBg    = { x - 2, y, colW + 4, secH };
        L.imgBadge = { x, y, colW, kBadgeH };
        int cy = y + kBadgeH + kBadgeGap;
        {
            const int half = (cw - gap) / 2;
            L.negToggle = { cx, cy, half, kRowH };
            L.dcToggle  = { cx + half + gap, cy, half, kRowH };
            cy += kRowH + kRowGap;
        }
        L.gammaLabel  = { cx, cy, kLabelW, kRowH };
        L.gammaSlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
        cy += kRowH + kRowGap;
        if (isStral)
        {
            L.contrastLabel  = { cx, cy, kLabelW, kRowH };
            L.contrastSlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
            cy += kRowH + kRowGap;
            L.rangeLabel  = { cx, cy, kLabelW, kRowH };
            L.rangeSlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
            cy += kRowH + kRowGap;
        }
        L.intensityLabel  = { cx, cy, kLabelW, kRowH };
        L.intensitySlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };

        L.footer = { x, y + secH + 6, colW, 16 };
        return L;
    }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(s);
    }

    Sp3ctraAudioProcessor& processor;
    ModuleType type_ { ModuleType::LuxStral };
    int        slot_ { 0 };

    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Label        gammaLabel, contrastMinLabel, rangeDbLabel, intensityLabel;
    juce::Slider       gammaSlider, contrastMinSlider, rangeDbSlider, intensitySlider;

    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SldAttach> gammaAttach, contrastMinAttach, rangeDbAttach, intensityAttach;
    std::unique_ptr<BtnAttach> negativeAttach, dcBlockAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthOutPageComponent)
};
