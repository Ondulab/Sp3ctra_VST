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
#include "../midi/MidiLearnAttachment.h"
#include "../ui/EnvelopeEditorComponent.h"
#include "../ui/MaskFilterEditorComponent.h"
#include "VisualizerMode.h"

class LuxMaskTabComponent : public juce::Component
{
public:
    /** Accent colour for the LUXMASK page. */
    static constexpr uint32_t kAccentARGB = 0xff6be0d0; // teal/cyan, complement of LuxPitch pink

    explicit LuxMaskTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          // Alpha-only editor: the ADSR now drives the filter cutoff, so the
          // separate width lane is gone (no width param IDs passed).
          envelopeEditor(p.getAPVTS(), juce::Colour(kAccentARGB),
                         lmParam(0, "AttackMs"), lmParam(0, "DecayMs"),
                         lmParam(0, "SustainLevel"), lmParam(0, "ReleaseMs"),
                         lmParam(0, "AttackCurve"), lmParam(0, "DecayCurve"),
                         lmParam(0, "ReleaseCurve")),
          filterEditor(p.getAPVTS(), juce::Colour(kAccentARGB),
                       lmParam(0, "FilterWidth"), lmParam(0, "FilterOffset"),
                       lmParam(0, "FilterSlope"))
    {
        // ── Integrated ADSR editor — owns the value boxes too
        envelopeEditor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(envelopeEditor);

        // ── Interactive filter editor (Width / Bias / Slope + live fill) ─
        filterEditor.setMidiMap(&p.getMidiMap());
        addAndMakeVisible(filterEditor);

        // ── Enable toggle ── moved to the rack LED + zone-3 header power switch

        // ── Background mode ────────────────────────────────────────────
        initLabel(bgLabel, "Background");
        addAndMakeVisible(bgCombo);
        bgCombo.addItem("Black", 1);
        bgCombo.addItem("White", 2);

        // ── Step Mode / px-per-semitone / PB Range ── moved to MASK SETUP ──

        // ── ADSR + Filter ── both moved into graphic editors (curve/handles
        //    + value boxes).  The ADSR output drives the filter openness.

        // ── Glide ──────────────────────────────────────────────────────
        initLabel(glideLabel, "Glide");
        initSlider(glideSlider);

        // ── LFO position ───────────────────────────────────────────────
        initLabel(lfoPosRateLabel,  "LFO Pos Rate");
        initSlider(lfoPosRateSlider);

        initLabel(lfoPosDepthLabel, "LFO Pos Depth");
        initSlider(lfoPosDepthSlider);

        // ── Velocity coupling ──────────────────────────────────────────
        initLabel(velCouplingLabel, "Velocity");
        velCouplingToggle.setButtonText("Active");
        addAndMakeVisible(velCouplingToggle);

        setSlot(0);   // bind every attachment to bank 0 until a block is selected
    }

