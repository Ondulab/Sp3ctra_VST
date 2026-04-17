#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief Video Scroll settings tab (inside the Settings window).
 *
 * Configuration parameters only (not live performance controls):
 *   Section Display — Brightness, Invert colour, Color/Grayscale mode
 *   Section Window  — Default width, Default height
 *
 * Live controls (source, mode, speed, direction, zoom, exposure, blend,
 * max sequence duration, open/close, fullscreen) live in the VIDEO tab
 * of the main editor (VideoScrollTab).
 */
class VideoScrollSettingsTab : public juce::Component
{
public:
    explicit VideoScrollSettingsTab(Sp3ctraAudioProcessor& processor);
    ~VideoScrollSettingsTab() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts_;

    // ── Scrollable content ────────────────────────────────────────────────────
    juce::Viewport  viewport_;
    juce::Component content_;

    // ── Section: Display ──────────────────────────────────────────────────────
    juce::Label      displaySectionLabel_;

    juce::Label      brightnessLabel_;
    juce::Slider     brightnessSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> brightnessAttach_;

    juce::Label      invertLabel_;
    juce::ToggleButton invertToggle_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAttach_;

    juce::Label      colorModeLabel_;
    juce::ToggleButton colorModeToggle_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> colorModeAttach_;

    // ── Section: Window ───────────────────────────────────────────────────────
    juce::Label      windowSectionLabel_;

    juce::Label      windowWLabel_;
    juce::Slider     windowWSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> windowWAttach_;

    juce::Label      windowHLabel_;
    juce::Slider     windowHSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> windowHAttach_;

    // ── Layout helper ─────────────────────────────────────────────────────────
    void layoutContent();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollSettingsTab)
};
