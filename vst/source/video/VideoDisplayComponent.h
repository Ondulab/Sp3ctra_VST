#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "VideoScrollMode.h"
#include <vector>
#include <atomic>
#include <cstdint>

class Sp3ctraAudioProcessor;

/**
 * @brief Bidirectional "birth-line" waterfall display for CIS image data.
 *
 * Reproduces the original Sp3ctra SFML renderer (display.c): rather than a
 * one-way waterfall, the image is split at a movable birth line — content above
 * scrolls one way and content below the other, so it spreads symmetrically away
 * from (or, in reverse, toward) the line where new scanlines are stamped.
 *
 * Architecture:
 * ─────────────────────────────────────────────────────────────────────────────
 *   CIS UDP thread  →  AudioImageBuffers (double-buffer, latest frame)
 *                               ↓  polled every ~1ms
 *   CaptureThread            →  frameRing_ (lock-free SPSC ring of scanlines)
 *                               ↓  consumed every timer tick
 *   timerCallback() (60fps)  →  scrollStep(): ping-pong blit of historyA_/B_
 *                               ↓
 *   paint()                  →  viewport (centred on birth line) + zoom/rotate
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Key controls (all APVTS):
 *   - videoScrollSpeed         bipolar scroll speed (reverse / freeze / forward)
 *   - videoScrollLinePos       birth-line position (centre = symmetric scroll)
 *   - videoScrollLineThickness scanline thickness (1 px → barcode)
 *   - videoScrollZoom          display zoom about centre
 *   - videoScrollFade          progressive aging (desaturate + dim) with distance
 *   - videoScrollMaxDuration   progressive time-squish (compression) with distance
 *   - videoScrollMode          orientation (0/90/180/270 deg rotation in paint)
 *
 * Thread safety:
 *   - frameRing_ uses an atomic write index (capture thread) + non-atomic read
 *     index (message thread only) → lock-free single-producer single-consumer.
 *   - historyA_/historyB_ are written only from timerCallback (message thread);
 *     paint() also runs on the message thread → no concurrent access.
 */
