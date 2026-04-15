/**
 * @file LuxStralTabComponent.h
 * @brief Tab 2 — LUXSTRAL: pipeline visual, source selector, toggles, output nodes.
 *
 * Pipeline:  Source → [Negative] → [DC Blocking] → [Gamma] → LUXSTRAL_GRAY / LUXSTRAL_COLOR / LUXSTRAL_BLOB
 *
 * UI style: follows the Synth-page charter (Label + Slider rows,
 * kFontSettings, centredRight justification). Boolean parameters are
 * rendered as sliders [0, 1] with step 1 for visual consistency.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "PipelineNodeComponent.h"
#include "VisualizerMode.h"
#include <functional>

class LuxStralTabComponent : public juce::Component
{
public:
    std::function<void(VisualizerMode)> onNodeClicked;

    explicit LuxStralTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          nodeGray("LUXSTRAL GRAY", juce::Colour(0xff6bb8e0), VisualizerMode::SPCTR_GRAY),
          nodeColor("LUXSTRAL COLOR", juce::Colour(0xff4ae0c8), VisualizerMode::SPCTR_COLOR),
          nodeBlob("LUXSTRAL BLOB", juce::Colour(0xff8888e0), VisualizerMode::SPCTR_BLOB)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector ───────────────────────────────────────────────
        initLabel(sourceLabel, "Source");
        addAndMakeVisible(sourceCombo);
        sourceCombo.addItem("S - Sampler", 1);
        sourceCombo.addItem("M - Mix",     2);
        sourceCombo.addItem("L - Live",    3);
        sourceAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxstralSource", sourceCombo));

        // ── Negative (ToggleButton — Synth-page charter) ────────────────
        initLabel(negativeLabel, "Negative");
        negativeToggle.setButtonText("Active");
        addAndMakeVisible(negativeToggle);
        negativeAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxstralInversion", negativeToggle));

        // ── DC Blocking (ToggleButton) ──────────────────────────────────
        initLabel(dcBlockLabel, "DC Blocking");
        dcBlockToggle.setButtonText("Active");
        addAndMakeVisible(dcBlockToggle);
        dcBlockAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxstralAcRemoval", dcBlockToggle));

        // ── Gamma slider ─────────────────────────────────────────────────
        initLabel(gammaLabel, "Gamma");
        gammaSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        gammaSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                    Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
        addAndMakeVisible(gammaSlider);
        gammaAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxstralGammaValue", gammaSlider));

        // ── Pipeline output nodes ────────────────────────────────────────
        for (auto* n : { &nodeGray, &nodeColor, &nodeBlob })
        {
            addAndMakeVisible(n);
            n->onClick = [this](VisualizerMode m)
            {
                setActiveMode(m);
                if (onNodeClicked) onNodeClicked(m);
            };
        }
    }

    void setActiveMode(VisualizerMode m)
    {
        nodeGray.setActive (m == VisualizerMode::SPCTR_GRAY);
        nodeColor.setActive(m == VisualizerMode::SPCTR_COLOR);
        nodeBlob.setActive (m == VisualizerMode::SPCTR_BLOB);
        nodeGray.setShowEye (m == VisualizerMode::SPCTR_GRAY);
        nodeColor.setShowEye(m == VisualizerMode::SPCTR_COLOR);
        nodeBlob.setShowEye (m == VisualizerMode::SPCTR_BLOB);
    }

    void paint(juce::Graphics& g) override
    {
        const auto accent = juce::Colour(0xff4fa3e0);
        const int pad = 12;
        const int stageW = getWidth() - 2 * pad;

        // Outputs section label
        g.setColour(accent.withAlpha(0.6f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("Pipeline outputs (click to visualize)",
                   pad, nodeY() - 16, stageW, 14, juce::Justification::centred);
    }

    void resized() override
    {
        const int w   = getWidth();
        const int ch  = Sp3ctraTheme::kControlH;
        const int nw  = stdNodeW();   // same width as Sources tab
        const int x0  = w / 2 - nw / 2;  // centred start X

        // Row 0: Source
        placeComboRow(sourceLabel, sourceCombo, 0);

        // Row 1: Negative (toggle)
        placeToggleRow(negativeLabel, negativeToggle, 1);

        // Row 2: DC Blocking (toggle)
        placeToggleRow(dcBlockLabel, dcBlockToggle, 2);

        // Row 3: Gamma
        placeSliderRow(gammaLabel, gammaSlider, 3);

        // Output nodes — stacked vertically, full stdNodeW each
        const int ny = nodeY();
        constexpr int nodeH  = 28;
        constexpr int nodeGap = 6;
        nodeGray.setBounds (x0, ny,                          nw, nodeH);
        nodeColor.setBounds(x0, ny + (nodeH + nodeGap),      nw, nodeH);
        nodeBlob.setBounds (x0, ny + 2 * (nodeH + nodeGap),  nw, nodeH);
    }

private:
    Sp3ctraAudioProcessor& processor;

    // Labels (Synth-page charter: kFontSettings, centredRight)
    juce::Label sourceLabel, negativeLabel, dcBlockLabel, gammaLabel;

    // Controls
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Slider       gammaSlider;

    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach, dcBlockAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gammaAttach;

    // Pipeline output nodes
    PipelineNodeComponent nodeGray, nodeColor, nodeBlob;

    // ── Helpers ───────────────────────────────────────────────────────────
    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    /// Same width formula as SourcesTabComponent
    int stdNodeW() const { return juce::jmin(getWidth() * 2 / 5, 360); }

    int rowY(int row) const
    {
        return 6 + row * (Sp3ctraTheme::kControlH + 14);
    }

    int nodeY() const { return rowY(4) + 18; }

    void placeComboRow(juce::Label& lbl, juce::ComboBox& combo, int row)
    {
        const int w  = getWidth();
        const int nw = stdNodeW();
        const int x0 = w / 2 - nw / 2;
        const int ch = Sp3ctraTheme::kControlH;
        const int y  = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        combo.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y,
                        nw - labelW - Sp3ctraTheme::kGap, ch);
    }

    void placeToggleRow(juce::Label& lbl, juce::ToggleButton& toggle, int row)
    {
        const int w  = getWidth();
        const int nw = stdNodeW();
        const int x0 = w / 2 - nw / 2;
        const int ch = Sp3ctraTheme::kControlH;
        const int y  = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        toggle.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y, 120, ch);
    }

    void placeSliderRow(juce::Label& lbl, juce::Slider& slider, int row)
    {
        const int w  = getWidth();
        const int nw = stdNodeW();
        const int x0 = w / 2 - nw / 2;
        const int ch = Sp3ctraTheme::kControlH;
        const int y  = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        slider.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y,
                         nw - labelW - Sp3ctraTheme::kGap, ch);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxStralTabComponent)
};
