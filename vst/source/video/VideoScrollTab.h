#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VideoScrollMode.h"

// Forward declaration — VideoWindow.h included in .cpp to avoid heavy includes
class VideoWindow;

/**
 * @brief VIDEO tab — live performance controls for the video scroll window.
 *
 * Placed as the 4th main tab in the plugin editor (IMAGE | SYNTH | SAMPLER | VIDEO).
 *
 * Dissociation of concerns (task requirement):
 *   - This tab = live controls only:
 *       source selection (L / Sample / Mix / LuxPitch)
 *       scroll mode, speed, direction, zoom, exposure, blend mode
 *       open/close window, fullscreen toggle
 *   - Configuration (brightness, invert, color mode, dimensions, BPM, MIDI sync)
 *     lives in the Settings window → "Video Scroll" tab (VideoScrollSettingsTab).
 *
 * APVTS parameters used here:
 *   "videoScrollEnabled"    bool   — master enable (opens/closes the VideoWindow)
 *   "videoScrollSource"     choice — L / Sample / Mix / LuxPitch
 *   "videoScrollMode"       choice — scroll direction + loop mode
 *   "videoScrollSpeed"      float  — scroll speed factor [0.1 .. 20 x]
 *   "videoScrollDirection"  choice — Forward / Reverse
 *   "videoScrollZoom"       float  — zoom factor [0.5 .. 4 x]
 *   "videoScrollExposure"   float  — exposure [0.0 .. 1.0]
 *   "videoScrollBlendMode"  choice — Mix / Add / Screen / Mask
 */
class VideoScrollTab : public juce::Component,
                       private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit VideoScrollTab(Sp3ctraAudioProcessor& processor);
    ~VideoScrollTab() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Called by PluginEditor when the user leaves the VIDEO tab. */
    void onTabDeactivated();

    /** Called by PluginEditor when the user enters the VIDEO tab. */
    void onTabActivated();

private:
    // ── APVTS listener ───────────────────────────────────────────────────────
    void parameterChanged(const juce::String& paramID, float newValue) override;

    // ── Window management ─────────────────────────────────────────────────────
    void openVideoWindow();
    void closeVideoWindow();
    void toggleVideoWindow();
    void requestFullscreen();
    void updateUIFromState();

    // ── Reference ────────────────────────────────────────────────────────────
    Sp3ctraAudioProcessor& processor_;

    // ── ACTIVATION ───────────────────────────────────────────────────────────
    juce::ToggleButton enableToggle_ { "Enable" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttach_;

    // ── SOURCE ───────────────────────────────────────────────────────────────
    juce::ComboBox sourceCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAttach_;

    // ── SCROLL MODE ───────────────────────────────────────────────────────────
    juce::ComboBox modeCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttach_;

    // ── SPEED ────────────────────────────────────────────────────────────────
    juce::Slider speedSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach_;

    // ── DIRECTION ────────────────────────────────────────────────────────────
    juce::ComboBox directionCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> directionAttach_;

    // ── SEQ MAX FRAMES ────────────────────────────────────────────────────────
    juce::Slider maxDurSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxDurAttach_;

    // ── ZOOM (live control) ───────────────────────────────────────────────────
    juce::Slider zoomSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> zoomAttach_;

    // ── EXPOSURE ─────────────────────────────────────────────────────────────
    juce::Slider exposureSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> exposureAttach_;

    // ── BLEND MODE ───────────────────────────────────────────────────────────
    juce::ComboBox blendModeCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> blendModeAttach_;

    // ── ACTION BUTTONS ───────────────────────────────────────────────────────
    juce::TextButton windowBtn_;
    juce::TextButton fullscreenBtn_;

    // ── VIDEO WINDOW (owned here) ─────────────────────────────────────────────
    std::unique_ptr<VideoWindow> videoWindow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollTab)
};
