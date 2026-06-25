#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VideoScrollMode.h"

// Forward declaration — VideoWindow.h included in .cpp to avoid heavy includes
class VideoWindow;

/**
 * @brief Live performance controls for the video scroll waterfall.
 *
 * Hosted in the zone-4 waterfall column (WaterfallColumnComponent, M4).
 *
 * Dissociation of concerns:
 *   - This component = live controls only:
 *       source selection (LuxStral / LuxSynth / AllSynth)
 *       orientation, bipolar speed, birth-line position, thickness, zoom,
 *       fade/persistence, temporal compression
 *       open/close window, fullscreen toggle
 *   - Display configuration (brightness, invert, color mode) lives in the
 *     zone-4 toolbar (WaterfallColumnComponent header area, M5); the detached
 *     window default size lives in the gear Settings window → System tab.
 *
 * APVTS parameters used here (legacy birth-line scroll model):
 *   "videoScrollEnabled"        bool  — master enable (opens/closes the VideoWindow)
 *   "videoScrollSource"         choice— LuxStral / LuxSynth-LuxWave / AllSynth
 *   "videoScrollMode"           choice— orientation (0/90/180/270 deg)
 *   "videoScrollSpeed"          float — bipolar scroll speed [-1 reverse .. +1 forward]
 *   "videoScrollLinePos"        float — birth-line position [-1 .. +1]
 *   "videoScrollLineThickness"  float — scanline thickness [0 .. 1]
 *   "videoScrollZoom"           float — zoom factor [0.5 .. 4 x]
 *   "videoScrollFade"           float — progressive aging w/ distance [0 .. 1]
 *   "videoScrollMaxDuration"    float — progressive time-squish [1 .. 64]
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

    /** Opens (or re-shows) the detached VideoWindow — exposed for the
     *  zone 4 waterfall column's [⧉] button (M4 layout). Same code path
     *  the old VIDEO tab used internally. */
    void openDetachedWindow() { openVideoWindow(); }

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

    // ── SPEED (bipolar: reverse ◄ freeze ► forward) ──────────────────────────
    juce::Slider speedSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach_;

    // ── LINE POSITION (birth line, enables bidirectional scroll) ──────────────
    juce::Slider linePosSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> linePosAttach_;

    // ── LINE THICKNESS (1 px → barcode) ───────────────────────────────────────
    juce::Slider thicknessSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thicknessAttach_;

    // ── ZOOM (live control) ───────────────────────────────────────────────────
    juce::Slider zoomSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> zoomAttach_;

    // ── FADE / PERSISTENCE (trails) ───────────────────────────────────────────
    juce::Slider fadeSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fadeAttach_;

    // ── COMPRESSION (temporal — CIS frames averaged per painted line) ─────────
    juce::Slider maxDurSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxDurAttach_;

    // ── ACTION BUTTONS ───────────────────────────────────────────────────────
    juce::TextButton windowBtn_;
    juce::TextButton fullscreenBtn_;

    // ── VIDEO WINDOW (owned here) ─────────────────────────────────────────────
    std::unique_ptr<VideoWindow> videoWindow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollTab)
};
