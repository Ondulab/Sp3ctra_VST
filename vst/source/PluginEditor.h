#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "SettingsWindow.h"
#include "CisVisualizerComponent.h"
#include "sampler/SamplerPageComponent.h"

//==============================================================================
/**
 * @brief Main VST editor with tab navigation (SYNTH / SAMPLER).
 *
 * SYNTH tab — two-column layout:
 *   Left  : LuxStral params (Device On, Volume, Gamma, Contrast, Attack,
 *            Release, Stereo Temp., Sum. Exp., Noise Gate)
 *   Right : StrokeForge params (Active, Blob Thr., Merge Gap, Focus σ,
 *            Spectral Thr., Focus Only)
 *
 * SAMPLER tab — SamplerPageComponent:
 *   SlotGrid + SlotEditor + Sequencer + Transport bar
 *
 * A "Settings..." button at the bottom opens the full settings window.
 */
class Sp3ctraAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor&);
    ~Sp3ctraAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void suspendVisualizer();
    void resumeVisualizer();

private:
    // ── Layout constants ─────────────────────────────────────────────────────
    static constexpr int kHeaderH    = 52;
    static constexpr int kVisY       = kHeaderH + 8;
    static constexpr int kVisH       = 64;
    static constexpr int kTabsY      = kVisY + kVisH + 6;   // = 130
    static constexpr int kTabsH      = 26;
    static constexpr int kPageTop    = kTabsY + kTabsH + 6; // = 162
    static constexpr int kHPad       = 10;
    static constexpr int kColGap     = 18;
    static constexpr int kSectionH   = 24;
    static constexpr int kSectionGap = 4;
    static constexpr int kRowH       = 27;
    static constexpr int kRowStep    = kRowH + 4;  // = 31
    static constexpr int kLabelW     = 110;
    static constexpr int kCtrlOffset = kLabelW + 8;
    static constexpr int kCtrlW      = 210;
    static constexpr int kLS_ROWS    = 9;
    static constexpr int kSF_ROWS    = 6;

    int colWidth()   const noexcept { return (getWidth() - 2*kHPad - kColGap) / 2; }
    int colLX()      const noexcept { return kHPad; }
    int colRX()      const noexcept { return kHPad + colWidth() + kColGap; }
    int rowsStartY() const noexcept { return kPageTop + kSectionH + kSectionGap; }
    int footerY()    const noexcept { return getHeight() - 42; }

    void timerCallback() override;
    void openSettings();
    void switchToPage(bool showSampler);

    Sp3ctraAudioProcessor& audioProcessor;

    // ── Tab navigation ────────────────────────────────────────────────────────
    bool showingSamplerPage = false;
    juce::TextButton synthTabBtn   { "SYNTH" };
    juce::TextButton samplerTabBtn { "SAMPLER" };

    // ── CIS Visualizer ────────────────────────────────────────────────────────
    std::unique_ptr<CisVisualizerComponent> cisVisualizer;

    // ── SYNTH page — LuxStral ─────────────────────────────────────────────────
    juce::ToggleButton deviceOnToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deviceOnAttachment;

    juce::Slider masterVolumeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;

    juce::Slider gammaSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gammaAttachment;

    juce::Slider attackSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment;

    juce::Slider releaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;

    juce::Slider contrastMinSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> contrastMinAttachment;

    juce::Slider stereoTempSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stereoTempAttachment;

    juce::Slider sumExpSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sumExpAttachment;

    juce::Slider noiseGateSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noiseGateAttachment;

    // ── SYNTH page — StrokeForge ──────────────────────────────────────────────
    juce::ToggleButton sfEnabledToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfEnabledAttachment;

    juce::Slider sfBlobThreshSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfBlobThreshAttachment;

    juce::Slider sfMergeGapSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMergeGapAttachment;

    juce::Slider sfFocusSigmaSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfFocusSigmaAttachment;

    juce::Slider sfSpectralWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfSpectralWidthAttachment;

    juce::ToggleButton sfFocusOnlyToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfFocusOnlyAttachment;

    // ── SAMPLER page ──────────────────────────────────────────────────────────
    std::unique_ptr<SamplerPageComponent> samplerPage;

    // ── Footer ────────────────────────────────────────────────────────────────
    juce::TextButton settingsButton;
    juce::Label      statusLabel;
    std::unique_ptr<SettingsWindow> settingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraAudioProcessorEditor)
};
