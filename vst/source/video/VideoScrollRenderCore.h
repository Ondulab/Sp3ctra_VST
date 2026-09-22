#pragma once

#include <juce_graphics/juce_graphics.h>
#include <atomic>
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

    // ── Live-stream envelopes — the VIDEO MIX toile's flow pulse ──────────────
    // "Is this output receiving INK right now?" Fresh CIS lines drained this
    // tick, weighted by their darkness (white = the silent pole of the Sp3ctra
    // stream: the unfed white-line sweep and blank paper read as silence) and
    // by how much of the nominal ~1000 lps arrived, so a trickle BLINKS and a
    // full stream holds. now = fast release (the LED), peak = slow release
    // (its rémanence — the ui_in_now / ui_in_peak pair of the chain modules),
    // power = the same drive under a SYMMETRIC smoothing: the honest reading
    // to print beside the output ("how hard is this tap being driven"), which
    // a peak-hold envelope would overstate.
    // Written by tick() on the render thread, readable from any thread.
    float flowNow()   const noexcept { return flowNow_.load  (std::memory_order_relaxed); }
    float flowPeak()  const noexcept { return flowPeak_.load (std::memory_order_relaxed); }
    float flowPower() const noexcept { return flowPower_.load(std::memory_order_relaxed); }

    // ── Split render (perf): compute the expensive warp ONCE per tick, then blit
    // it cheaply into any number of views.
    //   buildWarp() : warp + age the linear history into warpBuf_ (bufW_×compH_).
    //                 Returns true when warpBuf_ was rebuilt (false = cache hit,
    //                 the previous warp is still current → no repaint needed).
    //   drawWarp()  : zoom/rotate/scale warpBuf_ into the destination Graphics.
    bool buildWarp();
    void drawWarp(juce::Graphics& g, int destW, int destH);
    /** Fast path — the SAME picture straight into a 4-byte ARGB image (the
     *  mixer's render targets). Replaces juce::Graphics' generic software
     *  rasteriser with a parallel fixed-point affine blit that paints its own
     *  border, so the separate full-surface fill disappears too. Falls back to
     *  the Graphics path if `dest` is not ARGB. */
    void drawWarp(juce::Image& dest);

    // Blank the history buffer (transport Stop). May be called by the mixer's
    // render thread only (same single-consumer discipline as tick()).
    void clear();

    // The display colour law itself (pure, static) — shared with the page's
    // VIEWPORT pad so its "no frame yet" paper matches the output's frame
    // colour. See the private colour-law section below for the members.
    static void applyDisplayColour(int& r, int& g, int& b, bool colorMode, int invMode);

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

    // ── History ring addressing ───────────────────────────────────────────────
    // Logical history row (0 = far past above the birth line, bufH_ = far past
    // below it) → the physical row of history_ that currently holds it.
    int  histRow(int y) const noexcept;
    // Roll the rings back to identity (offsets 0, physical order == logical
    // order). Needed whenever the split moves — the birth line is a parameter,
    // so this runs on a user gesture, not every frame — and before any resize.
    void linearizeHistory();
    // Make scratch_ hold `chunks` entries sized for the current bufW_.
    void ensureScratch(int chunks);

    // The zoom / centre / rotation transform shared by both drawWarp paths:
    // warpBuf_ pixel space → destination pixel space.
    juce::AffineTransform warpTransform(int destW, int destH) const;

    // APVTS convenience: load a slot-scoped raw param with a null-guarded default.
    float param(const char* suffix, float defaultValue) const;

    // ── Output geometry (zoom / centre / birth line) ──────────────────────────
    // The canvas is ALWAYS the whole output window (the sweep spans it edge to
    // edge). Zoom = width of the generation band on the transverse axis, applied
    // rigidly at draw time; centerX/centerY (window space) move the band and
    // the zoom frame hosting the birth line. docs/PLAN_VIDEO_SCROLL_CHAIN_PAGES_ZOOM.md
    float zoomParam() const;     // clamped to VideoScrollLimits
    float rotationRad() const;   // "rotation" (degrees, clockwise) → radians
    // Bounding box of the ROTATED view inside the square canvas (canvas px):
    // the extent the sweep must cover transversally (sx) and along the scroll
    // axis (sy). Equals the view dims at 0°/180°, swapped at 90°/270°. Used
    // for VISIBILITY only (warp/blur culling, birth-line clamp) — never to
    // size anything, or the picture would breathe with the angle.
    void  visibleSpans(float& sx, float& sy) const;
    // The zoom FRAME (what Zoom scales), rotation-INVARIANT: the view's own
    // dims (W transverse, H along the scroll axis, canvas px), like a camera
    // viewfinder turning over the scene. At zoom 1 the stamped line spans W
    // px whatever the angle — rotating never changes the apparent zoom (it
    // used to follow the bounding box, +41 % at 45° on a square view).
    void  frameSpans(float& fx, float& fy) const;
    // centerX/centerY (window space) → offset of the generation centre in the
    // canvas frame (canvas px): transverse (ox) and along the scroll axis (oy).
    void  centreOffset(float& ox, float& oy) const;
    // Birth-line position along the scroll axis, 0..1 of the canvas: the zoom
    // frame (z × sy, centred by Center Y) hosts the line via Line Pos; clamped
    // to the visible span. Shared by scrollStep() and buildWarp() so both agree.
    float birthLine01() const;

    // ── Display colour law (ONE path for everything that reaches the screen) ──
    // Color(RGB) off folds to luma, then Invert (Off / Negative / Luminance)
    // flips it. buildLineImage applies it to the stamped lines; the blank paper
    // (clear / allocate / vacated rows) and the frame border (drawWarp) go
    // through the SAME law, so a Luminance-inverted white paper shows a black
    // paper AND a black border — never a white frame around a black image.
    int  invertMode() const;   // 0 Off / 1 Negative / 2 Luminance (legacy bool folded in)
    juce::Colour displayColourOf(float r01, float g01, float b01) const;
    juce::Colour paperColour() const { return displayColourOf(1.f, 1.f, 1.f); }
    juce::Colour frameColour() const;   // bgR/bgG/bgB through the law
    void fillPaperRow(uint8_t* row, int pixelStride, juce::Colour paper) const;

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

    // ── Flow envelopes (see flowNow / flowPeak) ──────────────────────────────
    void noteFlow(int freshLines, double dtMs);
    static constexpr float kNominalLps = 1000.0f;   // full-fidelity CIS rate
    static constexpr float kInkFloor   = 0.03f;     // below: blank paper / noise
    static constexpr float kInkFull    = 0.12f;     // ink (above the floor) lighting the LED fully
    static constexpr float kFlowNowMs  = 90.0f;     // LED release
    static constexpr float kFlowPeakMs = 700.0f;    // rémanence release
    static constexpr float kFlowPwrMs  = 250.0f;    // printed value, both ways
    std::atomic<float> flowNow_   { 0.0f };
    std::atomic<float> flowPeak_  { 0.0f };
    std::atomic<float> flowPower_ { 0.0f };

    // ── History buffer (TWO RINGS, split at the birth line) ──────────────────
    // The waterfall pushes content AWAY from the birth line in both directions,
    // so the history is two back-to-back waterfalls: physical rows [0, birth)
    // hold the upper zone, [birth, bufH_) the lower one. Each is addressed
    // through its own rotating offset (histRow), so a scroll of s px costs s
    // rows of writes instead of shifting the whole 4×canvas buffer every tick —
    // at record resolutions that shift alone moved ~100 MB per frame per
    // output. The ring layout is rebuilt (linearizeHistory) only when the birth
    // line itself moves, which is a user gesture, not a per-frame event.
    juce::Image history_;
    int  upOff_     { 0 };    // rotation of the upper ring, in [0, ringBirth_)
    int  loOff_     { 0 };    // rotation of the lower ring, in [0, bufH_-ringBirth_)
    int  ringBirth_ { -1 };   // the split the current rotations are expressed in
    // Offscreen scratch: linear history warped (time-squish) + aged (fade),
    // before the zoom/orientation transform. Sized bufW_ × compH_.
    juce::Image warpBuf_;
    // Reused scratch (avoid per-frame allocation). The row accumulators and the
    // blur prefix sums are PER PARALLEL CHUNK: buildWarp fans its row range out
    // over videoparallel::parallelChunks, and two worker threads must never
    // share a row buffer.
    std::vector<int> warpEdge_;
    struct RowScratch { std::vector<int> accR, accG, accB, psR, psG, psB; };
    std::vector<RowScratch> scratch_;

    // View dims (budgeted logical px) the canvas was sized for. The canvas is
    // a SQUARE on their diagonal (compW_ = compH_ = ceil(hypot(viewW_, viewH_)))
    // so any rotation sweeps the whole view without ever reallocating.
    int  viewW_   { 0 };
    int  viewH_   { 0 };
    int  compW_   { 0 };       // canvas width  (= D)
    int  compH_   { 0 };       // canvas height (= D)
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
    float wsBirth_     { 1e9f };   // resolved birth line (linePos/zoom/centerY/rotation)
    float wsRot_       { 1e9f };   // rotation → visible span (aging distances)
    int   wsViewW_     { -1 };
    int   wsViewH_     { -1 };
    float wsCompress_  { 1e9f };   // bipolar "pack"
    float wsFade_      { 1e9f };
    float wsMidX_      { 1e9f };   // the attenuation curve's free middle point
    float wsMidY_      { 1e9f };
    float wsMidFree_   { 1e9f };
    float wsBlur_      { 1e9f };
    float wsGamma_     { 1e9f };
    int   wsBufW_      { -1 };
    int   wsCompH_     { -1 };

    // Fractional scroll accumulator → smooth sub-pixel scroll speed.
    float scrollAccumulator_ { 0.f };
};
