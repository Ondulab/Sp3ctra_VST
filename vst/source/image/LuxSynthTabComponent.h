/**
 * @file LuxSynthTabComponent.h
 * @brief Tab 3 — LUXSYNTH: pipeline visual, source selector, toggles, output nodes.
 *
 * Pipeline:  Source → [Negative] → [DC Blocking] → Gamma →
 *            SYNTH_GRAY / SYNTH_COLOR / SYNTH_BLOB → FFT → FFT_GRAY / FFT_COLOR
 *
 * Gamma is always active (no enable toggle) — set to 1.0 for identity (no-op).
 *
 * UI style: follows the Synth-page charter (Label + Slider rows,
 * kFontSettings, centredRight justification). Boolean parameters are
 * rendered as ToggleButtons for visual consistency with LUXSTRAL tab.
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

        // ── Negative (ToggleButton) ───────────────────────────────────────
        initLabel(negativeLabel, "Negative");
        negativeToggle.setButtonText("Active");
        addAndMakeVisible(negativeToggle);
        negativeAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxsynthInversion", negativeToggle));

        // ── DC Blocking (ToggleButton) ────────────────────────────────────
        initLabel(dcBlockLabel, "DC Blocking");
        dcBlockToggle.setButtonText("Active");
        addAndMakeVisible(dcBlockToggle);
        dcBlockAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxsynthAcRemoval", dcBlockToggle));

        // ── Gamma Value (Slider) — always active, 1.0 = no-op ────────────
        initLabel(gammaValueLabel, "Gamma");
        gammaSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        gammaSlider.setTextBoxStyle(juce::Slider::TextBoxRight,
                                    false,
                                    50,
                                    Sp3ctraTheme::kControlH);
        addAndMakeVisible(gammaSlider);
        gammaValueAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxsynthGammaValue", gammaSlider));

        // ── BLOB DETECTION — LuxSynth-only params (isolated from LuxStral) ──
        // Row 4: Amplitude threshold — expressed as normalised brightness [0..1].
        // 0.05 means pixels brighter than 5% of max amplitude are considered active.
        initLabel(blobThreshLabel, "Ampl. Thr.");
        blobThreshSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobThreshSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                         50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobThreshSlider);
        blobThreshAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "lxBlobThreshold", blobThreshSlider));

        // Row 5: Pixel threshold — minimum blob span in CIS pixels.
        // Acts as a width filter: blobs narrower than this value are discarded.
        initLabel(blobMinWidthLabel, "Pix. Thr.");
        blobMinWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMinWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                           50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMinWidthSlider);
        blobMinWidthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "lxBlobMinWidth", blobMinWidthSlider));

        // Row 7: Merge Gap
        initLabel(blobMergeGapLabel, "Merge Gap");
        blobMergeGapSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMergeGapSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                            50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMergeGapSlider);
        blobMergeGapAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "lxBlobMergeGap", blobMergeGapSlider));

        // Row 7: Color Split — how aggressively color differences cause splits.
        // 0% = no color-based split (pure gap-based merge, color ignored).
        // 100% = maximum split: any color divergence breaks a blob, even within
        //        a continuous active region (independent of Merge Gap).
        initLabel(blobColorSplitLabel, "Color Split");
        blobColorSplitSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobColorSplitSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                              50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobColorSplitSlider);
        blobColorSplitAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "lxBlobColorSplit", blobColorSplitSlider));

        // ── FFT PARAMETERS ────────────────────────────────────────────────
        // Row 8: FFT Bins — number of harmonics extracted from spatial FFT.
        // Each bin maps to one LuxSynth oscillator in the additive synthesis engine.
        // 32 = fast / low-res, 256 = slow / high-res (default 128).
        initLabel(fftBinsLabel, "FFT Bins");
        addAndMakeVisible(fftBinsCombo);
        fftBinsCombo.addItem("32  \xe2\x80\x94 fast",     1);
        fftBinsCombo.addItem("64",                         2);
        fftBinsCombo.addItem("128 \xe2\x80\x94 default",  3);
        fftBinsCombo.addItem("256 \xe2\x80\x94 quality",  4);
        fftBinsAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "lxFftBins", fftBinsCombo));

        // Row 9: FFT Smoothing — temporal averaging of FFT magnitudes.
        // 0 = very reactive (fast attack + fast release).
        // 1 = very smooth   (slow attack + slow release).
        initLabel(fftSmoothingLabel, "Smoothing");
        fftSmoothingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        fftSmoothingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                           50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(fftSmoothingSlider);
        fftSmoothingAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "lxFftSmoothing", fftSmoothingSlider));

        // ── All output nodes ──────────────────────────────────────────────
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
        const auto blobAccent = juce::Colour(0xffd07040); // SYNTH_BLOB colour
        const int pad = 12;
        const int stageW = getWidth() - 2 * pad;

        // ── Blob Detection section header (between Gamma row and blob sliders) ──
        // Draws at row 4 gap — between row 3 bottom and row 4 top
        const int blobSectionY = rowY(3) + Sp3ctraTheme::kControlH + 2;
        g.setColour(blobAccent.withAlpha(0.55f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("--- BLOB DETECTION ---", pad, blobSectionY, stageW, 12,
                   juce::Justification::centred);

        // ── FFT Parameters section header (between blob sliders and FFT rows) ──
        const int fftSectionY = rowY(7) + Sp3ctraTheme::kControlH + 2;
        g.setColour(juce::Colour(0xffe06868).withAlpha(0.55f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("--- FFT PARAMETERS ---", pad, fftSectionY, stageW, 12,
                   juce::Justification::centred);

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

        // Row 3: Gamma (slider — always active)
        placeSliderRow(gammaValueLabel, gammaSlider, 3);

        // Rows 4-7: Blob Detection sliders
        // (row 4 section header is drawn in paint() in the gap between row 3 and row 4)
        placeSliderRow(blobThreshLabel,    blobThreshSlider,    4);
        placeSliderRow(blobMinWidthLabel,  blobMinWidthSlider,  5);
        placeSliderRow(blobMergeGapLabel,  blobMergeGapSlider,  6);
        placeSliderRow(blobColorSplitLabel, blobColorSplitSlider, 7);

        // Rows 8-9: FFT Parameters
        // (row 8 section header is drawn in paint() in the gap between row 7 and row 8)
        placeComboRow (fftBinsLabel,      fftBinsCombo,      8);
        placeSliderRow(fftSmoothingLabel, fftSmoothingSlider, 9);

        // Preliminary nodes — stacked vertically, full stdNodeW each
        constexpr int nodeH   = 28;
        constexpr int nodeGap = 6;
        const int pny = preNodeY();
        nodeGray.setBounds (x0, pny,                          nw, nodeH);
        nodeColor.setBounds(x0, pny + (nodeH + nodeGap),      nw, nodeH);
        nodeBlob.setBounds (x0, pny + 2 * (nodeH + nodeGap),  nw, nodeH);

        // FFT nodes — stacked vertically, full stdNodeW each
        const int fny = fftNodeY();
        nodeFftGray.setBounds (x0, fny,                     nw, nodeH);
        nodeFftColor.setBounds(x0, fny + (nodeH + nodeGap), nw, nodeH);
    }

private:
    Sp3ctraAudioProcessor& processor;

    // Labels — image pipeline
    juce::Label sourceLabel, negativeLabel, dcBlockLabel, gammaValueLabel;
    // Labels — blob detection (LuxSynth-only, isolated from LuxStral)
    juce::Label blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel, blobColorSplitLabel;
    // Labels — FFT parameters
    juce::Label fftBinsLabel, fftSmoothingLabel;

    // Controls — image pipeline
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Slider       gammaSlider;
    // Controls — blob detection
    juce::Slider blobThreshSlider, blobMinWidthSlider, blobMergeGapSlider, blobColorSplitSlider;
    // Controls — FFT parameters
    juce::ComboBox fftBinsCombo;
    juce::Slider   fftSmoothingSlider;

    // Attachments — image pipeline
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach,
                                                                             dcBlockAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gammaValueAttach;
    // Attachments — blob detection
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   blobThreshAttach,
                                                                             blobMinWidthAttach,
                                                                             blobMergeGapAttach,
                                                                             blobColorSplitAttach;
    // Attachments — FFT parameters
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> fftBinsAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   fftSmoothingAttach;

    // Pipeline output nodes
    PipelineNodeComponent nodeGray, nodeColor, nodeBlob, nodeFftGray, nodeFftColor;

    // ── Helpers ───────────────────────────────────────────────────────────────
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

    // 10 control rows (0-7: pipeline + blob-detection, 8-9: FFT parameters),
    // then 38 px gap before preliminary output nodes.
    int preNodeY() const { return rowY(10) + 38; }
    // 3 stacked nodes: 3 × 28 + 2 × 6 = 96, plus 20 for label spacing
    int fftNodeY() const { return preNodeY() + 96 + 20; }

    void placeComboRow(juce::Label& lbl, juce::ComboBox& combo, int row)
    {
        const int nw     = stdNodeW();
        const int x0     = getWidth() / 2 - nw / 2;
        const int ch     = Sp3ctraTheme::kControlH;
        const int y      = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        combo.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y,
                        nw - labelW - Sp3ctraTheme::kGap, ch);
    }

    void placeToggleRow(juce::Label& lbl, juce::ToggleButton& toggle, int row)
    {
        const int nw     = stdNodeW();
        const int x0     = getWidth() / 2 - nw / 2;
        const int ch     = Sp3ctraTheme::kControlH;
        const int y      = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        toggle.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y, 120, ch);
    }

    void placeSliderRow(juce::Label& lbl, juce::Slider& slider, int row)
    {
        const int nw     = stdNodeW();
        const int x0     = getWidth() / 2 - nw / 2;
        const int ch     = Sp3ctraTheme::kControlH;
        const int y      = rowY(row);
        const int labelW = 80;
        lbl.setBounds(x0, y, labelW, ch);
        slider.setBounds(x0 + labelW + Sp3ctraTheme::kGap, y,
                         nw - labelW - Sp3ctraTheme::kGap, ch);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthTabComponent)
};
