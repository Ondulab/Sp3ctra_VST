/**
 * @file LuxSynthTabComponent.h
 * @brief Tab 3 — LUXSYNTH: pipeline visual, source selector, toggles, output nodes.
 *
 * Pipeline:  Source → [Negative] → [DC Blocking] → Gamma →
 *            SYNTH_GRAY / SYNTH_COLOR / SYNTH_BLOB → FFT → FFT_COLOR
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
          nodeGray     ("LUXSYNTH GRAY",  juce::Colour(0xffe0a84a), VisualizerMode::SYNTH_GRAY),
          nodeColor    ("LUXSYNTH COLOR", juce::Colour(0xffe0c864), VisualizerMode::SYNTH_COLOR),
          nodeBlob     ("LUXSYNTH BLOB",  juce::Colour(0xffd07040), VisualizerMode::SYNTH_BLOB),
          nodeFftColor ("LUXSYNTH FFT",   juce::Colour(0xffcc88cc), VisualizerMode::SYNTH_FFT_COLOR)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector ───────────────────────────────────────────────
        initLabel(sourceLabel, "Source");
        addAndMakeVisible(sourceCombo);
        sourceCombo.addItem("S - Sampler", 1);
        sourceCombo.addItem("M - Mix",     2);
        sourceCombo.addItem("L - Live",     3);
        sourceCombo.addItem("P - LuxPitch", 4);
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

        // ── Contrast Min (Slider) ───────────────────────────────────────────
        initLabel(contrastMinLabel, "Contrast Min");
        contrastMinSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        contrastMinSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                          50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(contrastMinSlider);
        contrastMinAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "samplerContrastMin", contrastMinSlider));

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
        fftBinsCombo.addItem("32  - fast",    1);
        fftBinsCombo.addItem("64",            2);
        fftBinsCombo.addItem("128 - default", 3);
        fftBinsCombo.addItem("256 - quality", 4);
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
        for (auto* n : { &nodeGray, &nodeColor, &nodeBlob, &nodeFftColor })
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
        nodeFftColor.setActive (m == VisualizerMode::SYNTH_FFT_COLOR);
        nodeGray.setShowEye     (m == VisualizerMode::SYNTH_GRAY);
        nodeColor.setShowEye    (m == VisualizerMode::SYNTH_COLOR);
        nodeBlob.setShowEye     (m == VisualizerMode::SYNTH_BLOB);
        nodeFftColor.setShowEye (m == VisualizerMode::SYNTH_FFT_COLOR);
    }

    void paint(juce::Graphics& g) override
    {
        const int W   = getWidth();
        const int H   = getHeight();
        const int pad = 8;
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // ── Divider line between columns ──────────────────────────────────
        const int divX = rightX_ - pad / 2;
        g.setColour(juce::Colour(0x18ffffff));
        g.fillRect(divX, 4, 1, H - 8);

        // ── Left column section headers ───────────────────────────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));

        const int blobSectionY = rowY(4) + Sp3ctraTheme::kControlH + 2;
        g.setColour(juce::Colour(0xffd07040).withAlpha(0.55f));
        g.drawText("--- BLOB DETECTION ---", leftX_, blobSectionY, leftW_, 12,
                   juce::Justification::centred);

        const int fftSectionY = rowY(8) + Sp3ctraTheme::kControlH + 2;
        g.setColour(juce::Colour(0xffe06868).withAlpha(0.55f));
        g.drawText("--- FFT PARAMETERS ---", leftX_, fftSectionY, leftW_, 12,
                   juce::Justification::centred);

        // ── Right column node section labels ──────────────────────────────
        const auto accent = juce::Colour(0xffe08844);
        g.setColour(accent.withAlpha(0.6f));

        // "Preliminary outputs" label sits above nodeGray
        const int prelimLabelY = nodeGray.getY() - 16;
        if (prelimLabelY >= 0)
            g.drawText("Preliminary outputs",
                       rightX_, prelimLabelY, rightW_, 14,
                       juce::Justification::centred);

        // "Spectral analysis (FFT)" label sits above nodeFftColor
        const int fftLabelY = nodeFftColor.getY() - 16;
        if (fftLabelY >= 0)
            g.drawText("Spectral analysis (FFT)",
                       rightX_, fftLabelY, rightW_, 14,
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
        gammaValueLabel.setBounds(lb(3));
        gammaSlider.setBounds(cb(3));
        // Row 4: Contrast Min slider
        contrastMinLabel.setBounds(lb(4));
        contrastMinSlider.setBounds(cb(4));
        // Rows 5-8: Blob Detection
        blobThreshLabel.setBounds(lb(5));      blobThreshSlider.setBounds(cb(5));
        blobMinWidthLabel.setBounds(lb(6));    blobMinWidthSlider.setBounds(cb(6));
        blobMergeGapLabel.setBounds(lb(7));    blobMergeGapSlider.setBounds(cb(7));
        blobColorSplitLabel.setBounds(lb(8));  blobColorSplitSlider.setBounds(cb(8));
        // Rows 9-10: FFT Parameters
        fftBinsLabel.setBounds(lb(9));         fftBinsCombo.setBounds(cb(9));
        fftSmoothingLabel.setBounds(lb(10));   fftSmoothingSlider.setBounds(cb(10));

        // ── Right column: 5 output nodes, vertically distributed ──────────
        // Layout (top → bottom):
        //   [16 px: "Preliminary outputs" label – drawn in paint()]
        //   nodeGray  (28px)
        //   nodeColor (28px)
        //   nodeBlob  (28px)
        //   [16 px: "Spectral analysis (FFT)" label – drawn in paint()]
        //   nodeFftColor (28px)
        constexpr int kNH  = 28;   // node height
        constexpr int kNG  = 5;    // gap between consecutive nodes
        constexpr int kLH  = 16;   // section label height
        constexpr int kSG  = 14;   // extra gap between the two sections
        // Total content height: 2 labels + 4 nodes + 3 intra-node gaps + 1 section gap
        const int totalH = 2 * kLH + 4 * kNH + 3 * kNG + kSG;
        // Centre the block vertically in the right column
        int ny = juce::jmax(4, (H - totalH) / 2);

        ny += kLH;   // space for "Preliminary outputs" label
        nodeGray.setBounds (rightX_, ny, rightW_, kNH);   ny += kNH + kNG;
        nodeColor.setBounds(rightX_, ny, rightW_, kNH);   ny += kNH + kNG;
        nodeBlob.setBounds (rightX_, ny, rightW_, kNH);   ny += kNH + kSG;

        ny += kLH;   // space for "Spectral analysis (FFT)" label
        nodeFftColor.setBounds(rightX_, ny, rightW_, kNH);
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels — image pipeline
    juce::Label sourceLabel, negativeLabel, dcBlockLabel, gammaValueLabel, contrastMinLabel;
    // Labels — blob detection (LuxSynth-only, isolated from LuxStral)
    juce::Label blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel, blobColorSplitLabel;
    // Labels — FFT parameters
    juce::Label fftBinsLabel, fftSmoothingLabel;

    // Controls — image pipeline
    juce::ComboBox     sourceCombo;
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Slider       gammaSlider, contrastMinSlider;
    // Controls — blob detection
    juce::Slider blobThreshSlider, blobMinWidthSlider, blobMergeGapSlider, blobColorSplitSlider;
    // Controls — FFT parameters
    juce::ComboBox fftBinsCombo;
    juce::Slider   fftSmoothingSlider;

    // Attachments — image pipeline
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   negativeAttach,
                                                                             dcBlockAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gammaValueAttach,
                                                                             contrastMinAttach;
    // Attachments — blob detection
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   blobThreshAttach,
                                                                             blobMinWidthAttach,
                                                                             blobMergeGapAttach,
                                                                             blobColorSplitAttach;
    // Attachments — FFT parameters
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> fftBinsAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   fftSmoothingAttach;

    // Pipeline output nodes
    PipelineNodeComponent nodeGray, nodeColor, nodeBlob, nodeFftColor;

    // Cached column geometry — updated by computeColumns() in paint() / resized()
    mutable int leftX_  = 0;
    mutable int leftW_  = 0;
    mutable int rightX_ = 0;
    mutable int rightW_ = 0;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** Splits the available width into left (controls) and right (nodes) columns.
     *  Left column: ~55% of width; right column: remainder minus divider gap. */
    static void computeColumns(int totalW,
                                int& lx, int& lw,
                                int& rx, int& rw) noexcept
    {
        constexpr int kPad    = 8;
        constexpr int kDiv    = 8;   // gap around the divider
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthTabComponent)
};
