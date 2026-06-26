#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VideoScrollMode.h"
#include <functional>

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
 *     The detached window is opened/closed/fullscreened from the zone-4
 *     column header (small/large window icons + green status dot).
 *   - Display configuration (invert, color mode) lives in the zone-4 toolbar
 *     (WaterfallColumnComponent header area, M5); the detached window default
 *     size lives in the gear Settings window → System tab.
 *
 * APVTS parameters used here (legacy birth-line scroll model):
 *   "videoScrollSource"         choice— LuxStral / LuxSynth-LuxWave / AllSynth
 *   "videoScrollMode"           choice— orientation (0/90/180/270 deg)
 *   "videoScrollSpeed"          float — bipolar scroll speed [-1 reverse .. +1 forward]
 *   "videoScrollLinePos"        float — birth-line position [-1 .. +1]
 *   "videoScrollLineThickness"  float — scanline thickness [0 .. 1]
 *   "videoScrollZoom"           float — zoom factor [0.5 .. 4 x]
 *   "videoScrollFade"           float — progressive aging w/ distance [0 .. 1]
 *   "videoScrollMaxDuration"    float — progressive time-squish [1 .. 64]
 */
class VideoScrollTab : public juce::Component
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
     *  zone 4 waterfall column's [⧉] button (M4 layout). */
    void openDetachedWindow() { openVideoWindow(); }

    /** Toggles the detached VideoWindow: opens it if closed, closes it if open.
     *  Driven by the column header's small-window [⧉] icon. */
    void toggleDetachedWindow();

    /** Opens the detached window (if needed) and toggles full screen.
     *  Driven by the column header's large-window [⛶] icon. */
    void requestFullscreenWindow();

    /** True while the detached VideoWindow exists and is visible — drives the
     *  column header's green status dot. */
    bool isVideoWindowOpen() const noexcept;

    /** Fired whenever the detached window opens or closes, so the host column
     *  can refresh its status dot. */
    std::function<void()> onWindowStateChanged;

private:
    // ── Window management ─────────────────────────────────────────────────────
    void openVideoWindow();
    void closeVideoWindow();
    void updateUIFromState();

    // ── Reference ────────────────────────────────────────────────────────────
    Sp3ctraAudioProcessor& processor_;

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

    // ── VIDEO WINDOW (owned here) ─────────────────────────────────────────────
    std::unique_ptr<VideoWindow> videoWindow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollTab)
};
