#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "VideoScrollMode.h"
#include <vector>
#include <atomic>
#include <cstdint>

class Sp3ctraAudioProcessor;

/**
 * @brief Full-resolution scrolling waterfall display for CIS image data.
 *
 * Renders incoming CIS frames as a 2D waterfall:
 *   - X axis = CIS pixel position (0 .. CIS_MAX_PIXELS_NB)
 *   - Y axis = time  (most recent row at bottom, oldest at top)
 *
 * Each timer tick a new CIS line is painted at the bottom of the waterfall
 * and the existing content is scrolled up.
 *
 * VideoScrollMode controls the pixel direction + replay behaviour:
 *   LiveLeftToRight — pixels drawn 0→N (normal)
 *   LiveRightToLeft — pixels drawn N→0 (mirrored)
 *   LiveDual        — alternates L→R / R→L every half-buffer (ping-pong)
 *   SeqLoopSimple   — records N frames then loops A→B→A→B
 *   SeqLoopPingPong — records N frames then bounces A→B→A
 *   SeqOneShot      — records N frames, plays once, freezes
 *
 * Scroll speed is read from APVTS "videoScrollSpeed" (rows per second factor).
 * Brightness and zoom are read from "videoScrollBrightness" / "videoScrollZoom".
 * Color mode is toggled by "videoColorMode" (false = grayscale, true = RGB).
 * Invert is toggled by "videoInvertColor".
 *
 * Double-click on the component triggers onFullscreenRequested (if set).
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

    // ── Callback: called on double-click so VideoWindow can toggle fullscreen ─
    std::function<void()> onFullscreenRequested;

    // ── Sequence control ──────────────────────────────────────────────────────
    /** Reset the sequence buffer (used when mode changes to a Seq* mode). */
    void resetSequence();

    /** Returns true while a Seq* mode is in recording phase. */
    bool isRecording() const noexcept { return seqRecording_; }

    /** Returns true when a SeqOneShot has finished playing. */
    bool isFinished() const noexcept { return seqFinished_; }

private:
    // ── Timer ─────────────────────────────────────────────────────────────────
    void timerCallback() override;

    // ── CIS data reading (UI thread only) ─────────────────────────────────────
    void readCisData();

    // ── Waterfall helpers ─────────────────────────────────────────────────────
    void allocateScrollBuffer();
    void advanceWaterfall(VideoScrollMode mode);
    void paintRowFromLive(int rowY, bool mirror) const;
    void paintRowFromSeq(int rowY, bool mirror) const;

    // ── Sequence playback helpers ─────────────────────────────────────────────
    void appendSeqFrame();
    void advanceSeqPlayHead(VideoScrollMode mode, bool reverse = false);

    // ── Processor reference ───────────────────────────────────────────────────
    Sp3ctraAudioProcessor& processor_;

    // ── Waterfall image buffer ────────────────────────────────────────────────
    juce::Image scrollBuffer_;
    int bufW_  { 0 };
    int bufH_  { 0 };

    // ── CIS local buffers (UI thread only) ────────────────────────────────────
    std::vector<uint8_t> cisR_, cisG_, cisB_, cisGray_;
    int cisCount_ { 0 };

    // ── Scroll accumulator ────────────────────────────────────────────────────
    float scrollAccumulator_ { 0.0f };

    // ── Live Dual mode state ──────────────────────────────────────────────────
    bool  dualForward_ { true  };
    int   dualCounter_ { 0     };  ///< rows since last direction flip

    // ── Sequence playback ─────────────────────────────────────────────────────
    struct SequenceFrame
    {
        std::vector<uint8_t> r, g, b, gray;
    };
    static constexpr int kMaxSeqFrames = 1000;

    std::vector<SequenceFrame> seqFrames_;
    int   seqPlayHead_   { 0    };
    bool  seqPingFwd_    { true };  ///< current direction for PingPong mode
    bool  seqRecording_  { false }; ///< true while filling seqFrames_
    bool  seqFinished_   { false }; ///< true after OneShot has played

    // ── Previous mode (detect mode switch) ───────────────────────────────────
    VideoScrollMode prevMode_ { VideoScrollMode::LiveLeftToRight };

    // ── Timer FPS ─────────────────────────────────────────────────────────────
    static constexpr int kTimerFps = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoDisplayComponent)
};
