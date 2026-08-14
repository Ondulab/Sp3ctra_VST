#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <cstdint>

class Sp3ctraAudioProcessor;

/**
 * @brief Per-instance "birth-line" waterfall engine for one VideoScroll output slot.
 *
 * Extracted from VideoDisplayComponent (the original global SFML-style renderer)
 * so a mixer can own many engines and composite their outputs. This is a plain
 * class — NO JUCE Component, NO Timer, NO Thread. The mixer's render thread
 * drives it by calling tick() (drain ring + advance scroll) then buildWarp() /
 * drawWarp() (blit the warped/aged waterfall into a target).
 *
 * Architecture (vs. the original):
 * ─────────────────────────────────────────────────────────────────────────────
 *   ORIGINAL: CaptureThread → frameRing_ (SPSC of scanlines)
 *                           → timerCallback() → scrollStep() → buildLineImage()
 *                           → paint() (compression/fade/zoom/rotate/invert/color)
 *
 *   HERE:     synth thread  → video_scroll_capture_line() → VideoScrollState ring
 *                           → tick()  (drains the ring via video_scroll_ring_*,
 *                                       runs the SAME scroll/stamp logic)
 *                           → buildWarp()+drawWarp() (the SAME paint() draw code)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * The capture path differs only in WHERE scanlines come from: instead of a
 * dedicated CaptureThread polling AudioImageBuffers, tick() drains the per-instance
 * lock-free ring (video_scroll_instance(slot)). The scroll/stamp/paint math —
 * compression (distance time-squish) and fade (distance aging) — is copied
 * verbatim from VideoDisplayComponent; both remain tuned display-time effects.
 *
 * Threading: SINGLE consumer thread (the mixer's render thread — historically
 * the message thread). The ring read cursor (cursor_) and the history buffer
 * are owned here and touched only by tick()/buildWarp()/drawWarp(). The producer
 * (synth thread) owns write_index + generation in VideoScrollState. APVTS params
 * are read through atomic raw-value pointers, safe from any thread.
 */
class VideoScrollRenderCore
{
public:
    VideoScrollRenderCore(Sp3ctraAudioProcessor& proc, int slot);
    ~VideoScrollRenderCore();

    // Re-point this engine at a different capture slot. Re-anchors the ring
    // cursor and generation snapshot, and clears history (discontinuity).
    void setSlot(int slot);
    int  slot() const noexcept { return slot_; }

    // (Re)allocate the history buffer for a new display size. Cheap no-op when
    // (w,h) is unchanged, so the mixer may call it every frame. On a real size
    // change the previous history is RESCALED into the new buffer (not blanked),
    // so opening/resizing the master window no longer flashes the waterfall black.
    void setDisplaySize(int w, int h);

    // Drain the capture ring and advance the scroll. `nowMs`/`dtMs` come from the
    // caller's frame clock (juce::Time::getMillisecondCounterHiRes): the scroll
    // advance is TIME-based (px/s), not per-call, so an irregular tick cadence
    // no longer distorts the time axis. No-op while paused (ring is consumed /
    // re-anchored but no scroll) or when the display size is 0. Returns true when
    // the history was mutated (scrolled/stamped/cleared) — false means the frozen
    // image is untouched and the caller may skip repainting its views this tick.
    bool tick(double nowMs, double dtMs);

    // ── Split render (perf): compute the expensive warp ONCE per tick, then blit
    // it cheaply into any number of views.
    //   buildWarp() : warp + age the linear history into warpBuf_ (bufW_×compH_).
    //                 Returns true when warpBuf_ was rebuilt (false = cache hit,
    //                 the previous warp is still current → no repaint needed).
    //   drawWarp()  : zoom/rotate/scale warpBuf_ into the destination Graphics.
    bool buildWarp();
    void drawWarp(juce::Graphics& g, int destW, int destH);

    // Blank the history buffer (transport Stop). May be called by the mixer's
    // render thread only (same single-consumer discipline as tick()).
    void clear();

private:
    // ── Ring drain (replaces CaptureThread + buildLineImage's ring access) ────
    // Pulls up to RING_SLOTS fresh scanlines from the per-instance ring into
    // capR_/capG_/capB_ (one row per captured CIS frame). Returns the count
    // actually captured this tick (0 when none). Advances cursor_ to writepos.
    int drainRing(int* outPixelCount);

