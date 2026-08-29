/**
 * @file LuxPitchTabComponent.h
 * @brief Tab — LUXPITCH: MIDI-driven image pitch shifting controls.
 *
 * Module page on the ModuleChrome skeleton (single column):
 *   • the graphic ADSR editor (frame + its A/D/S/R box row) at kPageTop;
 *   • the "--- MODULATION ---" caption right below it, then two label-above
 *     control rows: Glide / LFO Rate / LFO Depth, and Velocity.
 *   The background pole is chain-owned (rack header selector).
 *
 * ── Channel model (since "Modulated/Live" refactor) ─────────────────────────
 * LuxPitch no longer has a Source selector.  It now lives permanently as an
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
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "VisualizerMode.h"

class LuxPitchTabComponent : public juce::Component
{
public:
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Pitch).getARGB();   ///< inherited module colour

    /** Stacked editors' height (one ADSR editor) and the page's natural
     *  height: editors + "--- MODULATION ---" + 2 control rows. */
    static constexpr int kEditorsH   = EnvelopeEditorComponent::kPreferredH;      // 140
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 2);   // 4 + 140 + 22 + 2*36 + 8 = 246

    explicit LuxPitchTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          envelopeEditor(p.getAPVTS(), juce::Colour(kAccentARGB),
                         lpParam(0, "AttackMs"), lpParam(0, "DecayMs"),
                         lpParam(0, "SustainLevel"), lpParam(0, "ReleaseMs"),
                         lpParam(0, "AttackCurve"), lpParam(0, "DecayCurve"),
                         lpParam(0, "ReleaseCurve"))
    {
        // ── Integrated ADSR editor — owns the A/D/S/R value boxes too
        envelopeEditor.setMidiMap(&p.getMidiMap());   // right-click MIDI Learn
        addAndMakeVisible(envelopeEditor);

        // ── Enable toggle ── moved to the rack LED + zone-3 header power switch

        // ── Background ── chain-owned (rack header selector) ──

        // ── Step Mode / px-per-semitone / PB Range ── moved to PITCH SETUP ──

        // ── ADSR ── moved into the graphic editor (curve + value boxes) ──

        // ── MODULATION rows — labels are painted above the controls (paint()).
        addAndMakeVisible(glideSlider);
        addAndMakeVisible(lfoRateSlider);
        addAndMakeVisible(lfoDepthSlider);
        velCouplingToggle.setButtonText("Active");
        addAndMakeVisible(velCouplingToggle);

        setSlot(0);   // bind every attachment to bank 0 until a block is selected
    }

    /** Bind every control to the PITCH bank of `slot` (0..7) — the selected
     *  instance's parameters (same per-instance pattern as VideoScrollPage). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);
        auto& apvts = processor.getAPVTS();

        glideAttach.reset();
        lfoRateAttach.reset(); lfoDepthAttach.reset(); velCouplingAttach.reset();

        envelopeEditor.setParamIds(
            lpParam(slot_, "AttackMs"), lpParam(slot_, "DecayMs"),
            lpParam(slot_, "SustainLevel"), lpParam(slot_, "ReleaseMs"),
            lpParam(slot_, "AttackCurve"), lpParam(slot_, "DecayCurve"),
            lpParam(slot_, "ReleaseCurve"));

        glideAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lpParam(slot_, "GlideMs"), glideSlider));
        lfoRateAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lpParam(slot_, "LfoRate"), lfoRateSlider));
        lfoDepthAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, lpParam(slot_, "LfoDepth"), lfoDepthSlider));
        velCouplingAttach.reset(new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, lpParam(slot_, "VelocityCoupling"), velCouplingToggle));

        // Right-click MIDI Learn on every play control of THIS instance.
        learnAtts_.clear();
        auto& mm = processor.getMidiMap();
        auto learn = [&](juce::Component& c, const char* suffix)
        {
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                mm, c, lpParam(slot_, suffix)));
        };
        learn(glideSlider,       "GlideMs");
        learn(lfoRateSlider,     "LfoRate");
        learn(lfoDepthSlider,    "LfoDepth");
        learn(velCouplingToggle, "VelocityCoupling");
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);

        // "--- MODULATION ---" right below the last editor, then the labels
        // above each control of the two rows (ModuleChrome idiom).
        ModuleChrome::drawSectionCaption(g, envelopeEditor.getBottom(), getWidth(), accent, "MODULATION");
        ModuleChrome::drawBoxLabel(g, glideSlider,       accent, "Glide");
        ModuleChrome::drawBoxLabel(g, lfoRateSlider,     accent, "LFO Rate");
        ModuleChrome::drawBoxLabel(g, lfoDepthSlider,    accent, "LFO Depth");
        ModuleChrome::drawBoxLabel(g, velCouplingToggle, accent, "Velocity");
    }

    void resized() override
    {
        const int x = ModuleChrome::kPagePad;
        const int w = getWidth() - 2 * ModuleChrome::kPagePad;

        // Graphic envelope editor — top of the page.
        envelopeEditor.setBounds(x, ModuleChrome::kPageTop, w, envelopeEditor.preferredHeight());

        // MODULATION: caption band, then two label-above rows.
        int y = envelopeEditor.getBottom() + ModuleChrome::kSectionCaptionH;
        ModuleChrome::layoutBoxRow({ x, y, w, ModuleChrome::kBoxRowH },
                                   { &glideSlider, &lfoRateSlider, &lfoDepthSlider });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;
        ModuleChrome::layoutBoxRow({ x, y, w, ModuleChrome::kBoxRowH }, { &velCouplingToggle });
        velCouplingToggle.setSize(juce::jmin(kToggleW, velCouplingToggle.getWidth()),
                                  velCouplingToggle.getHeight());
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    static constexpr int kToggleW = 120;   ///< velocity toggle width cap (row 2)

    // Controls — ADSR sliders live inside the graphic editor; the row labels
    // are painted above the controls (ModuleChrome::drawBoxLabel).
    juce::ToggleButton velCouplingToggle;
    Sp3ctraBarSlider   glideSlider, lfoRateSlider, lfoDepthSlider;

    // Attachments — ADSR attachments removed (handled by the editor)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        velCouplingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        glideAttach, lfoRateAttach, lfoDepthAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    // Graphic ADSR editor (M5) — binds the four envelope params + per-segment
    // curves; the value boxes inside it stay in sync through the APVTS.
    EnvelopeEditorComponent envelopeEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxPitchTabComponent)
};
