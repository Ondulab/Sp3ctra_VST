/**
 * @file LuxPitchTabComponent.h
 * @brief Tab — LUXPITCH: MIDI-driven image pitch shifting controls & output node.
 *
 * Two-column layout (mirrors LuxStralTabComponent):
 *   Left  — LuxPitch controls (enable, background, ADSR, glide, LFO, velocity)
 *   Right — pipeline output node
 *
 * ── Channel model (since "Modulated/Live" refactor) ─────────────────────────
 * LuxPitch no longer has a Source selector.  It now lives permanently as an
 * insert inside the Modulated channel : Live ► [LuxPitch ⇄ LuxMask] ► LuxSampler
 * (insert order set by the chainInsertOrder parameter; the sampler records
 * the post-insert frame and its playback bypasses the inserts).
 * When disabled or when no MIDI notes are active, it auto-bypasses to the
 * upstream signal (zero-cost pass-through).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/EnvelopeEditorComponent.h"
#include "VisualizerMode.h"

class LuxPitchTabComponent : public juce::Component
{
public:
    explicit LuxPitchTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          envelopeEditor(p.getAPVTS(), juce::Colour(0xffe06bb8),
                         "luxpitchAttackMs", "luxpitchDecayMs",
                         "luxpitchSustainLevel", "luxpitchReleaseMs")
    {
        auto& apvts = p.getAPVTS();

        // ── Graphic ADSR editor (M5) — sliders below stay as numeric fallback
        addAndMakeVisible(envelopeEditor);

        // ── Enable toggle ──────────────────────────────────────────────
        initLabel(enableLabel, "Enable");
        enableToggle.setButtonText("Active");
        addAndMakeVisible(enableToggle);
        enableAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxpitchEnabled", enableToggle));

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
    }

    void paint(juce::Graphics& g) override
    {
        const int W = getWidth();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        // ADSR section header (now at row 5 — Source row removed)
        const int adsrSectionY = rowY(5) - 4;
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffe06bb8).withAlpha(0.55f));
        g.drawText("--- ADSR / MODULATION ---", leftX_, adsrSectionY, leftW_, 12,
                   juce::Justification::centred);
    }

    void resized() override
    {
        const int W = getWidth();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        const int labelW = 80;
        const int gap    = Sp3ctraTheme::kGap;
        const int ch     = Sp3ctraTheme::kControlH;

        auto lb = [&](int row) -> juce::Rectangle<int>
        { return { leftX_, rowY(row), labelW, ch }; };
        auto cb = [&](int row) -> juce::Rectangle<int>
        { return { leftX_ + labelW + gap, rowY(row), leftW_ - labelW - gap, ch }; };

        // Graphic envelope editor — top of the left column (rows shifted down
        // by kEnvHeaderH, see rowY()).
        envelopeEditor.setBounds(leftX_, 4, leftW_,
                                 EnvelopeEditorComponent::kPreferredH);

        // Row 0: Enable
        enableLabel.setBounds(lb(0));
        enableToggle.setBounds(cb(0).withWidth(80));
        // Row 1: Background  (Source row removed — channel routing is automatic)
        bgLabel.setBounds(lb(1));
        bgCombo.setBounds(cb(1));
        // Row 2: Step mode
        couplingLabel.setBounds(lb(2));
        couplingCombo.setBounds(cb(2));
        // Row 3: Free px/semitone
        freeStepLabel.setBounds(lb(3));
        freeStepSlider.setBounds(cb(3));
        // Row 4: PB Range
        pbRangeLabel.setBounds(lb(4));
        pbRangeSlider.setBounds(cb(4));
        // [ADSR section header drawn in paint at rowY(5)-4]
        // Row 5: Attack
        attackLabel.setBounds(lb(5));
        attackSlider.setBounds(cb(5));
        // Row 6: Decay
        decayLabel.setBounds(lb(6));
        decaySlider.setBounds(cb(6));
        // Row 7: Sustain
        sustainLabel.setBounds(lb(7));
        sustainSlider.setBounds(cb(7));
        // Row 8: Release
        releaseLabel.setBounds(lb(8));
        releaseSlider.setBounds(cb(8));
        // Row 9: Glide
        glideLabel.setBounds(lb(9));
        glideSlider.setBounds(cb(9));
        // Row 10: LFO Rate
        lfoRateLabel.setBounds(lb(10));
        lfoRateSlider.setBounds(cb(10));
        // Row 11: LFO Depth
        lfoDepthLabel.setBounds(lb(11));
        lfoDepthSlider.setBounds(cb(11));
        // Row 12: Velocity
        velCouplingLabel.setBounds(lb(12));
        velCouplingToggle.setBounds(cb(12).withWidth(80));
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels (no Source label — channel routing is now automatic)
    juce::Label enableLabel, bgLabel, couplingLabel, freeStepLabel, pbRangeLabel;
    juce::Label attackLabel, decayLabel, sustainLabel, releaseLabel;
    juce::Label glideLabel, lfoRateLabel, lfoDepthLabel, velCouplingLabel;

    // Controls (no source combo — channel routing is now automatic)
    juce::ToggleButton enableToggle, velCouplingToggle;
    juce::ComboBox     bgCombo, couplingCombo;
    juce::Slider       freeStepSlider, pbRangeSlider;
    juce::Slider       attackSlider, decaySlider, sustainSlider, releaseSlider;
    juce::Slider       glideSlider, lfoRateSlider, lfoDepthSlider;

    // Attachments (sourceAttach removed)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        enableAttach, velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        bgAttach, couplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        freeStepAttach, pbRangeAttach,
        attackAttach, decayAttach, sustainAttach, releaseAttach,
        glideAttach, lfoRateAttach, lfoDepthAttach;

    // Graphic ADSR editor (M5) — binds the same four envelope params as the
    // slider rows; both stay in sync through the APVTS.
    EnvelopeEditorComponent envelopeEditor;

    mutable int leftX_ = 0, leftW_ = 0, rightX_ = 0, rightW_ = 0;

    // Controls span the full width (pipeline-output node removed).
    static void computeColumns(int totalW, int& lx, int& lw, int& rx, int& rw) noexcept
    {
        constexpr int kPad = 8;
        lx = kPad;
        lw = totalW - 2 * kPad;
        rx = totalW - kPad;   // unused
        rw = 0;
    }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    /** Vertical offset reserved for the graphic envelope editor (96 px + gap). */
    static constexpr int kEnvHeaderH = EnvelopeEditorComponent::kPreferredH + 4;

    int rowY(int row) const noexcept
    { return kEnvHeaderH + 6 + row * (Sp3ctraTheme::kControlH + 6); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxPitchTabComponent)
};