    // ── Scroll + stamp (the original scrollStep + buildLineImage) ─────────────
    // scrollStep returns true when it mutated the history (see tick()).
    bool scrollStep(double nowMs, double dtMs);
    bool buildLineImage(juce::Image& out, int coreH, int bandH,
                        int captured, int captureCount);

    // ── Helpers (the original allocateScrollBuffer / clearHistory) ────────────
    void allocateScrollBuffer(int w, int h);

    // APVTS convenience: load a slot-scoped raw param with a null-guarded default.
    float param(const char* suffix, float defaultValue) const;

    // ── Processor reference (for APVTS + ring instance lookup) ────────────────
    Sp3ctraAudioProcessor& processor_;
    int slot_ { 0 };

    // ── Per-instance capture ring drain state ─────────────────────────────────
    // cursor_ is an ABSOLUTE total-pushes value (video_scroll_ring_writepos
    // semantics). lastGen_ tracks the producer-owned generation: when it changes
    // we clear() (discontinuity), mirroring the original's clear-pulse handling.
    uint32_t cursor_  { 0 };
    uint32_t lastGen_ { 0 };
    bool     genInit_ { false };

    // Captured scanlines drained this tick (preallocated; sized to RING_SLOTS in
    // ctor). One row per captured CIS frame, fed into buildLineImage exactly as
    // the original's frameRing_ window was.
    std::vector<std::vector<uint8_t>> capR_, capG_, capB_;
    std::vector<int>                  capPx_;
    // Scratch copy buffers for video_scroll_ring_get (each VIDEO_SCROLL_MAX_PIXELS).
    std::vector<uint8_t> tmpR_, tmpG_, tmpB_;

    // ── Starvation bridge ("hold last line") ──────────────────────────────────
    // The producer is bursty (UDP packets deliver several lines at once) while
    // the scroll advances continuously: a tick that drains ZERO fresh lines but
    // still scrolls used to stamp nothing, leaving a black 2×scroll gap — the
    // "bandes noires". When a tick is starved we re-stamp the newest previously
    // captured line for up to kHoldMs; past that the source is genuinely stopped
    // and the honest black gap returns (no-signal contract).
    static constexpr double kHoldMs = 250.0;
    std::vector<uint8_t> heldR_, heldG_, heldB_;
    int    heldPx_   { 0 };
    double heldAtMs_ { -1.0e12 };

    // ── History buffer (single, scrolled IN PLACE — legacy birth-line model) ──
    // The original kept two ping-pong images and re-blitted the whole W×4H
    // history through juce::Graphics every tick (plus a full black fill). The
    // in-place row-move version does the same shift with row memcpys on one
    // buffer: ~3-4× less memory traffic per tick and half the resident memory.
    juce::Image history_;
    // Offscreen scratch: linear history warped (time-squish) + aged (fade),
    // before the zoom/orientation transform. Sized bufW_ × compH_.
    juce::Image warpBuf_;
    // Reused scratch (avoid per-frame allocation).
    std::vector<int> warpEdge_;
    std::vector<int> accR_, accG_, accB_;
    std::vector<int> psR_, psG_, psB_;

    int  compW_   { 0 };       // viewport width
    int  compH_   { 0 };       // viewport height
    int  bufW_    { 0 };       // history width  (= compW_)
    int  bufH_    { 0 };       // history height (= 4 × compH_)
    bool buffersInit_ { false };

    // True once buildWarp() has produced a valid warpBuf_ this session; drawWarp()
    // paints black until then (and whenever there is no history to warp).
    bool warpReady_ { false };

    // ── Warp cache ────────────────────────────────────────────────────────────
    // buildWarp() is skipped when nothing that affects warpBuf_ changed since the
    // last build: the history is frozen (warpDirty_ stays false while paused) AND
    // the warp-shaping params are identical. This makes a paused waterfall cost
    // essentially zero instead of re-running the full per-pixel pass every tick.
    // scrollStep() sets warpDirty_ whenever it mutates the history; clear()/alloc
    // reset it too.
    bool  warpDirty_   { true };
    float wsLinePos_   { 1e9f };
    float wsCompress_  { 1e9f };
    float wsFade_      { 1e9f };
    float wsGamma_     { 1e9f };
    int   wsBufW_      { -1 };
    int   wsCompH_     { -1 };

    // Fractional scroll accumulator → smooth sub-pixel scroll speed.
    float scrollAccumulator_ { 0.f };
};
