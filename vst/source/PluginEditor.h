#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "SettingsWindow.h"
#include "CisVisualizerComponent.h"

//==============================================================================
/**
 * @brief Main VST editor — synthesis parameters exposed directly in two columns.
 *
 * Left column  — LuxStral:
 *   Device On, Gamma, Attack (ms), Release (ms), Contrast Min,
 *   Stereo Temp., Sum. Exp., Noise Gate, Workers
 *
 * Right column — StrokeForge:
 *   SF Active, Blob Thr., Merge Gap (pix), Focus σ (pix),
 *   Spectral Thr. (pix), Focus Only
 *
 * A "Settings..." button still gives access to the full settings window
 * for advanced parameters (network, tuning, physiological filter, etc.).
 */
class Sp3ctraAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor&);
    ~Sp3ctraAudioProcessorEditor() override;

    //==========================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

    // Called by PluginProcessor::prepareToPlay() to avoid Metal/CoreGraphics races
    void suspendVisualizer();
    void resumeVisualizer();

private:
    // ── Layout constants (shared between paint() and resized()) ────────────
    // All values in pixels, at 1:1 display scale.
    static constexpr int kHeaderH    = 52;          // painted header height
    static constexpr int kVisY       = kHeaderH + 8; // top of CIS visualizer
    static constexpr int kVisH       = 64;           // CIS visualizer height
    static constexpr int kContentY   = kVisY + kVisH + 10; // top of param area
    static constexpr int kHPad       = 10;           // horizontal outer padding
    static constexpr int kColGap     = 18;           // gap between columns
    static constexpr int kSectionH   = 24;           // section label height
    static constexpr int kSectionGap = 4;            // gap section → first row
    static constexpr int kRowH       = 27;           // row (control) height
    static constexpr int kRowStep    = kRowH + 4;    // row pitch (= 31 px)
    static constexpr int kLabelW     = 110;          // width of row labels
    static constexpr int kCtrlOffset = kLabelW + 8;  // col-relative x of control
    static constexpr int kCtrlW      = 210;          // width of controls
    static constexpr int kLS_ROWS    = 9;            // LuxStral row count
    static constexpr int kSF_ROWS    = 6;            // StrokeForge row count

    // Column geometry helpers (depend on runtime window width)
    int  colWidth()   const noexcept { return (getWidth() - 2 * kHPad - kColGap) / 2; }
    int  colLX()      const noexcept { return kHPad; }
    int  colRX()      const noexcept { return kHPad + colWidth() + kColGap; }
    int  rowsStartY() const noexcept { return kContentY + kSectionH + kSectionGap; }
    int  footerY()    const noexcept { return rowsStartY() + kLS_ROWS * kRowStep + 8; }

    void timerCallback() override;
    void openSettings();

    // ── Processor reference ──────────────────────────────────────────────────
    Sp3ctraAudioProcessor& audioProcessor;

    // ── CIS Visualizer ───────────────────────────────────────────────────────
    std::unique_ptr<CisVisualizerComponent> cisVisualizer;

    // ── LuxStral — Device On toggle ──────────────────────────────────────────
    juce::ToggleButton deviceOnToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deviceOnAttachment;

    // ── LuxStral — Master Volume ──────────────────────────────────────────────
    juce::Slider masterVolumeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;

    // ── LuxStral — Synthesis sliders ─────────────────────────────────────────
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

    // ── StrokeForge — Enable toggle ───────────────────────────────────────────
    juce::ToggleButton sfEnabledToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfEnabledAttachment;

    // ── StrokeForge — Sliders ────────────────────────────────────────────────
    juce::Slider sfBlobThreshSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfBlobThreshAttachment;

    juce::Slider sfMergeGapSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMergeGapAttachment;

    juce::Slider sfFocusSigmaSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfFocusSigmaAttachment;

    juce::Slider sfSpectralWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfSpectralWidthAttachment;

    // ── StrokeForge — Focus Only toggle ──────────────────────────────────────
    juce::ToggleButton sfFocusOnlyToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfFocusOnlyAttachment;

    // ── Footer ───────────────────────────────────────────────────────────────
    juce::TextButton settingsButton;
    juce::Label      statusLabel;

    // Settings window (created on demand)
    std::unique_ptr<SettingsWindow> settingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraAudioProcessorEditor)
};
