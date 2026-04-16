#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief Image Pipeline Settings Tab
 *
 * Contains ONLY advanced parameters that are NOT already exposed in the
 * main IMAGE tab (Sources / LuxStral / LuxSynth).
 *
 * Removed (now in main UI or deprecated):
 *   - Gamma Enable     → always active, no toggle needed
 *   - Invert Intensity → replaced by per-path "Negative" toggles
 *   - Fade-In          → already in IMAGE → Sources
 *   - Blob Min Width   → sfBlobMinWidth APVTS param removed (replaced by spctrBlob*)
 *
 * Remaining:
 *   - StrokeForge Contrast Adaptive Mode
 *   - StrokeForge Contrast Sensitivity
 */
class ImageSettingsTab : public juce::Component
{
public:
    explicit ImageSettingsTab(Sp3ctraAudioProcessor& processor);
    ~ImageSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

    // ── Section: StrokeForge Blob Detection (advanced) ──────────────────────────
    juce::Label        contrastAdaptiveLabel;
    juce::ToggleButton contrastAdaptiveToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> contrastAdaptiveAttachment;

    juce::Label  contrastSensLabel;
    juce::Slider contrastSensSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> contrastSensAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageSettingsTab)
};
