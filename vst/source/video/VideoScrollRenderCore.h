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
 * class — NO JUCE Component, NO Timer, NO Thread. The mixer drives it on the
 * message thread by calling tick() (drain ring + advance scroll) then
 * renderInto() (blit the warped/aged waterfall into a target Image).
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
 *                           → renderInto() (the SAME paint() draw code into `dest`)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * The capture path differs only in WHERE scanlines come from: instead of a
 * dedicated CaptureThread polling AudioImageBuffers, tick() drains the per-instance
 * lock-free ring (video_scroll_instance(slot)). The scroll/stamp/paint math —
 * compression (distance time-squish) and fade (distance aging) — is copied
 * verbatim from VideoDisplayComponent; both remain tuned display-time effects
 * applied in renderInto().
 *
 * Threading: message thread only. The ring read cursor (cursor_) and the
 * history/ping-pong buffers are owned here and touched only by tick()/renderInto().
 * The producer (synth thread) owns write_index + generation in VideoScrollState.
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

    // (Re)allocate the history/ping-pong buffers for a new display size. Cheap
    // no-op when (w,h) is unchanged, so the mixer may call it every frame.
    void setDisplaySize(int w, int h);

    // Drain the capture ring and advance the scroll by one step. No-op while
    // paused (matches the original: ring is consumed/re-anchored but no scroll)
    // or when the display size is 0.
    void tick();

    // Blit the current waterfall into `dest` (a caller-sized ARGB/RGB Image),
    // applying the display params (compression, fade, zoom, rotation/mode,
    // invert handled at stamp time, colorMode handled at stamp time). `dest`
    // is treated as the full target area; the engine fills it edge-to-edge.
    void renderInto(juce::Image& dest);

    // Paint the waterfall DIRECTLY into an existing Graphics at (0,0)..(destW,destH).
    // Used by the mixer to render a single output straight into the (retina-backed)
    // window/column Graphics — one resample, no offscreen image, no resolution cap,
    // matching the original renderer's crispness. The caller sets up clip/origin.
    // Convenience = buildWarp() + drawWarp(); prefer the split form in the hot path.
    void renderInto(juce::Graphics& g, int destW, int destH);

    // ── Split render (perf): compute the expensive warp ONCE per tick, then blit
    // it cheaply into any number of views. Previously renderInto() did BOTH per
    // call, so the column preview AND the detached window each recomputed the full
    // (window-resolution) warp every frame — doubling a 2560×1440 scalar pass and
    // collapsing the message-thread frame-rate. Now the mixer calls buildWarp()
    // once after tick(), then drawWarp() per destination.
    //   buildWarp() : warp + age the linear history into warpBuf_ (bufW_×compH_).
    //   drawWarp()  : zoom/rotate/scale warpBuf_ into the destination Graphics.
    void buildWarp();
    void drawWarp(juce::Graphics& g, int destW, int destH);

    // Blank both history buffers (transport Stop). May be called by the mixer.
    void clear();

private:
    // ── Ring drain (replaces CaptureThread + buildLineImage's ring access) ────
    // Pulls up to RING_SLOTS fresh scanlines from the per-instance ring into
    // capR_/capG_/capB_ (one row per captured CIS frame). Returns the count
    // actually captured this tick (0 when none). Advances cursor_ to writepos.
    int drainRing(int* outPixelCount);

    // ── Scroll + stamp (the original scrollStep + buildLineImage) ─────────────
    void scrollStep();
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

    // ── Bidirectional history buffers (ping-pong, legacy birth-line model) ────
    juce::Image historyA_, historyB_;
    // Offscreen scratch for renderInto(): linear history warped (time-squish) +
    // aged (fade), before the zoom/orientation transform. Sized bufW_ × compH_.
    juce::Image warpBuf_;
    // Reused paint() scratch (avoid per-frame allocation).
    std::vector<int> warpEdge_;
    std::vector<int> accR_, accG_, accB_;
    std::vector<int> psR_, psG_, psB_;

    int  curBuf_  { 0 };       // after a step: 0 → A freshly drawn, 1 → B
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
    // essentially zero instead of re-running the full per-pixel pass every tick
    // (the cause of the sluggish/flickery feel while paused). scrollStep() sets
    // warpDirty_ whenever it mutates the history; clear()/alloc reset it too.
    bool  warpDirty_   { true };
    float wsLinePos_   { 1e9f };
    float wsCompress_  { 1e9f };
    float wsFade_      { 1e9f };
    int   wsBufW_      { -1 };
    int   wsCompH_     { -1 };

    // Fractional scroll accumulator → smooth sub-pixel scroll speed.
    float scrollAccumulator_ { 0.f };
};
