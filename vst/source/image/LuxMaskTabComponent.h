/**
 * @file LuxMaskTabComponent.h
 * @brief Tab — LUXMASK: MIDI-driven mobile spotlight ("synesthetic EQ") controls.
 *
 * Module page on the ModuleChrome skeleton (single column, mirrors
 * LuxPitchTabComponent):
 *   • the graphic ADSR editor (frame + its A/D/S/R box row) at kPageTop,
 *     then the FILTER editor (frame + Width / Offset / Slope box row);
 *   • the "--- MODULATION ---" caption right below, then two label-above
 *     control rows: Glide / LFO Pos Rate / LFO Pos Depth, and Velocity.
 *   The background pole is chain-owned (rack header selector).
 *
 * ── Channel model (since "Modulated/Live" refactor) ─────────────────────────
 * LuxMask no longer has a Source selector.  It now lives permanently as an
 * insert inside the Modulated channel : Live ► [LuxPitch ⇄ LuxMask] ► LuxSampler
 * (insert order = the module's position in its chain; the sampler records
 * the post-insert frame and its playback bypasses the inserts).
 * When disabled or when no MIDI notes are active, it auto-bypasses to the
 * upstream signal (zero-cost pass-through).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../ui/EnvelopeEditorComponent.h"
#include "../ui/MaskFilterEditorComponent.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "VisualizerMode.h"

class LuxMaskTabComponent : public juce::Component
{
public:
    /** Accent colour for the LUXMASK page. */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Mask).getARGB();   ///< inherited module colour

    /** Stacked editors' height (ADSR + gap + FILTER) and the page's natural
     *  height: editors + "--- MODULATION ---" + 2 control rows. */
    static constexpr int kEditorsH   = EnvelopeEditorComponent::kPreferredH
                                     + ModuleChrome::kEditorGap
                                     + MaskFilterEditorComponent::kPreferredH;    // 140 + 4 + 108 = 252
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 2);   // 4 + 252 + 22 + 2*36 + 8 = 358

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

        // ── Background ── chain-owned (rack header selector) ──

        // ── Step Mode / px-per-semitone / PB Range ── moved to MASK SETUP ──

        // ── ADSR + Filter ── both moved into graphic editors (curve/handles
        //    + value boxes).  The ADSR output drives the filter openness.

        // ── MODULATION rows — labels are painted above the controls (paint()).
        initSlider(glideSlider);
        initSlider(lfoPosRateSlider);
        initSlider(lfoPosDepthSlider);
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

        glideAttach.reset();
        lfoPosRateAttach.reset(); lfoPosDepthAttach.reset(); velCouplingAttach.reset();

        envelopeEditor.setParamIds(
            lmParam(slot_, "AttackMs"), lmParam(slot_, "DecayMs"),
            lmParam(slot_, "SustainLevel"), lmParam(slot_, "ReleaseMs"),
            lmParam(slot_, "AttackCurve"), lmParam(slot_, "DecayCurve"),
            lmParam(slot_, "ReleaseCurve"));
        filterEditor.setInstance(slot_,
            lmParam(slot_, "FilterWidth"), lmParam(slot_, "FilterOffset"),
            lmParam(slot_, "FilterSlope"));

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
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);

        // "--- MODULATION ---" right below the last editor (the FILTER strip),
        // then the labels above each control of the two rows (ModuleChrome).
        ModuleChrome::drawSectionCaption(g, filterEditor.getBottom(), getWidth(), accent, "MODULATION");
        ModuleChrome::drawBoxLabel(g, glideSlider,       accent, "Glide");
        ModuleChrome::drawBoxLabel(g, lfoPosRateSlider,  accent, "LFO Pos Rate");
        ModuleChrome::drawBoxLabel(g, lfoPosDepthSlider, accent, "LFO Pos Depth");
        ModuleChrome::drawBoxLabel(g, velCouplingToggle, accent, "Velocity");
    }

    void resized() override
    {
        const int x = ModuleChrome::kPagePad;
        const int w = getWidth() - 2 * ModuleChrome::kPagePad;

        // Graphic envelope editor at the top, the live filter-response strip
        // right below it (kEditorGap apart).
        envelopeEditor.setBounds(x, ModuleChrome::kPageTop, w, envelopeEditor.preferredHeight());
        filterEditor.setBounds(x, envelopeEditor.getBottom() + ModuleChrome::kEditorGap,
                               w, filterEditor.preferredHeight());

        // MODULATION: caption band, then two label-above rows.
        int y = filterEditor.getBottom() + ModuleChrome::kSectionCaptionH;
        ModuleChrome::layoutBoxRow({ x, y, w, ModuleChrome::kBoxRowH },
                                   { &glideSlider, &lfoPosRateSlider, &lfoPosDepthSlider });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;
        ModuleChrome::layoutBoxRow({ x, y, w, ModuleChrome::kBoxRowH }, { &velCouplingToggle });
        velCouplingToggle.setSize(juce::jmin(kToggleW, velCouplingToggle.getWidth()),
                                  velCouplingToggle.getHeight());
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    static constexpr int kToggleW = 120;   ///< velocity toggle width cap (row 2)

    // Controls — ADSR + filter controls are inside the graphic editors; the
    // row labels are painted above the controls (ModuleChrome::drawBoxLabel).
    juce::ToggleButton velCouplingToggle;
    Sp3ctraBarSlider   glideSlider;
    Sp3ctraBarSlider   lfoPosRateSlider, lfoPosDepthSlider;

    // Attachments — ADSR + filter attachments handled by the editors.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        glideAttach,
        lfoPosRateAttach, lfoPosDepthAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    // Graphic ADSR editor — binds the four envelope params + per-segment curves.
    EnvelopeEditorComponent envelopeEditor;

    // Interactive filter editor (Width / Bias / Slope) — sits below the ADSR.
    MaskFilterEditorComponent filterEditor;

    void initSlider(Sp3ctraBarSlider& s)
    {
        addAndMakeVisible(s);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxMaskTabComponent)
};
