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
#include "VisualizerMode.h"

class LuxStralTabComponent : public juce::Component
{
public:
    explicit LuxStralTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector — RETIRED (source follows chain placement) ──────
        // LuxStral lives on Chain 1, so it always reads the Chain 1 signal; the
        // per-engine selector is no longer shown.  The combo + attachment are
        // kept (not made visible) so the param plumbing survives for the future
        // modular-chain routing.
        sourceCombo.addItem("Chain 1", 1);
        sourceCombo.addItem("Chain 2", 2);
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
    }

    void paint(juce::Graphics& g) override
    {
        const int W = getWidth();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // ── Section header: BLOB DETECTION ────────────────────────────────
        const int blobSectionY = rowY(3) + Sp3ctraTheme::kControlH + 2;
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xff8888e0).withAlpha(0.55f)); // SPCTR_BLOB accent
        g.drawText("--- BLOB DETECTION ---", leftX_, blobSectionY, leftW_, 12,
                   juce::Justification::centred);
    }

    void resized() override
    {
        const int W = getWidth();
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

        // Source row retired — controls start at row 0 (placement defines source).
        // Row 0: Negative toggle
        negativeLabel.setBounds(lb(0));
        negativeToggle.setBounds(cb(0).withWidth(80));
        // Row 1: DC Blocking toggle
        dcBlockLabel.setBounds(lb(1));
        dcBlockToggle.setBounds(cb(1).withWidth(80));
        // Row 2: Gamma slider
        gammaLabel.setBounds(lb(2));
        gammaSlider.setBounds(cb(2));
        // Row 3: Contrast Min slider
        contrastMinLabel.setBounds(lb(3));
        contrastMinSlider.setBounds(cb(3));
        // [blobSectionY header drawn in paint() between row 3 and 4]
        // Rows 4-7: Blob Detection
        blobThreshLabel.setBounds(lb(4));      blobThreshSlider.setBounds(cb(4));
        blobMinWidthLabel.setBounds(lb(5));    blobMinWidthSlider.setBounds(cb(5));
        blobMergeGapLabel.setBounds(lb(6));    blobMergeGapSlider.setBounds(cb(6));
        blobColorSplitLabel.setBounds(lb(7));  blobColorSplitSlider.setBounds(cb(7));
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

    // Cached column geometry — updated by computeColumns() in paint() / resized()
    mutable int leftX_  = 0;
    mutable int leftW_  = 0;
    mutable int rightX_ = 0;
    mutable int rightW_ = 0;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** Controls now span the full width (pipeline-output nodes removed — the
     *  visualizer shows all outputs simultaneously). */
    static void computeColumns(int totalW,
                                int& lx, int& lw,
                                int& rx, int& rw) noexcept
    {
        constexpr int kPad = 8;
        lx = kPad;
        lw = totalW - 2 * kPad;
        rx = totalW - kPad;   // unused (no right column)
        rw = 0;
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
