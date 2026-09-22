/*
 * lux_diff.h
 *
 * LuxDiff (DIFF in the UI) — per-PIXEL reference subtraction as a chain
 * insert: a "dark frame" the user captures on demand.
 *
 * DC BLOCK removes ONE value per line (the line's mean). DIFF removes a whole
 * LINE: on CAPTURE the module averages the next LUX_DIFF_LEARN_LINES lines of
 * its input (per pixel, per channel) and locks that mean as the REFERENCE.
 * From then on every line is compared to the reference pixel by pixel, so
 * whatever the camera saw when the reference was taken vanishes and only the
 * DIFFERENTIAL of what follows reaches the engines.
 *
 * Per pixel, in energy space (background pole = 0), per channel:
 *
 *   d      = e_in[i] - amount * e_ref[i]
 *   ADD:     e_out = max(0, d)      — what APPEARS toward the material pole
 *   ABS:     e_out = |d|            — what appears AND what vanishes
 *
 * The reference is stored RAW (pixel values); the polarity is applied at use,
 * so a chain background change never invalidates it. A reference taken at a
 * different pixel count is resampled nearest-neighbour (the chain width is
 * constant in practice — this is only a guard).
 *
 * FOLLOW — HOLD keeps the captured reference until the next CAPTURE / CLEAR.
 * TRACK makes the reference FOLLOW the stream per pixel with a first-order
 * lag of `track_lines` lines (in LINES, like ECHO's delay — the line rate is
 * a device property): whatever stays still fades out of the differential in
 * a few time constants, only motion remains — the true per-pixel DC blocker.
 * The tracker is seeded from the armed reference (or from the first line
 * when there is none); CAPTURE re-seeds it from a fresh mean, CLEAR from the
 * next line. It runs on a 16.16 fixed-point copy so the slowest lag still
 * moves on one-level differences. In TRACK the reference changes every line
 * and is persisted as a mere seed (no per-line generation bump).
 *
 * Threads:
 *   • process_frame runs on the image thread — it consumes the CAPTURE
 *     request, accumulates the learning window and publishes the reference
 *     under a seqlock (`ref_gen`, odd = being written).
 *   • request_capture / clear_reference may be called from ANY thread (UI,
 *     MIDI): single int stores, no buffer writes.
 *   • set_reference (session restore) and copy_reference (session save) run
 *     on the message thread; copy_reference retries on a torn read.
 *   Learning also runs while the module is BYPASSED (enabled = 0): the user
 *   can capture the scene with the block off and switch it on afterwards.
 *
 * Memory: per instance ≈ 200 KB (reference + accumulators + tracker + out).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-09-04
 */

#ifndef LUX_DIFF_H
#define LUX_DIFF_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxDrive (>6912 for 400 DPI CIS). */
#define LUX_DIFF_MAX_PIXELS  8192

/* Background mode — which pole is the "material" (mirrors LUX_EQ_BG_*). */
#define LUX_DIFF_BG_BLACK  0   /* bright material on black background */
#define LUX_DIFF_BG_WHITE  1   /* dark material on white background   */
#define LUX_DIFF_BG_AUTO   2   /* detect from the stream (default)    */

/* Output mode. */
#define LUX_DIFF_MODE_ADD  0   /* keep what appears toward the material pole */
#define LUX_DIFF_MODE_ABS  1   /* keep |in - ref| — appears AND vanishes     */

/* Lines averaged into the reference after a CAPTURE request. */
#define LUX_DIFF_LEARN_LINES  128

/* Follow mode. */
#define LUX_DIFF_FOLLOW_HOLD   0   /* reference frozen until CAPTURE / CLEAR   */
#define LUX_DIFF_FOLLOW_TRACK  1   /* reference lags the stream (track_lines) */

/* TRACK lag bounds (lines). */
#define LUX_DIFF_TRACK_MIN_LINES  4
#define LUX_DIFF_TRACK_MAX_LINES  16384

/* UI guide profile resolution — matches the editor's view width. */
#define LUX_DIFF_UI_BINS   128

/* ============================================================================
 * LuxDiffConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float amount;           /* 0..1 — fraction of the reference subtracted */
    int   mode;             /* LUX_DIFF_MODE_* */
    int   follow;           /* LUX_DIFF_FOLLOW_* */
    int   track_lines;      /* TRACK lag — time constant in lines */
    int   background_mode;  /* LUX_DIFF_BG_* (chain-owned, see applyChainBackgrounds) */
} LuxDiffConfig;

