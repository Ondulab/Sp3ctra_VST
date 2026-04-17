#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "VideoScrollMode.h"
#include <vector>
#include <atomic>
#include <cstdint>

class Sp3ctraAudioProcessor;

/**
 * @brief Full-resolution scrolling waterfall display for CIS image data.
 *
 * Architecture (matches Legacy SFML approach):
 * ─────────────────────────────────────────────────────────────────────────────
 *   CIS UDP thread  →  AudioImageBuffers (double-buffer, latest frame)
 *                               ↓  polled every ~1ms
 *   VideoFrameCaptureThread  →  frameRing_ (circular ring of captured frames)
 *                               ↓  drained every timer tick
 *   timerCallback() (30fps)  →  scrollBuffer_ (juce::Image, circular write)
 *                               ↓
 *   paint()                  →  drawn with writeRow_ offset (no memcpy)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Key design decisions:
 *   - NO moveImageSection() — zero memcpy for scrolling.
 *     writeRow_ advances forward; paint() renders the Image as a circular
 *     buffer using two drawImage calls (top/bottom halves).
 *   - Dedicated capture thread polls AudioImageBuffers.lines_received every
 *     ~1ms (matching the ~1000fps CIS acquisition rate) to capture all frames.
 *   - Timer callback drains the ring and paints all pending frames, giving true
 *     real-time scrolling regardless of the 30fps display rate.
 *   - speed parameter is now a true multiplier: 1.0 = real-time, 0.5 = half, 2.0 = 2×.
 *     It controls how many captured frames are consumed per ring-drain cycle.
 *
 * Thread safety:
 *   - frameRing_ uses atomic write index (capture thread) + non-atomic read
 *     index (message thread only) → lock-free single-producer single-consumer.
 *   - scrollBuffer_ is written only from timerCallback (message thread).
 *   - paint() also runs on message thread → no concurrent access to scrollBuffer_.
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

    // ── Sequence control ──────────────────────────────────────────────────────
    void resetSequence();
    bool isRecording() const noexcept { return seqRecording_; }
    bool isFinished()  const noexcept { return seqFinished_;  }

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

    // ── Waterfall ─────────────────────────────────────────────────────────────
    void allocateScrollBuffer();
    void drainRingAndAdvance(VideoScrollMode mode);

    // ── Row painters ─────────────────────────────────────────────────────────
    // Core painter — takes a RingFrame directly (used by pre-fill + normal scroll)
    void paintRowFromFrame(int target, bool mirror, const RingFrame& fr);
    // Convenience: paints from the last consumed ring slot
    void paintRowFromRing(int target, bool mirror);
    void paintRowFromSeq (int target, bool mirror, int seqIdx) const;

    // ── Sequence helpers ──────────────────────────────────────────────────────
    void appendSeqFrame(const std::vector<uint8_t>& r,
                        const std::vector<uint8_t>& g,
                        const std::vector<uint8_t>& b,
                        const std::vector<uint8_t>& gray);
    void advanceSeqPlayHead(VideoScrollMode mode, bool reverse);

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
    bool                    bufferPreFilled_ { false }; // true after first pre-fill pass

    // Counter from AudioImageBuffers — detect new frames without mutex
    std::atomic<uint64_t>   lastLinesReceived_ { 0 };

    // ── Scroll image (circular, no memcpy) ───────────────────────────────────
    juce::Image scrollBuffer_;
    int bufW_     { 0 };
    int bufH_     { 0 };
    int writeRow_ { 0 };  // current write position (advances circularly)

    // ── Live Dual mode ────────────────────────────────────────────────────────
    bool dualForward_ { true };
    int  dualCounter_ { 0    };

    // ── Sequence mode ─────────────────────────────────────────────────────────
    struct SeqFrame
    {
        std::vector<uint8_t> r, g, b, gray;
    };
    // Max sequence frames: driven by APVTS "videoScrollMaxDuration" at runtime
    static constexpr int kDefaultMaxSeqFrames = 1000;
    int maxSeqFrames_ { kDefaultMaxSeqFrames };

    std::vector<SeqFrame> seqFrames_;
    int  seqPlayHead_  { 0    };
    bool seqPingFwd_   { true };
    bool seqRecording_ { false };
    bool seqFinished_  { false };

    // Previous mode (detect transitions)
    VideoScrollMode prevMode_ { VideoScrollMode::LiveLeftToRight };

    // ── Speed fractional accumulator ──────────────────────────────────────────
    // Accumulated fractional "rows to paint" across timer ticks.
    // speed < 1.0: rowAccumulator_ < 1 most ticks → 0 rows drawn → real slow-down.
    // speed ≥ 1.0: ≥1 row drawn per tick (capped by available).
    float rowAccumulator_ { 0.f };

    // ── Display refresh rate ──────────────────────────────────────────────────
    static constexpr int kTimerFps = 60; // raised from 30 for smoother display

    // ── Owned capture thread ──────────────────────────────────────────────────
    CaptureThread captureThread_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoDisplayComponent)
};
