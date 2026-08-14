/*
 * lux_dcblock.h
 *
 * LuxDcBlock (DC BLOCK in the UI) — per-line mean removal (AC extraction) as a
 * chain insert.
 *
 * The OUT sends already offer "DC Blocking" as a binary conditioning toggle
 * (img_stage_remove_dc — subtract the line's mean, clamp): it removes the
 * continuous component of the stream so only the contrast INSIDE each line
 * reaches the engine. This module is the same operation promoted to a rack
 * insert: placeable anywhere in the chain, per-chain instance, and DOSED —
 * `amount` subtracts a fraction of the mean instead of all of it.
 *
 * Per line, in energy space (background pole = 0):
 *
 *   mean_c = Σ e_c[i] / N            (per CHANNEL — a uniform colour wash is
 *                                     that channel's DC and vanishes with it)
 *   e_out  = clamp(e_c[i] - amount * mean_c)
 *
 * The subtraction only ever pushes energy TOWARD the background, so the paper
 * (energy ≈ 0) passes bit-identical — no floor tracker needed: the tracked
 * paper pedestal IS part of the continuous component being removed. A fully
 * uniform line (all DC) collapses to background — the classic DC-blocker
 * contract (constant input → silence).
 *
 * Energy space: `background_mode` picks which pole carries the material
 * (mirrors LuxEq/LuxDrive — the chain inserts see the RAW image, upstream of
 * the synth's Negative).
 *
 * Memory: per instance ≈ 25 KB (RGB out buffers + UI guide) — no history.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-08-04
 */

#ifndef LUX_DCBLOCK_H
#define LUX_DCBLOCK_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxDrive (>6912 for 400 DPI CIS). */
#define LUX_DCBLOCK_MAX_PIXELS  8192

/* Background mode — which pole is the "material" (mirrors LUX_EQ_BG_*). */
#define LUX_DCBLOCK_BG_BLACK  0   /* bright material on black background */
#define LUX_DCBLOCK_BG_WHITE  1   /* dark material on white background   */
#define LUX_DCBLOCK_BG_AUTO   2   /* detect from the stream (default)    */

/* UI guide profile resolution — matches the editor's view width. */
#define LUX_DCBLOCK_UI_BINS   128

/* ============================================================================
 * LuxDcBlockConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float amount;           /* 0..1 — fraction of the per-line mean removed
                             * (1 = the OUT sends' full DC Blocking) */
    int   background_mode;  /* LUX_DCBLOCK_BG_* */
} LuxDcBlockConfig;

/* ============================================================================
 * LuxDcBlockState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxDcBlockConfig config;

    int  dc_active;      /* latch: a stream was processed at least once since
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

    /* UI guide — live profile of the input energy (0..1), downsampled to
     * LUX_DCBLOCK_UI_BINS bins and refreshed EVERY line as two layers:
     *   ui_in_now  — fast release: the stream as it breathes (the lows),
     *   ui_in_peak — slow release: rémanence of the recent maxima (the highs).
     * The editor reads both at its repaint rate — no publication window. */
    float ui_in_now [LUX_DCBLOCK_UI_BINS];
    float ui_in_peak[LUX_DCBLOCK_UI_BINS];
    int   ui_in_valid;      /* a stream line was captured at least once */

    /* UI guide — smoothed LUMINANCE mean of the line (0..1): the DC level the
     * editor draws its reference line at. EMA (1/16, every line). */
    float ui_dc;

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_DCBLOCK_MAX_PIXELS];
    uint8_t out_g[LUX_DCBLOCK_MAX_PIXELS];
    uint8_t out_b[LUX_DCBLOCK_MAX_PIXELS];
} LuxDcBlockState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void             lux_dcblock_init(LuxDcBlockState *state);
void             lux_dcblock_reset(LuxDcBlockState *state);   /* AUTO re-armed, config untouched */
LuxDcBlockConfig lux_dcblock_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or amount is 0) the input pointers are returned
 * as-is (O(1) pass-through after the one-shot lazy re-arm).
 */
void lux_dcblock_process_frame(
    LuxDcBlockState *state,
    const uint8_t   *in_r,
    const uint8_t   *in_g,
    const uint8_t   *in_b,
    int              pixel_count,
    int              luxstral_num_octaves,
    const uint8_t  **out_r,
    const uint8_t  **out_g,
    const uint8_t  **out_b);

/* ── Global instance + per-chain pool (mirrors LuxEq/LuxDrive) ─────────────── */
extern LuxDcBlockState g_lux_dcblock_proc;
LuxDcBlockState *lux_dcblock_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void             lux_dcblock_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_DCBLOCK_H */
