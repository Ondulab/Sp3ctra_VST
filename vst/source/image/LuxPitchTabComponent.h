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
                         "luxpitchSustainLevel", "luxpitchReleaseMs",
                         "luxpitchAttackCurve", "luxpitchDecayCurve",
                         "luxpitchReleaseCurve")
    {
        auto& apvts = p.getAPVTS();

        // ── Integrated ADSR editor — owns the A/D/S/R value boxes too
        addAndMakeVisible(envelopeEditor);

        // ── Enable toggle ── moved to the rack LED + zone-3 header power switch

        // ── Background mode ────────────────────────────────────────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Black", 1);
        bgCombo.addItem("White", 2);
        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchBackgroundMode", bgCombo));

        // ── Step Mode / px-per-semitone / PB Range ── moved to PITCH SETUP ──

        // ── ADSR ── moved into the graphic editor (curve + value boxes) ──

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

        // MODULATION section header (above the first control row below the editor)
        const int sectionY = rowY(1) - 4;
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffe06bb8).withAlpha(0.55f));
        g.drawText("--- MODULATION ---", leftX_, sectionY, leftW_, 12,
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
                                 envelopeEditor.preferredHeight());

        // Row 0: Background  (Enable row removed — power lives in rack/header)
        bgLabel.setBounds(lb(0));
        bgCombo.setBounds(cb(0));
        // [Step mode / px-per-semitone / PB Range moved to PITCH SETUP]
        // [MODULATION section header drawn in paint at rowY(1)-4]
        // Row 1: Glide
        glideLabel.setBounds(lb(1));
        glideSlider.setBounds(cb(1));
        // Row 2: LFO Rate
        lfoRateLabel.setBounds(lb(2));
        lfoRateSlider.setBounds(cb(2));
        // Row 3: LFO Depth
        lfoDepthLabel.setBounds(lb(3));
        lfoDepthSlider.setBounds(cb(3));
        // Row 4: Velocity
        velCouplingLabel.setBounds(lb(4));
        velCouplingToggle.setBounds(cb(4).withWidth(80));
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // Labels — ADSR labels removed (now inside the graphic editor);
    //          Step Mode / px-per-semitone / PB Range moved to PITCH SETUP.
    juce::Label bgLabel;
    juce::Label glideLabel, lfoRateLabel, lfoDepthLabel, velCouplingLabel;

    // Controls — ADSR sliders removed (now inside the graphic editor)
    juce::ToggleButton velCouplingToggle;
    juce::ComboBox     bgCombo;
    juce::Slider       glideSlider, lfoRateSlider, lfoDepthSlider;

    // Attachments — ADSR attachments removed (handled by the editor)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        bgAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
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
