/**
 * @file LuxGrainPanel.h
 * @brief LUXGRAIN engine page — the stochastic cloud's playable parameters.
 *
 * Shown in ZONE 3 when the LUXGR strip is selected in the AUDIO MIX dock
 * (engine view). The "→ LUXGRAIN" rack block keeps showing the shared
 * OUT/send page (conditioning per chain) — same split as LuxWave. The
 * machine-level settings (Bands, LOAD SAMPLE) live on the SETUP face.
 *
 * Single-column stack, shared audio-panel visual language:
 *
 *   Volume ────────────────────────────── (luxgrainVolume, mirrors the dock)
 *   ┌ CLOUD — STOCHASTIC EMISSION ─────┐
 *   │ Envelope [Hann ▾]                │   toggle strip hosts the env combo
 *   │  Density / Shape / Spread / Edge │
 *   └──────────────────────────────────┘
 *   ┌ GRAIN — MORPHOLOGY ──────────────┐
 *   │  Size Min / Size Max / Texture>Size / Jitter
 *   │  Width / Color Pan / Amp Follow  │
 *   └──────────────────────────────────┘
 *   ┌ MATERIAL ────────────────────────┐
 *   │ Material [Sine ▾]                │   (sample loads on the SETUP face)
 *   │  Scrub                           │
 *   └──────────────────────────────────┘
 *
 * Every slider is MIDI-learnable (right click); the combos are deliberately
 * not mappable (house rule).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "AudioPanelWidgets.h"
#include "ModuleCatalog.h"
#include <memory>
#include <vector>

class LuxGrainPanel : public juce::Component
{
public:
    explicit LuxGrainPanel(Sp3ctraAudioProcessor& p) : processor(p)
    {
        auto& apvts = p.getAPVTS();

        volumeLabel.setText("Volume", juce::dontSendNotification);
        volumeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        volumeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb8c4d0));
        addAndMakeVisible(volumeLabel);

        volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52,
                                     Sp3ctraTheme::kControlH - 4);
        addAndMakeVisible(volumeSlider);
        volumeAttach = std::make_unique<SldAttach>(apvts, "luxgrainVolume",
                                                   volumeSlider);

        struct KnobDef { juce::Slider* s; const char* id; const char* suffix; };
        const KnobDef knobs[] = {
            { &densitySlider,  "luxgrainDensity",      nullptr },
            { &shapeSlider,    "luxgrainDensityShape", nullptr },
            { &spreadSlider,   "luxgrainSpread",       nullptr },
            { &edgeSlider,     "luxgrainEdge",         nullptr },
            { &sizeMinSlider,  "luxgrainSizeMin",      " ms"   },
            { &sizeMaxSlider,  "luxgrainSizeMax",      " ms"   },
            { &textureSlider,  "luxgrainTexture",      nullptr },
            { &jitterSlider,   "luxgrainJitter",       " st"   },
            { &widthSlider,    "luxgrainWidth",        nullptr },
            { &colorPanSlider, "luxgrainColorPan",     nullptr },
            { &followSlider,   "luxgrainAmpFollow",    nullptr },
            { &scrubSlider,    "luxgrainScrub",        nullptr },
        };
        for (const auto& k : knobs)
        {
            AudioPanelUI::initKnob(*k.s, k.suffix);
            addAndMakeVisible(*k.s);
            knobAttach.push_back(std::make_unique<SldAttach>(apvts, k.id, *k.s));
        }

        envCombo.addItemList({ "Hann", "Tukey", "Expodec", "Rexpodec" }, 1);
        addAndMakeVisible(envCombo);
        envAttach = std::make_unique<CmbAttach>(apvts, "luxgrainEnvShape",
                                                envCombo);

        matCombo.addItemList({ "Sine", "Sample" }, 1);
        addAndMakeVisible(matCombo);
        matAttach = std::make_unique<CmbAttach>(apvts, "luxgrainMaterial",
                                                matCombo);

        // MIDI learn on every slider (combos deliberately excluded).
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            p.getMidiMap(), volumeSlider, "luxgrainVolume"));
        for (const auto& k : knobs)
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                p.getMidiMap(), *k.s, k.id));
    }

    //── Layout tokens (mirrors AudioWavePanel) ───────────────────────────────
    static constexpr int kTopPad   = 6;
    static constexpr int kHeaderH  = 30;
    static constexpr int kSecGapV  = AudioPanelLayout::kSecGapV;   // 10
    static constexpr int kSecPadB  = 8;
    static constexpr int kSecInsetX= 8;
    static constexpr int kLabelW   = 96;
    static constexpr int kPad      = 8;

    static constexpr int kCloudSecH =
        AudioPanelLayout::sectionH(4, /*toggles=*/true)  + kSecPadB;
    static constexpr int kGrainSecH =
        AudioPanelLayout::sectionH(7, /*toggles=*/false) + kSecPadB;
    static constexpr int kMatSecH =
        AudioPanelLayout::sectionH(1, /*toggles=*/true)  + kSecPadB;

    static constexpr int kPreferredH =
        kTopPad + kHeaderH + kSecGapV + kCloudSecH + kSecGapV + kGrainSecH
        + kSecGapV + kMatSecH + AudioPanelLayout::kBottomPad;

    void paint(juce::Graphics& g) override
    {
        const auto geom = computeGeom(getWidth());
        const auto accent = moduleColour(ModuleType::LuxGrain);

        AudioPanelUI::drawSectionBg(g, geom.cloudBg.getX(), geom.cloudBg.getY(),
                                    geom.cloudBg.getWidth(), geom.cloudBg.getHeight());
        AudioPanelUI::drawBadge(g, geom.cloudBg.getX() + 4, geom.cloudBg.getY() + 4,
                                geom.cloudBg.getWidth() - 8,
                                0xff2e2718, accent.getARGB(),
                                "CLOUD -- STOCHASTIC EMISSION");
        AudioPanelUI::drawSectionBg(g, geom.grainBg.getX(), geom.grainBg.getY(),
                                    geom.grainBg.getWidth(), geom.grainBg.getHeight());
        AudioPanelUI::drawBadge(g, geom.grainBg.getX() + 4, geom.grainBg.getY() + 4,
                                geom.grainBg.getWidth() - 8,
                                0xff2e2718, accent.getARGB(),
                                "GRAIN -- MORPHOLOGY");
        AudioPanelUI::drawSectionBg(g, geom.matBg.getX(), geom.matBg.getY(),
                                    geom.matBg.getWidth(), geom.matBg.getHeight());
        AudioPanelUI::drawBadge(g, geom.matBg.getX() + 4, geom.matBg.getY() + 4,
                                geom.matBg.getWidth() - 8,
                                0xff2e2718, accent.getARGB(),
                                "MATERIAL");

        g.setColour(juce::Colour(0xffb8c4d0));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText("Grain Envelope:", geom.envCaption,
                   juce::Justification::centredLeft, true);
        g.drawText("Material:", geom.matCaption,
                   juce::Justification::centredLeft, true);

        static const char* cloudNames[] = { "Density", "Dens. Shape", "Spread",
                                            "Edge Burst" };
        for (int i = 0; i < 4; ++i)
            AudioPanelUI::drawKnobLabel(g, geom.cloudGridX, geom.cloudGridW,
                                        geom.cloudGridY, i, cloudNames[i]);
        static const char* grainNames[] = { "Size Min", "Size Max",
                                            "Texture>Size", "Jitter", "Width",
                                            "Color Pan", "Amp Follow" };
        for (int i = 0; i < 7; ++i)
            AudioPanelUI::drawKnobLabel(g, geom.grainGridX, geom.grainGridW,
                                        geom.grainGridY, i, grainNames[i]);
        AudioPanelUI::drawKnobLabel(g, geom.matGridX, geom.matGridW,
                                    geom.matGridY, 0, "Scrub");
    }

    void resized() override
    {
        const auto geom = computeGeom(getWidth());

        volumeLabel .setBounds(geom.volLabel);
        volumeSlider.setBounds(geom.volSlider);
        envCombo    .setBounds(geom.envCombo);
        matCombo    .setBounds(geom.matCombo);

        juce::Slider* cloud[] = { &densitySlider, &shapeSlider, &spreadSlider,
                                  &edgeSlider };
        for (int i = 0; i < 4; ++i)
            AudioPanelUI::placeKnob(*cloud[i], geom.cloudGridX, geom.cloudGridW,
                                    geom.cloudGridY, i);
        juce::Slider* grain[] = { &sizeMinSlider, &sizeMaxSlider, &textureSlider,
                                  &jitterSlider, &widthSlider, &colorPanSlider,
                                  &followSlider };
        for (int i = 0; i < 7; ++i)
            AudioPanelUI::placeKnob(*grain[i], geom.grainGridX, geom.grainGridW,
                                    geom.grainGridY, i);
        AudioPanelUI::placeKnob(scrubSlider, geom.matGridX, geom.matGridW,
                                geom.matGridY, 0);
    }

