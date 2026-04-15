/**
 * @file LuxSynthTabComponent.h
 * @brief Tab 3 — LUXSYNTH: pipeline visual, source selector, toggles, output nodes.
 *
 * Pipeline:  Source → [Negative] → [DC Blocking] → NO GAMMA →
 *            SYNTH_GRAY / SYNTH_COLOR / SYNTH_BLOB → FFT → FFT_GRAY / FFT_COLOR
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

class LuxSynthTabComponent : public juce::Component
{
public:
    std::function<void(VisualizerMode)> onNodeClicked;

    explicit LuxSynthTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          nodeGray     ("SYNTH GRAY",    juce::Colour(0xffe0a84a), VisualizerMode::SYNTH_GRAY),
          nodeColor    ("SYNTH COLOR",   juce::Colour(0xffe0c864), VisualizerMode::SYNTH_COLOR),
          nodeBlob     ("SYNTH BLOB",    juce::Colour(0xffd07040), VisualizerMode::SYNTH_BLOB),
          nodeFftGray  ("FFT GRAY",      juce::Colour(0xffe06868), VisualizerMode::SYNTH_FFT_GRAY),
          nodeFftColor ("FFT COLOR",     juce::Colour(0xffcc88cc), VisualizerMode::SYNTH_FFT_COLOR)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector ───────────────────────────────────────────────
        initLabel(sourceLabel, "Source");
        addAndMakeVisible(sourceCombo);
        sourceCombo.addItem("S - Sampler", 1);
        sourceCombo.addItem("M - Mix",     2);
        sourceCombo.addItem("L - Live",    3);
        sourceAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxsynthSource", sourceCombo));

        // ── Negative (ToggleButton — Synth-page charter) ────────────────
        initLabel(negativeLabel, "Negative");
        negativeToggle.setButtonText("Active");
        addAndMakeVisible(negativeToggle);
        negativeAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxsynthInversion", negativeToggle));

        // ── DC Blocking (ToggleButton) ──────────────────────────────────
        initLabel(dcBlockLabel, "DC Blocking");
        dcBlockToggle.setButtonText("Active");
        addAndMakeVisible(dcBlockToggle);
        dcBlockAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxsynthAcRemoval", dcBlockToggle));

        // ── All output nodes ─────────────────────────────────────────────
        for (auto* n : { &nodeGray, &nodeColor, &nodeBlob, &nodeFftGray, &nodeFftColor })
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
        nodeGray.setActive     (m == VisualizerMode::SYNTH_GRAY);
        nodeColor.setActive    (m == VisualizerMode::SYNTH_COLOR);
        nodeBlob.setActive     (m == VisualizerMode::SYNTH_BLOB);
        nodeFftGray.setActive  (m == VisualizerMode::SYNTH_FFT_GRAY);
        nodeFftColor.setActive (m == VisualizerMode::SYNTH_FFT_COLOR);
        nodeGray.setShowEye     (m == VisualizerMode::SYNTH_GRAY);
        nodeColor.setShowEye    (m == VisualizerMode::SYNTH_COLOR);
        nodeBlob.setShowEye     (m == VisualizerMode::SYNTH_BLOB);
        nodeFftGray.setShowEye  (m == VisualizerMode::SYNTH_FFT_GRAY);
        nodeFftColor.setShowEye (m == VisualizerMode::SYNTH_FFT_COLOR);
    }

    void paint(juce::Graphics& g) override
    {
        const auto accent = juce::Colour(0xffe08844);
        const int pad = 12;
        const int stageW = getWidth() - 2 * pad;

        // Preliminary outputs label
        g.setColour(accent.withAlpha(0.6f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("Preliminary outputs", pad, preNodeY() - 16, stageW, 14,
                   juce::Justification::centred);

        // FFT outputs label
        g.setColour(accent.withAlpha(0.6f));
        g.drawText("Spectral analysis (FFT)", pad, fftNodeY() - 16, stageW, 14,
                   juce::Justification::centred);
    }

    void resized() override
    {
        const int w  = getWidth();
        const int nw = stdNodeW();
        const int x0 = w / 2 - nw / 2;

        // Row 0: Source
        placeComboRow(sourceLabel, sourceCombo, 0);

        // Row 1: Negative (toggle)
        placeToggleRow(negativeLabel, negativeToggle, 1);

        // Row 2: DC Blocking (toggle)
        placeToggleRow(dcBlockLabel, dcBlockToggle, 2);

        // Preliminary nodes — stacked vertically, full stdNodeW each
        constexpr int nodeH  = 28;
        constexpr int nodeGap = 6;
        const int pny = preNodeY();
        nodeGray.setBounds (x0, pny,                          nw, nodeH);
        nodeColor.setBounds(x0, pny + (nodeH + nodeGap),      nw, nodeH);
        nodeBlob.setBounds (x0, pny + 2 * (nodeH + nodeGap),  nw, nodeH);

        // FFT nodes — stacked vertically, full stdNodeW each
        const int fny = fftNodeY();
        nodeFftGray.setBounds (x0, fny,                    nw, nodeH);
        nodeFftColor.setBounds(x0, fny + (nodeH + nodeGap), nw, nodeH);
    }

private:
    Sp3ctraAudioProcessor& processor;

    // Labels
    juce::Label sourceLabel, negativeLabel, dcBlockLabel;

    // Controls
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;

    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach, dcBlockAttach;

    // Pipeline output nodes
    PipelineNodeComponent nodeGray, nodeColor, nodeBlob, nodeFftGray, nodeFftColor;

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

    int preNodeY() const { return rowY(3) + 38; }
    // 3 stacked nodes: 3 × 28 + 2 × 6 = 96, plus 20 for label spacing
    int fftNodeY() const { return preNodeY() + 96 + 20; }

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthTabComponent)
};
