/**
 * @file LuxPitchTabComponent.h
 * @brief Tab — LUXPITCH: MIDI-driven image pitch shifting controls & output node.
 *
 * Two-column layout (mirrors LuxStralTabComponent):
 *   Left  — LuxPitch controls (enable, background, ADSR, glide, LFO, velocity)
 *   Right — pipeline output node
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "PipelineNodeComponent.h"
#include "VisualizerMode.h"
#include <functional>

class LuxPitchTabComponent : public juce::Component
{
public:
    std::function<void(VisualizerMode)> onNodeClicked;

    explicit LuxPitchTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          nodeOutput("LUXPITCH OUTPUT", juce::Colour(0xffe06bb8), VisualizerMode::LUXPITCH_OUTPUT)
    {
        auto& apvts = p.getAPVTS();

        // ── Enable toggle ──────────────────────────────────────────────
        initLabel(enableLabel, "Enable");
        enableToggle.setButtonText("Active");
        addAndMakeVisible(enableToggle);
        enableAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxpitchEnabled", enableToggle));

        // ── Source selector ────────────────────────────────────────────
        initLabel(sourceLabel, "Source");
        addAndMakeVisible(sourceCombo);
        sourceCombo.addItem("S - Sampler", 1);
        sourceCombo.addItem("M - Mix",     2);
        sourceCombo.addItem("L - Live",    3);
        sourceAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchSource", sourceCombo));

        // ── Background mode ────────────────────────────────────────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Black", 1);
        bgCombo.addItem("White", 2);
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchBackgroundMode", bgCombo));

        // ── Coupling mode ──────────────────────────────────────────────
        initLabel(couplingLabel, "Step Mode");
        addAndMakeVisible(couplingCombo);
        couplingCombo.addItem("LuxStral", 1);
        couplingCombo.addItem("Free",     2);
        couplingAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchCouplingMode", couplingCombo));

        // ── Free pixels per semitone ───────────────────────────────────
        initLabel(freeStepLabel, "px/semitone");
        freeStepSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        freeStepSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                       50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(freeStepSlider);
        freeStepAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchFreePixelsPerST", freeStepSlider));

        // ── Pitch Bend Range ───────────────────────────────────────────
        initLabel(pbRangeLabel, "PB Range");
        pbRangeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        pbRangeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                      50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(pbRangeSlider);
        pbRangeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchPitchBendRange", pbRangeSlider));

        // ── ADSR ───────────────────────────────────────────────────────
        initLabel(attackLabel, "Attack");
        attackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        attackSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                     50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(attackSlider);
        attackAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchAttackMs", attackSlider));

        initLabel(decayLabel, "Decay");
        decaySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        decaySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                    50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(decaySlider);
        decayAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchDecayMs", decaySlider));

        initLabel(sustainLabel, "Sustain");
        sustainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        sustainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                      50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(sustainSlider);
        sustainAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchSustainLevel", sustainSlider));

        initLabel(releaseLabel, "Release");
        releaseSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        releaseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                      50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(releaseSlider);
        releaseAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchReleaseMs", releaseSlider));

        // ── Glide ──────────────────────────────────────────────────────
        initLabel(glideLabel, "Glide");
        glideSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        glideSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                    50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(glideSlider);
        glideAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchGlideMs", glideSlider));

        // ── LFO ────────────────────────────────────────────────────────
        initLabel(lfoRateLabel, "LFO Rate");
        lfoRateSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        lfoRateSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                      50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(lfoRateSlider);
        lfoRateAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchLfoRate", lfoRateSlider));

        initLabel(lfoDepthLabel, "LFO Depth");
        lfoDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        lfoDepthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                       50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(lfoDepthSlider);
        lfoDepthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxpitchLfoDepth", lfoDepthSlider));

        // ── Velocity coupling ──────────────────────────────────────────
        initLabel(velCouplingLabel, "Velocity");
        velCouplingToggle.setButtonText("Active");
        addAndMakeVisible(velCouplingToggle);
        velCouplingAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxpitchVelocityCoupling", velCouplingToggle));

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
        nodeOutput.setActive (m == VisualizerMode::LUXPITCH_OUTPUT);
        nodeOutput.setShowEye(m == VisualizerMode::LUXPITCH_OUTPUT);
    }

    void paint(juce::Graphics& g) override
    {
        const int W = getWidth();
        const int H = getHeight();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // Divider
        g.setColour(juce::Colour(0x18ffffff));
        g.fillRect(rightX_ - 4, 4, 1, H - 8);

        // ADSR section header
        const int adsrSectionY = rowY(6) - 4;
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffe06bb8).withAlpha(0.55f));
        g.drawText("--- ADSR / MODULATION ---", leftX_, adsrSectionY, leftW_, 12,
                   juce::Justification::centred);

        // Right column label
        const auto accent = juce::Colour(0xffe06bb8);
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
        enableLabel.setBounds(lb(0));
        enableToggle.setBounds(cb(0).withWidth(80));
        // Row 1: Source
        sourceLabel.setBounds(lb(1));
        sourceCombo.setBounds(cb(1));
        // Row 2: Background
        bgLabel.setBounds(lb(2));
        bgCombo.setBounds(cb(2));
        // Row 3: Step mode
        couplingLabel.setBounds(lb(3));
        couplingCombo.setBounds(cb(3));
        // Row 4: Free px/semitone
        freeStepLabel.setBounds(lb(4));
        freeStepSlider.setBounds(cb(4));
        // Row 5: PB Range
        pbRangeLabel.setBounds(lb(5));
        pbRangeSlider.setBounds(cb(5));
        // [ADSR section header drawn in paint at rowY(6)-4]
        // Row 6: Attack
        attackLabel.setBounds(lb(6));
        attackSlider.setBounds(cb(6));
        // Row 7: Decay
        decayLabel.setBounds(lb(7));
        decaySlider.setBounds(cb(7));
        // Row 8: Sustain
        sustainLabel.setBounds(lb(8));
        sustainSlider.setBounds(cb(8));
        // Row 9: Release
        releaseLabel.setBounds(lb(9));
        releaseSlider.setBounds(cb(9));
        // Row 10: Glide
        glideLabel.setBounds(lb(10));
        glideSlider.setBounds(cb(10));
        // Row 11: LFO Rate
        lfoRateLabel.setBounds(lb(11));
        lfoRateSlider.setBounds(cb(11));
        // Row 12: LFO Depth
        lfoDepthLabel.setBounds(lb(12));
        lfoDepthSlider.setBounds(cb(12));
        // Row 13: Velocity
        velCouplingLabel.setBounds(lb(13));
        velCouplingToggle.setBounds(cb(13).withWidth(80));

        // Right column: single output node, vertically centred
        constexpr int kNH = 28;
        constexpr int kLH = 16;
        int ny = juce::jmax(4, (H - kNH - kLH) / 2) + kLH;
        nodeOutput.setBounds(rightX_, ny, rightW_, kNH);
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels
    juce::Label enableLabel, sourceLabel, bgLabel, couplingLabel, freeStepLabel, pbRangeLabel;
    juce::Label attackLabel, decayLabel, sustainLabel, releaseLabel;
    juce::Label glideLabel, lfoRateLabel, lfoDepthLabel, velCouplingLabel;

    // Controls
    juce::ToggleButton enableToggle, velCouplingToggle;
    juce::ComboBox     sourceCombo, bgCombo, couplingCombo;
    juce::Slider       freeStepSlider, pbRangeSlider;
    juce::Slider       attackSlider, decaySlider, sustainSlider, releaseSlider;
    juce::Slider       glideSlider, lfoRateSlider, lfoDepthSlider;

    // Attachments
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        enableAttach, velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        sourceAttach, bgAttach, couplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        freeStepAttach, pbRangeAttach,
        attackAttach, decayAttach, sustainAttach, releaseAttach,
        glideAttach, lfoRateAttach, lfoDepthAttach;

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

    int rowY(int row) const noexcept
    { return 6 + row * (Sp3ctraTheme::kControlH + 6); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxPitchTabComponent)
};