class VideoDisplayComponent : public juce::Component,
                              private juce::Timer
{
public:
    explicit VideoDisplayComponent(Sp3ctraAudioProcessor& proc);
    ~VideoDisplayComponent() override;

    // ── JUCE Component overrides ──────────────────────────────────────────────
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    // ── Callback: double-click toggles fullscreen ─────────────────────────────
    std::function<void()> onFullscreenRequested;

private:
    // ── Forward-declared ring frame (used in method signatures below) ─────────
    struct RingFrame
    {
        std::vector<uint8_t> r, g, b, gray;
    };

    // ── Dedicated CIS capture thread ──────────────────────────────────────────
    class CaptureThread final : public juce::Thread
    {
    public:
        explicit CaptureThread(VideoDisplayComponent& owner)
            : juce::Thread("VideoCaptureThread"), owner_(owner) {}
        void run() override;
    private:
        VideoDisplayComponent& owner_;
    };

    void captureCurrentFrame();   // called from capture thread

    // ── Timer (display refresh) ───────────────────────────────────────────────
    void timerCallback() override;

    // ── Bidirectional waterfall (legacy birth-line model) ─────────────────────
    void allocateScrollBuffer();
    // Blanks both history buffers to black and resets the scroll accumulator —
    // used by the transport "Stop" (clears the waterfall to a fresh start).
    void clearHistory();
    // Performs one ping-pong scroll step: shifts the two zones around the birth
    // line, stamps the freshly-built scanline, and swaps the buffers.
    void scrollStep();
    // Builds the fresh stamp as a temporally-resolved strip (width bufW_,
    // height bandH).  The newest `available` CIS frames are box-averaged on the
    // CIS axis (anti-alias) and distributed across the CENTRAL coreH rows
    // (oldest row first → newest at the birth line); the remaining thickness is
    // padded by holding the core's edge rows, so a thick line is a clean fat bar
    // rather than a temporal stretch.  coreH should equal the motion gap
    // (2*scroll) to keep the waterfall time-scale constant.  Returns false if no
    // new frame.  `available`/`wr` are the ring snapshot taken by scrollStep()
    // (which also drains the ring).
    bool buildLineImage(juce::Image& out, int coreH, int bandH,
                        int available, int wr);

    // ── Processor reference ───────────────────────────────────────────────────
    Sp3ctraAudioProcessor& processor_;

    // ── Frame ring buffer (SPSC: capture thread → timer callback) ────────────
    // Each entry holds one CIS scanline (R/G/B/Gray channels).
    // kRingSize must be power of 2 for cheap modulo.
    static constexpr int kRingSize = 2048; // ~2 s at 1000fps
    std::vector<RingFrame>  frameRing_;
    std::atomic<int>        ringWriteIdx_ { 0 };   // written by capture thread
    int                     ringReadIdx_  { 0 };   // read by timer tick (msg thread)
    int                     cisCount_     { 0 };   // updated by capture thread, checked atomically

    // Counters from AudioImageBuffers — detect new frames without mutex.
    // - lastLinesReceived_  : tracks raw UDP frames (incremented on every
    //                         complete_write() from the UDP thread)
    // - lastLinesModulated_ : tracks modulated frames (incremented on every
    //                         snapshot_modulated() from the synthesis thread).
    //                         Required because the UDP write bus is suppressed
    //                         while the LuxSampler is playing, freezing
    //                         lines_received even though the modulated stream
    //                         keeps advancing.
    std::atomic<uint64_t>   lastLinesReceived_  { 0 };
    std::atomic<uint64_t>   lastLinesModulated_ { 0 };

    // ── Bidirectional history buffers (ping-pong, legacy birth-line model) ────
    // Two RGB images of size (bufW_ × bufH_).  Each scroll step blits the source
    // into the destination split at the birth line — the upper zone shifts one
    // way, the lower zone the other — then stamps the new scanline at the birth
    // line and swaps.  bufH_ is 2× the component height so the birth line can be
    // repositioned and the viewport still has content on both sides.
    juce::Image historyA_, historyB_;
    // Offscreen scratch for paint(): the linear history warped (time-squish) and
    // aged (fade) as a function of distance from the birth line, before the
    // zoom/orientation transform is applied.  Sized bufW_ × compH_.
    juce::Image warpBuf_;
    // Reused paint() scratch (avoids per-frame allocation):
    //   warpEdge_      — compH_+1 buffer-row edges of the squish map
    //   accR_/G_/B_    — per-column row accumulators (size bufW_)
    //   psR_/G_/B_     — per-row horizontal-blur prefix sums (size bufW_+1)
    //   accR_/G_/B_    — per-column row accumulators (size bufW_)
    //   psR_/G_/B_     — per-row horizontal-blur prefix sums (size bufW_+1)
    std::vector<int> warpEdge_;
    std::vector<int> accR_, accG_, accB_;
    std::vector<int> psR_, psG_, psB_;
    int  curBuf_  { 0 };       // after a step: 0 → A is freshly drawn, 1 → B
    int  compW_   { 0 };       // component width  (viewport width)
    int  compH_   { 0 };       // component height (viewport height)
    int  bufW_    { 0 };       // history width  (= compW_)
    int  bufH_    { 0 };       // history height (= 2 × compH_)
    bool buffersInit_ { false };

    // Fractional scroll accumulator → smooth sub-pixel scroll speed.
    float scrollAccumulator_ { 0.f };

    // Last "Stop" pulse seen from the processor; when it advances we blank the
    // waterfall on the next timer tick (transport clear).
    uint32_t lastClearGen_ { 0 };

    // ── Display refresh rate ──────────────────────────────────────────────────
    static constexpr int kTimerFps = 60; // raised from 30 for smoother display

    // ── Owned capture thread ──────────────────────────────────────────────────
    CaptureThread captureThread_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoDisplayComponent)
};