private:
    struct Geom
    {
        juce::Rectangle<int> volLabel, volSlider;
        juce::Rectangle<int> cloudBg, grainBg, matBg;
        juce::Rectangle<int> envCaption, envCombo, matCaption, matCombo;
        int cloudGridX = 0, cloudGridW = 0, cloudGridY = 0;
        int grainGridX = 0, grainGridW = 0, grainGridY = 0;
        int matGridX = 0, matGridW = 0, matGridY = 0;
    };

    Geom computeGeom(int w) const
    {
        using namespace AudioPanelLayout;
        Geom geo;
        const int x = kPad, cw = juce::jmax(200, w - 2 * kPad);
        int y = kTopPad;

        geo.volLabel  = { x, y, kLabelW, kHeaderH };
        geo.volSlider = { x + kLabelW, y, cw - kLabelW, kHeaderH };
        y += kHeaderH + kSecGapV;

        geo.cloudBg = { x, y, cw, kCloudSecH };
        int cy = y + 4 + Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap;
        geo.envCaption = { x + kSecInsetX, cy, 110, kToggleStripH };
        geo.envCombo   = { x + kSecInsetX + 110, cy, 150, kToggleStripH };
        cy += kToggleStripH + kToggleGap;
        geo.cloudGridX = x + kSecInsetX;
        geo.cloudGridW = cw - 2 * kSecInsetX;
        geo.cloudGridY = cy;
        y += kCloudSecH + kSecGapV;

        geo.grainBg = { x, y, cw, kGrainSecH };
        geo.grainGridX = x + kSecInsetX;
        geo.grainGridW = cw - 2 * kSecInsetX;
        geo.grainGridY = y + 4 + Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap;
        y += kGrainSecH + kSecGapV;

        geo.matBg = { x, y, cw, kMatSecH };
        int my = y + 4 + Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap;
        geo.matCaption = { x + kSecInsetX, my, 70, kToggleStripH };
        geo.matCombo   = { x + kSecInsetX + 70, my, 150, kToggleStripH };
        my += kToggleStripH + kToggleGap;
        geo.matGridX = x + kSecInsetX;
        geo.matGridW = cw - 2 * kSecInsetX;
        geo.matGridY = my;
        return geo;
    }

    Sp3ctraAudioProcessor& processor;

    juce::Label    volumeLabel;
    juce::Slider   volumeSlider;
    juce::Slider   densitySlider, shapeSlider, spreadSlider, edgeSlider;
    juce::Slider   sizeMinSlider, sizeMaxSlider, textureSlider, jitterSlider,
                   widthSlider, colorPanSlider, followSlider;
    juce::Slider   scrubSlider;
    juce::ComboBox envCombo, matCombo;

    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CmbAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SldAttach> volumeAttach;
    std::vector<std::unique_ptr<SldAttach>> knobAttach;
    std::unique_ptr<CmbAttach> envAttach, matAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxGrainPanel)
};