/* ============================================================================
 * LuxDiffState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxDiffConfig config;

    int  diff_active;    /* latch: a stream was processed at least once since
                          * the last reset — NOT an activity indicator */

    /* Rack-LED heartbeat: bumped once per line the module actually CHANGED
     * (output != input) — see lux_reverb.h. */
    uint32_t active_ticks;

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE). */
    int  auto_bg_white;
    int  auto_locked;
    int  auto_lock_countdown;
    int  auto_max_mean;
    int  auto_min_mean;

    /* ── Requests (any thread → image thread) ────────────────────────────── */
    volatile int req_capture;   /* 1 = start a learning window on the next line */

    /* ── Learning window (image thread only) ─────────────────────────────── */
    int      learning;          /* nonzero while accumulating */
    int      learn_left;        /* lines still to accumulate */
    int      learn_px;          /* pixel count of the window (restart on change) */
    uint16_t acc_r[LUX_DIFF_MAX_PIXELS];   /* 128 × 255 < 65535 */
    uint16_t acc_g[LUX_DIFF_MAX_PIXELS];
    uint16_t acc_b[LUX_DIFF_MAX_PIXELS];

    /* ── Reference (raw pixel values, polarity applied at use) ───────────── */
    uint8_t  ref_r[LUX_DIFF_MAX_PIXELS];
    uint8_t  ref_g[LUX_DIFF_MAX_PIXELS];
    uint8_t  ref_b[LUX_DIFF_MAX_PIXELS];
    int      ref_px;                 /* pixel count of the reference (0 = none) */
    volatile int      ref_valid;     /* a reference is armed */
    volatile uint32_t ref_gen;       /* seqlock — odd while a writer is inside;
                                      * bumped on capture / clear / restore /
                                      * TRACK seed — NOT per tracked line.
                                      * The host watches it to persist. */

    /* ── TRACK follower (image thread only) — 16.16 fixed-point mirror of the
     * reference, so a lag of thousands of lines still moves on a one-level
     * difference. Re-seeded (trk_valid = 0) by capture / clear / restore. */
    uint32_t trk_r[LUX_DIFF_MAX_PIXELS];
    uint32_t trk_g[LUX_DIFF_MAX_PIXELS];
    uint32_t trk_b[LUX_DIFF_MAX_PIXELS];
    int      trk_valid;
    int      trk_px;
    uint32_t trk_tick;               /* lines tracked — paces the UI guide refresh */

    /* UI guide — live profile of the input energy (0..1), downsampled to
     * LUX_DIFF_UI_BINS bins and refreshed EVERY line as two layers:
     *   ui_in_now  — fast release: the stream as it breathes (the lows),
     *   ui_in_peak — slow release: rémanence of the recent maxima (the highs).
     * The editor reads both at its repaint rate — no publication window. */
    float ui_in_now [LUX_DIFF_UI_BINS];
    float ui_in_peak[LUX_DIFF_UI_BINS];
    int   ui_in_valid;      /* a stream line was captured at least once */

    /* UI guide — the REFERENCE profile (per-bin max of its luminance energy,
     * 0..1), rebuilt whenever the reference or the resolved polarity
     * changes (`ui_ref_bg` remembers which pole it was built for). */
    float ui_ref[LUX_DIFF_UI_BINS];
    int   ui_ref_bg;

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_DIFF_MAX_PIXELS];
    uint8_t out_g[LUX_DIFF_MAX_PIXELS];
    uint8_t out_b[LUX_DIFF_MAX_PIXELS];
} LuxDiffState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void          lux_diff_init(LuxDiffState *state);
/* AUTO re-armed, learning aborted, config untouched — the REFERENCE SURVIVES
 * (a bypass/enable toggle must not lose the user's capture). */
void          lux_diff_reset(LuxDiffState *state);
LuxDiffConfig lux_diff_config_default(void);

/* ── Reference control ─────────────────────────────────────────────────────── */
/* Ask the image thread to (re)learn the reference from the next lines.
 * Any thread. */
void lux_diff_request_capture(LuxDiffState *state);
/* Drop the reference immediately (also aborts a learning window on the next
 * line). Any thread — int stores only. */
void lux_diff_clear_reference(LuxDiffState *state);
/* Session save: copy the reference (planar R,G,B — `cap` pixels max each).
 * Retries a torn read. Returns the pixel count, 0 when no reference is armed.
 * Message thread. */
int  lux_diff_copy_reference(const LuxDiffState *state,
                             uint8_t *r, uint8_t *g, uint8_t *b, int cap);
/* Session restore: install a reference (planar R,G,B, `px` pixels).
 * Message thread. */
void lux_diff_set_reference(LuxDiffState *state,
                            const uint8_t *r, const uint8_t *g,
                            const uint8_t *b, int px);
/* Learning progress 0..1 for the UI (0 when idle). */
float lux_diff_learn_progress(const LuxDiffState *state);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled, no reference is armed or amount is 0, the input
 * pointers are returned as-is (the learning window still runs).
 */
void lux_diff_process_frame(
    LuxDiffState  *state,
    const uint8_t *in_r,
    const uint8_t *in_g,
    const uint8_t *in_b,
    int            pixel_count,
    int            luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxEq/LuxDrive) ─────────────── */
extern LuxDiffState g_lux_diff_proc;
LuxDiffState *lux_diff_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void          lux_diff_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_DIFF_H */