    /** Bind every control to the MASK bank of `slot` (0..7) — the selected
     *  instance's parameters (same per-instance pattern as VideoScrollPage). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        auto& apvts = processor.getAPVTS();

        bgAttach.reset(); glideAttach.reset();
        lfoPosRateAttach.reset(); lfoPosDepthAttach.reset(); velCouplingAttach.reset();

        envelopeEditor.setParamIds(
            lmParam(slot_, "AttackMs"), lmParam(slot_, "DecayMs"),
            lmParam(slot_, "SustainLevel"), lmParam(slot_, "ReleaseMs"),
            lmParam(slot_, "AttackCurve"), lmParam(slot_, "DecayCurve"),
            lmParam(slot_, "ReleaseCurve"));
        filterEditor.setInstance(slot_,
            lmParam(slot_, "FilterWidth"), lmParam(slot_, "FilterOffset"),
            lmParam(slot_, "FilterSlope"));

        bgAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, lmParam(slot_, "BackgroundMode"), bgCombo));
        glideAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lmParam(slot_, "GlideMs"), glideSlider));
        lfoPosRateAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lmParam(slot_, "LfoPosRate"), lfoPosRateSlider));
        lfoPosDepthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lmParam(slot_, "LfoPosDepth"), lfoPosDepthSlider));
        velCouplingAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, lmParam(slot_, "VelocityCoupling"), velCouplingToggle));

        // Right-click MIDI Learn on every play control of THIS instance.
        learnAtts_.clear();
        auto& mm = processor.getMidiMap();
        auto learn = [&](juce::Component& c, const char* suffix)
        {
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                mm, c, lmParam(slot_, suffix)));
        };
        learn(glideSlider,       "GlideMs");
        learn(lfoPosRateSlider,  "LfoPosRate");
        learn(lfoPosDepthSlider, "LfoPosDepth");
        learn(velCouplingToggle, "VelocityCoupling");
        learn(bgCombo,           "BackgroundMode");
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const int W = getWidth();
        computeColumns(W, leftX_, leftW_, rightX_, rightW_);

        const juce::Colour accent (kAccentARGB);

        auto sectionHeader = [&](int row, const char* text)
        {
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.setColour(accent.withAlpha(0.55f));
            g.drawText(text, leftX_, rowY(row) + 2, leftW_, 12,
                       juce::Justification::centred);
        };

        // Filter lives in its own graphic editor now; only MODULATION remains.
        sectionHeader(1, "--- MODULATION ---");
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
        // by kEnvHeaderH, see rowY()).  The live filter-response strip sits
        // right below it, mirroring the old ADSR + width two-lane layout.
        envelopeEditor.setBounds(leftX_, 4, leftW_,
                                 envelopeEditor.preferredHeight());
        filterEditor.setBounds(leftX_, 4 + envelopeEditor.preferredHeight() + 4,
                               leftW_, filterEditor.preferredHeight());

        // Row 0: Background  (Enable row removed — power lives in rack/header)
        bgLabel      .setBounds(lb(0));
        bgCombo      .setBounds(cb(0));
        // [Step mode / px-per-semitone / PB Range moved to MASK SETUP]
        // [MODULATION section header drawn in paint at row 1]
        // Row 2: Glide
        glideLabel    .setBounds(lb(2));
        glideSlider   .setBounds(cb(2));
        // Row 3: LFO Pos Rate
        lfoPosRateLabel  .setBounds(lb(3));
        lfoPosRateSlider .setBounds(cb(3));
        // Row 4: LFO Pos Depth
        lfoPosDepthLabel .setBounds(lb(4));
        lfoPosDepthSlider.setBounds(cb(4));
        // Row 5: Velocity
        velCouplingLabel .setBounds(lb(5));
        velCouplingToggle.setBounds(cb(5).withWidth(80));
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    // Labels — ADSR labels removed (now inside the graphic editor);
    //          Step Mode / px-per-semitone / PB Range moved to MASK SETUP.
    juce::Label bgLabel;
    juce::Label glideLabel;
    juce::Label lfoPosRateLabel, lfoPosDepthLabel;
    juce::Label velCouplingLabel;

    // Controls — ADSR + filter controls are inside the graphic editors.
    juce::ToggleButton velCouplingToggle;
    juce::ComboBox     bgCombo;
    juce::Slider       glideSlider;
    juce::Slider       lfoPosRateSlider, lfoPosDepthSlider;

    // Attachments — ADSR + filter attachments handled by the editors.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        bgAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        glideAttach,
        lfoPosRateAttach, lfoPosDepthAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    // Graphic ADSR editor — binds the four envelope params + per-segment curves.
    EnvelopeEditorComponent envelopeEditor;

    // Interactive filter editor (Width / Bias / Slope) — sits below the ADSR.
    MaskFilterEditorComponent filterEditor;

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

    void initSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                          50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(s);
    }

    /** Vertical offset reserved for the two graphic editors (ADSR + filter).
     *  The ADSR editor is alpha-only (it drives the filter openness); the
     *  filter shape is edited in the interactive strip below it. */
    static constexpr int kEnvHeaderH =
        EnvelopeEditorComponent::kPreferredH + 4
            + MaskFilterEditorComponent::kPreferredH + 4;

    int rowY(int row) const noexcept
    { return kEnvHeaderH + 6 + row * (Sp3ctraTheme::kControlH + 6); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxMaskTabComponent)
};
