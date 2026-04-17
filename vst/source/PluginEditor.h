#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "SettingsWindow.h"
#include "CisVisualizerComponent.h"
#include "image/ImagePageComponent.h"
#include "sampler/SamplerPageComponent.h"
#include "video/VideoScrollTab.h"
#include "UITheme.h"
#include "Sp3ctraLookAndFeel.h"

// ============================================================================
// GearButton — settings icon rendered as a yellow cogwheel.
// Self-contained: painting logic lives in the header to avoid a separate TU.
// ============================================================================
class GearButton : public juce::Button
{
public:
    GearButton() : juce::Button("settings") {}

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b  = getLocalBounds().toFloat().reduced(3.f);
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float r  = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;

        // Background
        const juce::Colour bg(0xff2a2a2a);
        g.setColour(isButtonDown ? bg.brighter(0.3f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 4.f);

        // Cogwheel (yellow)
        const juce::Colour gear = isButtonDown ? juce::Colour(0xffffe066)
                                : isMouseOver  ? juce::Colour(0xffffcc00)
                                :                juce::Colour(0xffc89600);
        g.setColour(gear);
        g.fillPath(makeGearPath(cx, cy, r * 0.82f, 8));

        // Centre hole — punched out with background colour
        g.setColour(bg);
        g.fillEllipse(cx - r * 0.23f, cy - r * 0.23f, r * 0.46f, r * 0.46f);
    }

private:
    static juce::Path makeGearPath(float cx, float cy, float r, int teeth)
    {
        const float outer = r;
        const float inner = r * 0.68f;
        const float arc   = juce::MathConstants<float>::twoPi / (float)(teeth * 2);
        const float half  = arc * 0.36f;
        juce::Path p;
        bool first = true;
        for (int i = 0; i < teeth * 2; ++i)
        {
            const float ri = (i % 2 == 0) ? outer : inner;
            const float a0 = arc * (float)i - half;
            const float a1 = arc * (float)i + half;
            if (first) { p.startNewSubPath(cx + ri * std::cos(a0), cy + ri * std::sin(a0)); first = false; }
            else         p.lineTo         (cx + ri * std::cos(a0), cy + ri * std::sin(a0));
            p.lineTo(cx + ri * std::cos(a1), cy + ri * std::sin(a1));
        }
        p.closeSubPath();
        return p;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GearButton)
};

//==============================================================================
/**
 * @brief Main VST editor with three-tab navigation (IMAGE / SYNTH / SAMPLER).
 *
 * Header bar:
 *   Left  : "Sp3ctra" logo
 *   Centre: version string (v0.1.5)
 *   Right : GearButton — opens the full settings window
 *
 * IMAGE tab — ImagePageComponent (two-column layout):
 *   Left  : Image processing (Gamma, Contrast Min, Opacities, Transport)
 *   Right : Blob detection (StrokeForge detection params — moved from Synth)
 *
 * SYNTH tab — single left column (audio-only params):
 *   Device On, Volume, Attack, Release, Stereo Temp., Sum. Exp., Noise Gate
 *   No image pre-processing parameters in this tab.
 *
 * SAMPLER tab — SamplerPageComponent
 */
class Sp3ctraAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor&);
    ~Sp3ctraAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void suspendVisualizer();
    void resumeVisualizer();

