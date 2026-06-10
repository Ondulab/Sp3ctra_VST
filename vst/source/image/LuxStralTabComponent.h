/**
 * @file LuxStralTabComponent.h
 * @brief Tab 2 — LUXSTRAL: pipeline visual, source selector, toggles, output nodes.
 *
 * Pipeline:  Source → [Negative] → [DC Blocking] → [Gamma] →
 *            LUXSTRAL_GRAY / LUXSTRAL_COLOR / LUXSTRAL_BLOB
 *
 * Two-column layout (mirrors LuxSynthTabComponent):
 *   Left  — image pipeline controls + blob detection parameters
 *   Right — pipeline output nodes
 *
 * Blob detection controls are wired to spctrBlob* APVTS parameters:
 *   spctrBlobThreshold / spctrBlobMinWidth / spctrBlobMergeGap / spctrBlobColorSplit
 * These have IDENTICAL labels, ranges and semantics as the lxBlob* params
 * used in IMAGE LUXSYNTH, ensuring a consistent cross-path UX.
 * They also drive the StrokeForge audio synthesis engine (via g_sp3ctra_config)
 * through PluginProcessor::applyConfigurationToCore().
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
          nodeGray ("LUXSTRAL GRAY",  juce::Colour(0xff6bb8e0), VisualizerMode::SPCTR_GRAY),
          nodeColor("LUXSTRAL COLOR", juce::Colour(0xff4ae0c8), VisualizerMode::SPCTR_COLOR),
          nodeBlob ("LUXSTRAL BLOB",  juce::Colour(0xff8888e0), VisualizerMode::SPCTR_BLOB)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector ───────────────────────────────────────────────
        initLabel(sourceLabel, "Source");
        addAndMakeVisible(sourceCombo);
        sourceCombo.addItem("S - Sampler",  1);
        sourceCombo.addItem("M - Mix",      2);
        sourceCombo.addItem("L - Live",     3);
        sourceCombo.addItem("P - LuxPitch", 4);
        sourceCombo.addItem("K - LuxMask",  5);
        sourceAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxstralSource", sourceCombo));

        // ── Negative (ToggleButton) ───────────────────────────────────────
        initLabel(negativeLabel, "Negative");
        negativeToggle.setButtonText("Active");
        addAndMakeVisible(negativeToggle);
        negativeAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxstralInversion", negativeToggle));

        // ── DC Blocking (ToggleButton) ────────────────────────────────────
        initLabel(dcBlockLabel, "DC Blocking");
        dcBlockToggle.setButtonText("Active");
        addAndMakeVisible(dcBlockToggle);
        dcBlockAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxstralAcRemoval", dcBlockToggle));

        // ── Gamma Value (Slider) ──────────────────────────────────────────
        initLabel(gammaLabel, "Gamma");
        gammaSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        gammaSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                    50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(gammaSlider);
        gammaAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxstralGammaValue", gammaSlider));

        // ── Contrast Min (Slider) ───────────────────────────────────────────
        initLabel(contrastMinLabel, "Contrast Min");
        contrastMinSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        contrastMinSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                          50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(contrastMinSlider);
        contrastMinAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxstralContrastMin", contrastMinSlider));

        // ── BLOB DETECTION — spctrBlob* params ────────────────────────────
        // Identical labels, ranges and semantics as lxBlob* (IMAGE LUXSYNTH).
        // Also drives StrokeForge audio synthesis via g_sp3ctra_config.
        // spctrBlobColorSplit is visualizer-only (no StrokeForge equivalent).

        // Row 4: Amplitude threshold — normalised CIS brightness [0..1].
        // Pixels brighter than this value are considered active strokes.
        initLabel(blobThreshLabel, "Ampl. Thr.");
        blobThreshSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobThreshSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                         50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobThreshSlider);
        blobThreshAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "spctrBlobThreshold", blobThreshSlider));

        // Row 5: Pixel threshold — minimum blob span in CIS pixels.
        // Blobs narrower than this value are discarded.
        initLabel(blobMinWidthLabel, "Pix. Thr.");
        blobMinWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMinWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                           50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMinWidthSlider);
        blobMinWidthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "spctrBlobMinWidth", blobMinWidthSlider));

        // Row 6: Merge Gap — maximum inactive-pixel gap that keeps two sub-blobs merged.
        // 0 = strict (every gap splits), higher = more tolerant merging.
        initLabel(blobMergeGapLabel, "Merge Gap");
        blobMergeGapSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMergeGapSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                            50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMergeGapSlider);
        blobMergeGapAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "spctrBlobMergeGap", blobMergeGapSlider));

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
            apvts, "spctrBlobColorSplit", blobColorSplitSlider));

        // ── Pipeline output nodes ─────────────────────────────────────────
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
        const int W   = getWidth();
        const int H   = getHeight();
        const int pad = 8;
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // ── Divider between columns ───────────────────────────────────────
        const int divX = rightX_ - pad / 2;
        g.setColour(juce::Colour(0x18ffffff));
        g.fillRect(divX, 4, 1, H - 8);

        // ── Left column section header: BLOB DETECTION ────────────────────
        const int blobSectionY = rowY(4) + Sp3ctraTheme::kControlH + 2;
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xff8888e0).withAlpha(0.55f)); // SPCTR_BLOB accent
        g.drawText("--- BLOB DETECTION ---", leftX_, blobSectionY, leftW_, 12,
                   juce::Justification::centred);

        // ── Right column: "Pipeline outputs" label above nodes ───────────
        const auto accent = juce::Colour(0xff4fa3e0);
        g.setColour(accent.withAlpha(0.6f));
        const int prelimLabelY = nodeGray.getY() - 16;
        if (prelimLabelY >= 0)
            g.drawText("Pipeline outputs (click to visualize)",
                       rightX_, prelimLabelY, rightW_, 14,
                       juce::Justification::centred);
    }

    void resized() override
    {
        const int W = getWidth();
        const int H = getHeight();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // ── Left column: all controls ─────────────────────────────────────
        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;

        auto lb = [&](int row) -> juce::Rectangle<int>
        {
            return { leftX_, rowY(row), labelW, ch };
        };
        auto cb = [&](int row) -> juce::Rectangle<int>
        {
            return { leftX_ + labelW + gap, rowY(row),
                     leftW_ - labelW - gap,  ch };
        };

        // Row 0: Source combo
        sourceLabel.setBounds(lb(0));
        sourceCombo.setBounds(cb(0));
        // Row 1: Negative toggle
        negativeLabel.setBounds(lb(1));
        negativeToggle.setBounds(cb(1).withWidth(80));
        // Row 2: DC Blocking toggle
        dcBlockLabel.setBounds(lb(2));
        dcBlockToggle.setBounds(cb(2).withWidth(80));
        // Row 3: Gamma slider
        gammaLabel.setBounds(lb(3));
        gammaSlider.setBounds(cb(3));
        // Row 4: Contrast Min slider
        contrastMinLabel.setBounds(lb(4));
        contrastMinSlider.setBounds(cb(4));
        // [blobSectionY header drawn in paint() between row 4 and 5]
        // Rows 5-8: Blob Detection (same layout as LuxSynthTabComponent rows 4-7)
        blobThreshLabel.setBounds(lb(5));      blobThreshSlider.setBounds(cb(5));
        blobMinWidthLabel.setBounds(lb(6));    blobMinWidthSlider.setBounds(cb(6));
        blobMergeGapLabel.setBounds(lb(7));    blobMergeGapSlider.setBounds(cb(7));
        blobColorSplitLabel.setBounds(lb(8));  blobColorSplitSlider.setBounds(cb(8));

        // ── Right column: 3 output nodes, vertically centred ─────────────
        constexpr int kNH = 28;   // node height
        constexpr int kNG = 6;    // gap between nodes
        constexpr int kLH = 16;   // "Pipeline outputs" label height
        const int totalH  = kLH + 3 * kNH + 2 * kNG;
        int ny = juce::jmax(4, (H - totalH) / 2);

        ny += kLH;   // space for the section label
        nodeGray.setBounds (rightX_, ny, rightW_, kNH);   ny += kNH + kNG;
        nodeColor.setBounds(rightX_, ny, rightW_, kNH);   ny += kNH + kNG;
        nodeBlob.setBounds (rightX_, ny, rightW_, kNH);
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels — image pipeline
    juce::Label sourceLabel, negativeLabel, dcBlockLabel, gammaLabel, contrastMinLabel;
    // Labels — blob detection (same names as LuxSynthTabComponent for UX consistency)
    juce::Label blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel, blobColorSplitLabel;

    // Controls — image pipeline
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Slider       gammaSlider, contrastMinSlider;
    // Controls — blob detection
    juce::Slider blobThreshSlider, blobMinWidthSlider,
                 blobMergeGapSlider, blobColorSplitSlider;

    // Attachments — image pipeline
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach,
                                                                             dcBlockAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gammaAttach,
                                                                             contrastMinAttach;
    // Attachments — blob detection (spctrBlob* APVTS params)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   blobThreshAttach,
                                                                             blobMinWidthAttach,
                                                                             blobMergeGapAttach,
                                                                             blobColorSplitAttach;

    // Pipeline output nodes
    PipelineNodeComponent nodeGray, nodeColor, nodeBlob;

    // Cached column geometry — updated by computeColumns() in paint() / resized()
    mutable int leftX_  = 0;
    mutable int leftW_  = 0;
    mutable int rightX_ = 0;
    mutable int rightW_ = 0;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** Splits the available width into left (controls) and right (nodes) columns.
     *  Left column: ~55% of width; right column: remainder minus divider gap.
     *  Identical split ratio to LuxSynthTabComponent. */
    static void computeColumns(int totalW,
                                int& lx, int& lw,
                                int& rx, int& rw) noexcept
    {
        constexpr int kPad = 8;
        constexpr int kDiv = 8;   // gap around the divider
        lx = kPad;
        lw = totalW * 55 / 100 - kPad - kDiv / 2;
        rx = lx + lw + kDiv;
        rw = totalW - rx - kPad;
    }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    /** Y coordinate of control row n (left column). */
    int rowY(int row) const noexcept
    {
        return 6 + row * (Sp3ctraTheme::kControlH + 14);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxStralTabComponent)
};
