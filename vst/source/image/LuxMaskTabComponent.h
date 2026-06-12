/**
 * @file LuxMaskTabComponent.h
 * @brief Tab — LUXMASK: MIDI-driven mobile spotlight ("synesthetic EQ") controls
 *        + pipeline output node.
 *
 * Two-column layout (mirrors LuxPitchTabComponent):
 *   Left  — LuxMask controls (enable, background, width, ADSR,
 *           width-bloom horizons, glide, position LFO, velocity)
 *   Right — pipeline output node (LUXMASK_OUTPUT visualizer)
 *
 * ── Channel model (since "Modulated/Live" refactor) ─────────────────────────
 * LuxMask no longer has a Source selector.  It now lives permanently as an
 * insert inside the Modulated channel : Live ► LuxSampler ► LuxPitch ► LuxMask.
 * When disabled or when no MIDI notes are active, it auto-bypasses to the
 * upstream signal (zero-cost pass-through).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "PipelineNodeComponent.h"
#include "VisualizerMode.h"
#include <functional>

class LuxMaskTabComponent : public juce::Component
{
public:
    std::function<void(VisualizerMode)> onNodeClicked;

    /** Accent colour used by ImagePageComponent for the LUXMASK sub-tab. */
    static constexpr uint32_t kAccentARGB = 0xff6be0d0; // teal/cyan, complement of LuxPitch pink

    explicit LuxMaskTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          nodeOutput("LUXMASK OUTPUT", juce::Colour(kAccentARGB),
                     VisualizerMode::LUXMASK_OUTPUT)
    {
        auto& apvts = p.getAPVTS();

        // ── Enable toggle ──────────────────────────────────────────────
        initLabel(enableLabel, "Enable");
        enableToggle.setButtonText("Active");
        addAndMakeVisible(enableToggle);
        enableAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxmaskEnabled", enableToggle));

        // ── Background mode ────────────────────────────────────────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Black", 1);
        bgCombo.addItem("White", 2);
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskBackgroundMode", bgCombo));

        // ── Coupling mode ──────────────────────────────────────────────
        initLabel(couplingLabel, "Step Mode");
        addAndMakeVisible(couplingCombo);
        couplingCombo.addItem("LuxStral", 1);
        couplingCombo.addItem("Free",     2);
        couplingAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskCouplingMode", couplingCombo));

        // ── Free pixels per semitone ───────────────────────────────────
        initLabel(freeStepLabel, "px/semitone");
        initSlider(freeStepSlider);
        freeStepAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskFreePixelsPerST", freeStepSlider));

        // ── Pitch Bend Range ───────────────────────────────────────────
        initLabel(pbRangeLabel, "PB Range");
        initSlider(pbRangeSlider);
        pbRangeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskPitchBendRange", pbRangeSlider));

        // ── Width (base) ───────────────────────────────────────────────
        // Sustain width — what the mask collapses to during ATTACK and what
        // it grows from during RELEASE.  The two horizons below define the
        // peak ratios at note-on and full-release.
        initLabel(widthLabel, "Width");
        initSlider(widthSlider);
        widthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskWidth", widthSlider));

        // ── ADSR (on mask alpha) ───────────────────────────────────────
        initLabel(attackLabel, "Attack");
        initSlider(attackSlider);
        attackAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskAttackMs", attackSlider));

        initLabel(decayLabel, "Decay");
        initSlider(decaySlider);
        decayAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskDecayMs", decaySlider));

        initLabel(sustainLabel, "Sustain");
        initSlider(sustainSlider);
        sustainAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskSustainLevel", sustainSlider));

        initLabel(releaseLabel, "Release");
        initSlider(releaseSlider);
        releaseAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskReleaseMs", releaseSlider));

        // ── Width horizons (absolute pixel widths, fully ADSR-coupled) ──
        initLabel(widthAttackMaxLabel, "Width @ Attack");
        initSlider(widthAttackMaxSlider);
        widthAttackMaxAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskWidthAttackPx", widthAttackMaxSlider));

        initLabel(widthReleaseMaxLabel, "Width @ Release");
        initSlider(widthReleaseMaxSlider);
        widthReleaseMaxAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskWidthReleasePx", widthReleaseMaxSlider));

        // ── Glide ──────────────────────────────────────────────────────
        initLabel(glideLabel, "Glide");
        initSlider(glideSlider);
        glideAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskGlideMs", glideSlider));

        // ── LFO position ───────────────────────────────────────────────
        initLabel(lfoPosRateLabel,  "LFO Pos Rate");
        initSlider(lfoPosRateSlider);
        lfoPosRateAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskLfoPosRate", lfoPosRateSlider));

        initLabel(lfoPosDepthLabel, "LFO Pos Depth");
        initSlider(lfoPosDepthSlider);
        lfoPosDepthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskLfoPosDepth", lfoPosDepthSlider));

        // ── Velocity coupling ──────────────────────────────────────────
        initLabel(velCouplingLabel, "Velocity");
        velCouplingToggle.setButtonText("Active");
        addAndMakeVisible(velCouplingToggle);
        velCouplingAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxmaskVelocityCoupling", velCouplingToggle));

        // ── Pipeline output node ───────────────────────────────────────
        addAndMakeVisible(nodeOutput);
        nodeOutput.onClick = [this](VisualizerMode m)
        {
            setActiveMode(m);
            if (onNodeClicked) onNodeClicked(m);
        };
    }

    void setActiveMode(VisualizerMode m)
    {
        nodeOutput.setActive (m == VisualizerMode::LUXMASK_OUTPUT);
        nodeOutput.setShowEye(m == VisualizerMode::LUXMASK_OUTPUT);
    }

    void paint(juce::Graphics& g) override
    {
        const int W = getWidth();
        const int H = getHeight();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // Divider
        g.setColour(juce::Colour(0x18ffffff));
        g.fillRect(rightX_ - 4, 4, 1, H - 8);

        const juce::Colour accent (kAccentARGB);

        // WIDTH section header (above row 5 = Width — Source row removed)
        {
            const int y = rowY(5) - 4;
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.setColour(accent.withAlpha(0.55f));
            g.drawText("--- MASK WIDTH ---", leftX_, y, leftW_, 12,
                       juce::Justification::centred);
        }
        // ADSR section header (above row 6 = Attack)
        {
            const int y = rowY(6) - 4;
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.setColour(accent.withAlpha(0.55f));
            g.drawText("--- ADSR / MODULATION ---", leftX_, y, leftW_, 12,
                       juce::Justification::centred);
        }

        // Right column label
        g.setColour(accent.withAlpha(0.6f));
        const int labelY = nodeOutput.getY() - 16;
        if (labelY >= 0)
            g.drawText("Pipeline output (click to visualize)",
                       rightX_, labelY, rightW_, 14,
                       juce::Justification::centred);
    }

    void resized() override
    {
        const int W = getWidth();
        const int H = getHeight();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;

        auto lb = [&](int row) -> juce::Rectangle<int>
        { return { leftX_, rowY(row), labelW, ch }; };
        auto cb = [&](int row) -> juce::Rectangle<int>
        { return { leftX_ + labelW + gap, rowY(row), leftW_ - labelW - gap, ch }; };

        // Row 0: Enable
        enableLabel  .setBounds(lb(0));
        enableToggle .setBounds(cb(0).withWidth(80));
        // Row 1: Background  (Source row removed)
        bgLabel      .setBounds(lb(1));
        bgCombo      .setBounds(cb(1));
        // Row 2: Step mode
        couplingLabel.setBounds(lb(2));
        couplingCombo.setBounds(cb(2));
        // Row 3: Free px/semitone
        freeStepLabel .setBounds(lb(3));
        freeStepSlider.setBounds(cb(3));
        // Row 4: PB Range
        pbRangeLabel  .setBounds(lb(4));
        pbRangeSlider .setBounds(cb(4));
        // [WIDTH section header drawn in paint at rowY(5)-4]
        // Row 5: Width (base)
        widthLabel    .setBounds(lb(5));
        widthSlider   .setBounds(cb(5));
        // [ADSR section header drawn in paint at rowY(6)-4]
        // Row 6: Attack
        attackLabel   .setBounds(lb(6));
        attackSlider  .setBounds(cb(6));
        // Row 7: Decay
        decayLabel    .setBounds(lb(7));
        decaySlider   .setBounds(cb(7));
        // Row 8: Sustain
        sustainLabel  .setBounds(lb(8));
        sustainSlider .setBounds(cb(8));
        // Row 9: Release
        releaseLabel  .setBounds(lb(9));
        releaseSlider .setBounds(cb(9));
        // Row 10: Width @ Attack
        widthAttackMaxLabel .setBounds(lb(10));
        widthAttackMaxSlider.setBounds(cb(10));
        // Row 11: Width @ Release
        widthReleaseMaxLabel .setBounds(lb(11));
        widthReleaseMaxSlider.setBounds(cb(11));
        // Row 12: Glide
        glideLabel    .setBounds(lb(12));
        glideSlider   .setBounds(cb(12));
        // Row 13: LFO Pos Rate
        lfoPosRateLabel  .setBounds(lb(13));
        lfoPosRateSlider .setBounds(cb(13));
        // Row 14: LFO Pos Depth
        lfoPosDepthLabel .setBounds(lb(14));
        lfoPosDepthSlider.setBounds(cb(14));
        // Row 15: Velocity
        velCouplingLabel .setBounds(lb(15));
        velCouplingToggle.setBounds(cb(15).withWidth(80));

        // Right column: single output node, vertically centred
        constexpr int kNH = 28;
        constexpr int kLH = 16;
        int ny = juce::jmax(4, (H - kNH - kLH) / 2) + kLH;
        nodeOutput.setBounds(rightX_, ny, rightW_, kNH);
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels (no Source label — channel routing is now automatic)
    juce::Label enableLabel, bgLabel, couplingLabel, freeStepLabel, pbRangeLabel;
    juce::Label widthLabel;
    juce::Label attackLabel, decayLabel, sustainLabel, releaseLabel;
    juce::Label widthAttackMaxLabel, widthReleaseMaxLabel;
    juce::Label glideLabel;
    juce::Label lfoPosRateLabel, lfoPosDepthLabel;
    juce::Label velCouplingLabel;

    // Controls (no source combo — channel routing is now automatic)
    juce::ToggleButton enableToggle, velCouplingToggle;
    juce::ComboBox     bgCombo, couplingCombo;
    juce::Slider       freeStepSlider, pbRangeSlider;
    juce::Slider       widthSlider;
    juce::Slider       attackSlider, decaySlider, sustainSlider, releaseSlider;
    juce::Slider       widthAttackMaxSlider, widthReleaseMaxSlider;
    juce::Slider       glideSlider;
    juce::Slider       lfoPosRateSlider, lfoPosDepthSlider;

    // Attachments (sourceAttach removed)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        enableAttach, velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        bgAttach, couplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        freeStepAttach, pbRangeAttach,
        widthAttach,
        attackAttach, decayAttach, sustainAttach, releaseAttach,
        widthAttackMaxAttach, widthReleaseMaxAttach,
        glideAttach,
        lfoPosRateAttach, lfoPosDepthAttach;

    PipelineNodeComponent nodeOutput;

    mutable int leftX_ = 0, leftW_ = 0, rightX_ = 0, rightW_ = 0;

    static void computeColumns(int totalW, int& lx, int& lw, int& rx, int& rw) noexcept
    {
        constexpr int kPad = 8, kDiv = 8;
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

    void initSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                          50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(s);
    }

    int rowY(int row) const noexcept
    { return 6 + row * (Sp3ctraTheme::kControlH + 6); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxMaskTabComponent)
};