private:
    // ── Active tab ────────────────────────────────────────────────────────────
    enum class Tab { Image, Synth, Sampler, Video };
    enum class SynthSub { LuxStral, LuxSynth };

    // ── Layout constants ──────────────────────────────────────────────────────
    static constexpr int kHeaderH    = 52;
    static constexpr int kVisY       = kHeaderH + 8;
    static constexpr int kVisH       = 64;
    static constexpr int kTabsY      = kVisY + kVisH + 6;
    static constexpr int kTabsH      = 26;
    static constexpr int kSubTabsY   = kTabsY + kTabsH + 6;
    static constexpr int kSubTabsH   = 22;
    static constexpr int kPageTop    = kSubTabsY + kSubTabsH + 4;
    static constexpr int kColGap     = 18;
    static constexpr int kCtrlW      = 210;
    // SYNTH tab left col: 8 audio-only rows
    static constexpr int kLS_ROWS    = 8;
    // SYNTH tab right col: 8 StrokeForge rows
    static constexpr int kSF_ROWS    = 5;  // Enable + Square at Width + Focus Sigma + Spectral Thr. + Focus Only

    static constexpr int kHPad       = Sp3ctraTheme::kHPad;
    static constexpr int kSectionH   = Sp3ctraTheme::kSectionH;
    static constexpr int kSectionGap = Sp3ctraTheme::kSectionGap;
    static constexpr int kRowH       = Sp3ctraTheme::kControlH;
    static constexpr int kRowStep    = Sp3ctraTheme::kRowStep;
    static constexpr int kLabelW     = Sp3ctraTheme::kLabelW;
    static constexpr int kCtrlOffset = kLabelW + 8;

    int colWidth()   const noexcept { return (getWidth() - 2*kHPad - kColGap) / 2; }
    int colLX()      const noexcept { return kHPad; }
    int colRX()      const noexcept { return kHPad + colWidth() + kColGap; }
    int rowsStartY() const noexcept { return kPageTop + kSectionH + kSectionGap; }

    void openSettings();
    void switchToTab(Tab tab);

    Sp3ctraAudioProcessor& audioProcessor;

    // ── State ─────────────────────────────────────────────────────────────────
    Tab currentTab { Tab::Image };
    SynthSub currentSynthSub { SynthSub::LuxStral };

    // ── Tab navigation (4 tabs: IMAGE | SYNTH | SAMPLER | VIDEO) ─────────────
    juce::TextButton imageTabBtn   { "IMAGE" };
    juce::TextButton synthTabBtn   { "SYNTH" };
    juce::TextButton samplerTabBtn { "SAMPLER" };
    juce::TextButton videoTabBtn   { "VIDEO" };

    // ── Synth sub-tab buttons ────────────────────────────────────────────────
    juce::TextButton luxstralSubBtn  { "LuxStral" };
    juce::TextButton luxsynthSubBtn  { "LuxSynth" };

    void switchSynthSubTab(SynthSub sub);

    // ── CIS Visualizer ────────────────────────────────────────────────────────
    std::unique_ptr<CisVisualizerComponent> cisVisualizer;

    // ── IMAGE page — delegated to ImagePageComponent ───────────────────────────
    std::unique_ptr<ImagePageComponent> imagePage;

    // ── SYNTH page — LuxStral audio params (image params removed) ─────────────
    juce::ToggleButton deviceOnToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deviceOnAttachment;

    juce::Slider luxstralVolumeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> luxstralVolumeAttachment;

    juce::Slider attackSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment;

    juce::Slider releaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;

    juce::ToggleButton stereoEnableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> stereoEnableAttachment;

    juce::Slider stereoTempSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stereoTempAttachment;

    juce::Slider sumExpSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sumExpAttachment;

    juce::Slider noiseGateSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noiseGateAttachment;

    // ── SYNTH right column — StrokeForge synthesis controls ───────────────────
    juce::ToggleButton sfEnabledToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfEnabledAttachment;

    juce::Slider sfMorphWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMorphWidthAttachment;

    juce::Slider sfFocusSigmaSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfFocusSigmaAttachment;

    juce::Slider sfSpectralThreshSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfSpectralThreshAttachment;

    juce::ToggleButton sfFocusOnlyToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfFocusOnlyAttachment;

    // ── SYNTH page — LuxSynth audio params ──────────────────────────────────
    juce::ToggleButton lxEnableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lxEnableAttachment;

    juce::Slider luxsynthVolumeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> luxsynthVolumeAttachment;

    juce::Slider lxAttackSlider, lxDecaySlider, lxSustainSlider, lxReleaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        lxAttackAttach, lxDecayAttach, lxSustainAttach, lxReleaseAttach;

    juce::Slider lxFltAttackSlider, lxFltDecaySlider, lxFltSustainSlider, lxFltReleaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        lxFltAttackAttach, lxFltDecayAttach, lxFltSustainAttach, lxFltReleaseAttach;

    juce::Slider lxFltCutoffSlider, lxFltDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        lxFltCutoffAttach, lxFltDepthAttach;

    juce::Slider lxNumOscSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        lxNumOscAttach;

    juce::Slider lxLfoRateSlider, lxLfoDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        lxLfoRateAttach, lxLfoDepthAttach;

    // ── SAMPLER page ──────────────────────────────────────────────────────────
    std::unique_ptr<SamplerPageComponent> samplerPage;

    // ── VIDEO page ────────────────────────────────────────────────────────────
    std::unique_ptr<VideoScrollTab> videoScrollPage;

    // ── LookAndFeel (declared before all JUCE components that use it) ─────────
    Sp3ctraLookAndFeel sp3ctraLaf;

    // ── Header: gear settings button ──────────────────────────────────────────
    GearButton settingsButton;
    std::unique_ptr<SettingsWindow> settingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraAudioProcessorEditor)
};
