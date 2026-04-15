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
 * Blob detection controls are wired to StrokeForge APVTS parameters
 * (sfBlobBaseThreshold / sfBlobMinWidth / sfBlobMergeGap) which are also
 * read by the StrokeForge audio synthesis engine.
 * Image analysis (SPCTR_BLOB renderer) and audio synthesis share the same
 * parameters deliberately: tuning one tunes both.
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
        sourceCombo.addItem("S - Sampler", 1);
        sourceCombo.addItem("M - Mix",     2);
        sourceCombo.addItem("L - Live",    3);
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

        // ── BLOB DETECTION — StrokeForge params ───────────────────────────
        // These parameters are shared between:
        //   1. Image analysis: detectSpctrBlobs() reads g_sp3ctra_config.strokeforge_blob_*
        //      (updated from APVTS by PluginProcessor::parameterChanged)
        //   2. Audio synthesis: StrokeForge engine reads the same config fields
        // Tuning here affects both the visualizer and the synthesis simultaneously.

        // Row 4: Amplitude threshold — normalised brightness [0..0.2].
        // Pixels brighter than this value are considered active strokes.
        initLabel(blobThreshLabel, "Ampl. Thr.");
        blobThreshSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobThreshSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                         50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobThreshSlider);
        blobThreshAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "sfBlobBaseThreshold", blobThreshSlider));

        // Row 5: Pixel threshold — minimum blob span in CIS pixels.
        // Blobs narrower than this value are discarded.
        initLabel(blobMinWidthLabel, "Pix. Thr.");
        blobMinWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMinWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                           50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMinWidthSlider);
        blobMinWidthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "sfBlobMinWidth", blobMinWidthSlider));

        // Row 6: Merge Gap — maximum inactive-pixel gap that keeps two sub-blobs merged.
        // 0 = strict (every gap splits), higher = more tolerant merging.
        initLabel(blobMergeGapLabel, "Merge Gap");
        blobMergeGapSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        blobMergeGapSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                            50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(blobMergeGapSlider);
        blobMergeGapAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "sfBlobMergeGap", blobMergeGapSlider));

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
        const int blobSectionY = rowY(3) + Sp3ctraTheme::kControlH + 2;
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
        // [blobSectionY header drawn in paint() between row 3 and 4]
        // Rows 4-6: Blob Detection
        blobThreshLabel.setBounds(lb(4));     blobThreshSlider.setBounds(cb(4));
        blobMinWidthLabel.setBounds(lb(5));   blobMinWidthSlider.setBounds(cb(5));
        blobMergeGapLabel.setBounds(lb(6));   blobMergeGapSlider.setBounds(cb(6));

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
    juce::Label sourceLabel, negativeLabel, dcBlockLabel, gammaLabel;
    // Labels — blob detection
    juce::Label blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel;

    // Controls — image pipeline
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Slider       gammaSlider;
    // Controls — blob detection
    juce::Slider blobThreshSlider, blobMinWidthSlider, blobMergeGapSlider;

    // Attachments — image pipeline
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach,
                                                                             dcBlockAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gammaAttach;
    // Attachments — blob detection (StrokeForge APVTS params)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   blobThreshAttach,
                                                                             blobMinWidthAttach,
                                                                             blobMergeGapAttach;

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
