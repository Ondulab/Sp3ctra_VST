#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "image/VisualizerMode.h"
#include <vector>
#include <atomic>
#include <cstdint>
#include <utility>

class Sp3ctraAudioProcessor;

/**
 * @brief CIS Sensor Visualizer Component
 *
 * Reads CIS frame data at ~30 FPS and renders it in the selected mode.
 * When in IMAGE tab, an optional blob overlay is drawn on top of the live frame.
 *
 * Freeze modes (read from APVTS "imageFreezeMode"):
 *   0 = PLAY  — live frames updated every timer tick
 *   1 = HOLD  — last captured frame frozen (no update)
 *   2 = WHITE — display forced to full-white (silence / no signal)
 *
 * Opacity (read from APVTS "imageLiveOpacity"):
 *   1.0 = full image (dark pixels → loud, white pixels → silent)
 *   0.0 = all white (complete silence)
 *   Applied by blending each CIS pixel toward 255 (white = silence).
 *
 * Blob overlay (IMAGE tab only):
 *   Coloured bounding-box rectangles drawn over the live frame.
 *   Regions are fed from the audio thread via setBlobRegions()
 *   using a simple lock-free double-buffer (pair of vectors + atomic index).
 */
class CisVisualizerComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit CisVisualizerComponent(Sp3ctraAudioProcessor& proc);
    ~CisVisualizerComponent() override;

    // ── JUCE Component overrides ──────────────────────────────────────────────
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;

    // ── Suspend / resume (call during prepareToPlay to avoid CoreGraphics crash) ──
    void suspend();
    void resume();

    // ── Blob overlay API (call from UI thread only) ───────────────────────────
    /**
     * Show or hide the blob overlay.
     * Should be called by PluginEditor whenever the active tab changes.
     * @param visible  true = show overlay (IMAGE tab active)
     */
    void setBlobOverlayVisible(bool visible) noexcept { blobOverlayVisible = visible; }

    /**
     * Push a new set of blob regions.
     * Normalised CIS coordinates: x0, x1 ∈ [0..1] relative to total pixel count.
     * Called from the UI / message thread (not from processBlock).
     */
    void setBlobRegions(const std::vector<std::pair<float, float>>& regions);

    // ── Pipeline source selection API ─────────────────────────────────────────
    /**
     * Set which pipeline node is currently being visualized.
     * Called by PluginEditor when the user clicks a pipeline node.
     */
    void setActiveSource(VisualizerMode mode) noexcept;

    /** Get the currently active pipeline source. */
    VisualizerMode getActiveSource() const noexcept;

private:
    // ── Timer callback ────────────────────────────────────────────────────────
    void timerCallback() override;

    // ── Data acquisition and helpers ──────────────────────────────────────────
    void    updateCisData();
    uint8_t interpolateCisPixel(const uint8_t* buffer, int displayX, int displayWidth) const;

    // ── Paint helpers ─────────────────────────────────────────────────────────
    void paintImageMode          (juce::Graphics& g, int W, int H) const;
    void paintRawImageMode       (juce::Graphics& g, int W, int H) const;
    void paintWaveformMode       (juce::Graphics& g, int W, int H, bool inverted, bool useGray = false) const;
    void paintColorTemperatureMode(juce::Graphics& g, int W, int H) const;
    void paintBlobOverlay        (juce::Graphics& g, int W, int H) const;
    void paintSourceLabel        (juce::Graphics& g, int W, int H) const;

    // ── Mode helpers ──────────────────────────────────────────────────────────
    /** Returns true when the active source is a COLOR (temperature map) view. */
    bool isColorSource(VisualizerMode m) const noexcept;

    /** Returns true when the active source supports display mode switching
     *  (Image / Waveform / Inverted Waveform).  COLOR and BLOB do not. */
    bool supportsDisplayModes(VisualizerMode m) const noexcept;

    // ── Right-click context menu ──────────────────────────────────────────────
    void showDisplayModeMenu();

    // ── Processor reference ───────────────────────────────────────────────────
    Sp3ctraAudioProcessor& processor;

    // ── CIS local buffers (UI thread only, updated in timerCallback) ──────────
    // localDataR/G/B : raw mix from AudioImageBuffers (opacity already applied).
    // localDataGray  : final processed grayscale — inversion + correct gamma
    //                  applied on top of localDataR/G/B.  This is the single
    //                  source of truth for image-mode rendering AND synthesis:
    //                  it mirrors exactly what preprocess_luxstral*() computes.
    std::vector<uint8_t> localDataR;
    std::vector<uint8_t> localDataG;
    std::vector<uint8_t> localDataB;
    std::vector<uint8_t> localDataGray; // final image: inversion + gamma applied
    int cisPixelsCount = 0;

    // ── Timer config ──────────────────────────────────────────────────────────
    static constexpr int kTimerFps = 30;

    // ── Suspend flag (prevents CoreGraphics crash during prepareToPlay) ───────
    std::atomic<bool> isSuspended { false };

    // ── Active pipeline source (selected via pipeline node clicks) ────────────
    std::atomic<int> activeSource_ { static_cast<int>(VisualizerMode::MIX) };

    // ── Blob overlay ──────────────────────────────────────────────────────────
    bool blobOverlayVisible = false;
    // Double-buffer for lock-free UI updates (both buffers UI-thread-only here)
    std::vector<std::pair<float, float>> blobRegions; // normalised x0, x1 ∈ [0..1]

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CisVisualizerComponent)
};
