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
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;

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

    // ── FFT spectrum visualization ────────────────────────────────────────────
    /**
     * @brief Compute FFT magnitude spectrum from localDataGray.
     *
     * Uses KissFFT real FFT (kiss_fftr) with a Hann window to reduce spectral
     * leakage.  Results are stored in fftMagnitudesSmoothed_ (exponential
     * moving average: fast attack α=0.40, slow release α=0.08).
     *
     * DC bin (k=0) is suppressed.  Magnitudes are peak-normalised to [0..1].
     * The KissFFT config is cached in fftCfg_ and only reallocated when
     * cisPixelsCount changes.
     *
     * Called at most once per timer tick (30 fps) from paintFftGrayMode()
     * or paintFftColorMode() — always on the UI/message thread.
     */
    void computeFftMagnitudes();

    /** Render the FFT spectrum as a monochromatic bar chart (gray scale). */
    void paintFftGrayMode (juce::Graphics& g, int W, int H);

    /** Render the FFT spectrum with per-bin HSV colour mapping
     *  (blue = low frequency, red = high frequency). */
    void paintFftColorMode(juce::Graphics& g, int W, int H);

    // ── Mode helpers ──────────────────────────────────────────────────────────
    /** Returns true when the active source is a COLOR (temperature map) view. */
    bool isColorSource(VisualizerMode m) const noexcept;

    /** Returns true when the active source supports display mode switching
     *  (Image / Waveform / Inverted Waveform).  COLOR and BLOB do not. */
    bool supportsDisplayModes(VisualizerMode m) const noexcept;

    // ── SYNTH_BLOB mode: coloured blob visualizer ─────────────────────────────
    /**
     * @brief One detected blob on the LuxSynth path.
     *
     * Detected from localDataGray (activity = brightness after LuxSynth pipeline)
     * AND from localDataR/G/B (color temperature used to split blobs at
     * color-temperature discontinuities → color + continuity detection).
     */
    struct SynthBlob
    {
        int          startPx;        ///< First CIS pixel index (inclusive)
        int          endPx;          ///< Last  CIS pixel index (exclusive)
        float        peakIntensity;  ///< Max  brightness within blob [0..1]
        float        avgIntensity;   ///< Mean brightness within blob [0..1]
        float        avgColorTemp;   ///< Mean (R-B)/255 — kept for tooltip warm/cool display
        juce::Colour color;          ///< Unique display colour (hue-wheel)
        juce::Colour avgLocalColor;  ///< Mean locally-smoothed RGB (used by color-merge logic)
    };

    /** Max blobs for SYNTH_BLOB mode — matches piano keyboard range. */
    static constexpr int kMaxSynthBlobs = 88;

    /**
     * @brief Detect blobs from localDataGray + localDataR/G/B.
     *
     * Algorithm:
     *  1. Activity = localDataGray / 255 (bright ≥ threshold → active pixel)
     *  2. 1-D connected-component scan with gap tolerance (strokeforge_blob_merge_gap)
     *  3. Color-temperature jump > kColorSplitThreshold → split running blob
     *  4. Blobs narrower than strokeforge_blob_min_width are discarded
     *  5. At most kMaxSynthBlobs (88) blobs kept
     *  6. Each blob gets a unique hue (evenly spaced on the HSV wheel)
     *
     * Results written to synthBlobs_ (cleared on each call).
     * Called from paintSynthBlobMode() at 30 fps — the 1-D scan is O(N) and
     * fast enough for the UI thread.
     */
    void detectSynthBlobs();

    /**
     * @brief Render the SYNTH_BLOB visualizer.
     *
     * Displays:
     *  - Dark background
     *  - Per-pixel waveform fill, coloured by blob membership
     *  - Blob outlines with unique colours
     *  - Peak-intensity horizontal marker per blob
     *  - Blob number + width + intensity label (when space allows)
     *  - "N/88 blobs" count badge
     */
    void paintSynthBlobMode(juce::Graphics& g, int W, int H);

    /** Cache of detected blobs — rebuilt every paint call in SYNTH_BLOB mode. */
    std::vector<SynthBlob> synthBlobs_;

    // ── FFT state (UI thread only, owned by computeFftMagnitudes) ────────────
    /** Raw per-bin FFT magnitudes after peak normalisation.
     *  Size = cisPixelsCount / 2 + 1.  DC bin (index 0) is always 0. */
    std::vector<float> fftMagnitudes_;
    /** Exponentially smoothed version of fftMagnitudes_ for display.
     *  Same size as fftMagnitudes_. */
    std::vector<float> fftMagnitudesSmoothed_;
    /** cisPixelsCount value used for the last kiss_fftr_alloc call. */
    int                fftSize_ { 0 };
    /** Opaque pointer to a kiss_fftr_cfg.
     *  Cast to kiss_fftr_cfg inside CisVisualizerComponent.cpp only. */
    void*              fftCfg_  { nullptr };

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

    // ── SYNTH_BLOB hover tooltip ───────────────────────────────────────────────
    // Index of the blob currently under the mouse cursor (-1 = none).
    // Updated by mouseMove() / mouseExit(); read by paintSynthBlobMode().
    // Both methods run on the UI/message thread — no synchronisation needed.
    int              hoverBlobIdx_ { -1 };
    juce::Point<int> hoverPos_     {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CisVisualizerComponent)
};
