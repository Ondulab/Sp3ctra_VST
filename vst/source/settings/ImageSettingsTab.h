#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief Image Pipeline Settings Tab
 *
 * Persistent, advanced parameters for the image acquisition and processing
 * pipeline.  Kept separate from runtime controls (which live in the main
 * IMAGE tab of the plugin editor).
 *
 * Sections:
 *   Image Processing Flags  — Gamma enable, Invert intensity
 *   Stream Opacities        — Default live / sampler opacity, fade-in
 *   Advanced Blob Detection — Min blob width, contrast-adaptive mode,
 *                             contrast sensitivity
 */
class ImageSettingsTab : public juce::Component
{
public:
    explicit ImageSettingsTab(Sp3ctraAudioProcessor& processor);
    ~ImageSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    Sp3ctraAudioProcessor&              audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;

    // ── Section: Image Processing Flags ───────────────────────────────────────
    juce::Label        gammaEnableLabel;
    juce::ToggleButton gammaEnableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gammaEnableAttachment;

    juce::Label        invertLabel;
    juce::ToggleButton invertToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAttachment;

    // ── Section: Stream Settings ──────────────────────────────────────────────
    // Note: Live/Sampler opacity are now driven by the Mix Balance crossfader
    // in SourcesTabComponent — no separate sliders needed here.

    juce::Label  fadeInLabel;
    juce::Slider fadeInSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fadeInAttachment;

    // ── Section: Advanced Blob Detection ──────────────────────────────────────
    juce::Label  blobMinWidthLabel;
    juce::Slider blobMinWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> blobMinWidthAttachment;

    juce::Label        contrastAdaptiveLabel;
    juce::ToggleButton contrastAdaptiveToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> contrastAdaptiveAttachment;

    juce::Label  contrastSensLabel;
    juce::Slider contrastSensSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> contrastSensAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageSettingsTab)
};
