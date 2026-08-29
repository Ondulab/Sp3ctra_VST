/**
 * @file LuxHarmoTabComponent.h
 * @brief Tab — SCALE: musical quantizer on the image-line stream.
 *
 * Standard module page (ModuleEditorChrome): the interactive scale-grid view
 * on top (HarmoEditorComponent — the comb over the frequency axis, click =
 * root), the "--- SCALE ---" section caption, then two label-above control
 * rows: the Root / Scale / Mode combos and the Strength / Width / Slope /
 * Glide bars. The background pole is chain-owned (rack header selector), not
 * a module setting.
 *
 * Power lives in the zone-3 header switch + the rack LED.
 * Per-instance: setSlot(slot) rebinds every control to the luxharmo{slot}_*
 * bank of the selected instance (same pattern as LuxReverb/LuxEcho/LuxEq).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/HarmoEditorComponent.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"

class LuxHarmoTabComponent : public juce::Component
{
public:
    /** Accent colour for the SCALE page (matches the catalogue chip). */
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Harmonize).getARGB();   ///< inherited module colour

    static constexpr int kEditorsH   = HarmoEditorComponent::kPreferredH;
    /** Editor + section caption + 2 control rows (combos, bars). */
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 2);

    explicit LuxHarmoTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          editor(p.getAPVTS(), juce::Colour(kAccentARGB))
    {
        // ── Interactive scale-grid view (click a key = root) ───────────
        editor.setMidiMap(&p.getMidiMap());   // right-click canvas → learn Root
        addAndMakeVisible(editor);

        // ── Musical grid: Root / Scale / Mode (labels painted above) ───
        addAndMakeVisible(rootCombo);
        for (int i = 0; i < 12; ++i)
            rootCombo.addItem(juce::StringArray{"C", "C#", "D", "D#", "E", "F",
                                                "F#", "G", "G#", "A", "A#", "B"}[i],
                              i + 1);

        addAndMakeVisible(scaleCombo);
        {
            const juce::StringArray scales{"Chromatic", "Major", "Minor",
                                           "Harm Minor", "Penta Maj", "Penta Min",
                                           "Blues", "Whole Tone", "Dorian",
                                           "Phrygian", "Lydian", "Mixolydian",
                                           "Fifths", "Octaves"};
            for (int i = 0; i < scales.size(); ++i)
                scaleCombo.addItem(scales[i], i + 1);
        }

        addAndMakeVisible(modeCombo);
        modeCombo.addItem("Mask", 1);   // comb: off-grid material fades out
        modeCombo.addItem("Warp", 2);   // reassign: material slides to the grid

        // ── Morph + comb shape + chord-change glide (lime bars) ────────
        for (auto* s : { &strengthSlider, &widthSlider, &slopeSlider, &glideSlider })
            addAndMakeVisible(*s);

        setSlot(0);   // bind to bank 0 until a block is selected
    }

    /** Bind every control to the SCALE bank of `slot` (0..7). */
    void setSlot(int slot)
    {
        slot_ = juce::jlimit(0, 7, slot);

        // Reset-first, then rebind (JUCE attachment rebind contract — a live
        // attachment must never see the new param through the old binding).
        rootAttach.reset();  scaleAttach.reset(); modeAttach.reset();
        strengthAttach.reset(); widthAttach.reset();
        slopeAttach.reset(); glideAttach.reset();

        editor.setInstance(slot_,
                           hmParam(slot_, "Root"),  hmParam(slot_, "Scale"),
                           hmParam(slot_, "Width"), hmParam(slot_, "Strength"));

        auto& ap = processor.getAPVTS();
        using CB = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using SL = juce::AudioProcessorValueTreeState::SliderAttachment;
        rootAttach    .reset(new CB(ap, hmParam(slot_, "Root"),     rootCombo));
        scaleAttach   .reset(new CB(ap, hmParam(slot_, "Scale"),    scaleCombo));
        modeAttach    .reset(new CB(ap, hmParam(slot_, "Mode"),     modeCombo));
        strengthAttach.reset(new SL(ap, hmParam(slot_, "Strength"), strengthSlider));
        widthAttach   .reset(new SL(ap, hmParam(slot_, "Width"),    widthSlider));
        slopeAttach   .reset(new SL(ap, hmParam(slot_, "Slope"),    slopeSlider));
        glideAttach   .reset(new SL(ap, hmParam(slot_, "Glide"),    glideSlider));

        auto& mm = processor.getMidiMap();
        scaleLearn_    = std::make_unique<MidiLearnAttachment>(mm, scaleCombo,     hmParam(slot_, "Scale"));
        modeLearn_     = std::make_unique<MidiLearnAttachment>(mm, modeCombo,      hmParam(slot_, "Mode"));
        strengthLearn_ = std::make_unique<MidiLearnAttachment>(mm, strengthSlider, hmParam(slot_, "Strength"));
        widthLearn_    = std::make_unique<MidiLearnAttachment>(mm, widthSlider,    hmParam(slot_, "Width"));
        slopeLearn_    = std::make_unique<MidiLearnAttachment>(mm, slopeSlider,    hmParam(slot_, "Slope"));
        glideLearn_    = std::make_unique<MidiLearnAttachment>(mm, glideSlider,    hmParam(slot_, "Glide"));
    }

    int slot() const noexcept { return slot_; }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent (kAccentARGB);
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH,
                                         getWidth(), accent, "SCALE");
        // Label-above idiom for every control row (combos and bars alike).
        ModuleChrome::drawBoxLabel(g, rootCombo,      accent, "Root");
        ModuleChrome::drawBoxLabel(g, scaleCombo,     accent, "Scale");
        ModuleChrome::drawBoxLabel(g, modeCombo,      accent, "Mode");
        ModuleChrome::drawBoxLabel(g, strengthSlider, accent, "Strength");
        ModuleChrome::drawBoxLabel(g, widthSlider,    accent, "Width");
        ModuleChrome::drawBoxLabel(g, slopeSlider,    accent, "Slope");
        ModuleChrome::drawBoxLabel(g, glideSlider,    accent, "Glide");
    }

    void resized() override
    {
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth() - 2 * pad;
        editor.setBounds(pad, ModuleChrome::kPageTop, w, kEditorsH);

        // Below the "--- SCALE ---" band: one label-above row of combos,
        // then one of bars (ModuleChrome metrics).
        int y = ModuleChrome::kPageTop + kEditorsH + ModuleChrome::kSectionCaptionH;
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &rootCombo, &scaleCombo, &modeCombo });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &strengthSlider, &widthSlider,
                                     &slopeSlider, &glideSlider });
    }

private:
    Sp3ctraAudioProcessor& processor;
    int slot_ { 0 };   // pool slot of the bound instance

    HarmoEditorComponent editor;

    juce::ComboBox rootCombo, scaleCombo, modeCombo;
    Sp3ctraBarSlider strengthSlider, widthSlider, slopeSlider, glideSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        rootAttach, scaleAttach, modeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        strengthAttach, widthAttach, slopeAttach, glideAttach;
    std::unique_ptr<MidiLearnAttachment>
        scaleLearn_, modeLearn_, strengthLearn_, widthLearn_,
        slopeLearn_, glideLearn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxHarmoTabComponent)
};
