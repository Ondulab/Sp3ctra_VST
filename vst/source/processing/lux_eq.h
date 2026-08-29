/*
 * lux_eq.h
 *
 * LuxEq — typed-handle equalizer on the image-line stream.
 *
 * The pixel axis IS the instrument's frequency axis (log-mapped, pixel 0 =
 * low_frequency, last pixel = high_frequency — see wave_generation.c). The
 * curve is a stack of up to SHAPE_EQ_MAX_HANDLES typed handles (Bell / LP /
 * HP / DJ / Tilt — see shape_eq.h), evaluated per pixel in dB by shape_eq_db
 * (shared with the UI editor, so the drawn curve IS the applied gain).
 *
 * Gain applies to the MATERIAL energy only (input minus the tracked PAPER
 * level — 10th-percentile estimator, see lux_drive.c): boosting must re-print
 * the strokes, never brighten the paper. The pixel's OWN pedestal is kept, so
 * background pixels pass through bit-identical. Energy space: `background_mode`
 * picks which pole carries the material (mirrors LuxPitch/LuxMask/LuxReverb/
 * LuxEcho — the chain inserts see the RAW image, upstream of the synth's
 * Negative).
 *
 *   m     = max(0, e_in - floor)
 *   e_out = (e_in - m) + 10^(dB(x)/20) * m              (clamped to 0..255)
 *
 * Memory: per instance ≈ 56 KB (float LUT + RGB out buffers) — no history.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N). The gain LUT is rebuilt
 *            only when a band value (or the pixel count) changes.
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-07-05
 */

#ifndef LUX_EQ_H
#define LUX_EQ_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */
#include "shape_eq.h"     /* ShapeEqHandle + shared curve evaluator */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_EQ_MAX_PIXELS  8192

/* Background mode — which pole is the "material" (mirrors LUX_ECHO_BG_*). */
#define LUX_EQ_BG_BLACK  0   /* bright material on black background */
#define LUX_EQ_BG_WHITE  1   /* dark material on white background   */
#define LUX_EQ_BG_AUTO   2   /* detect from the stream (default)    */

/* ============================================================================
 * LuxEqConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int           enabled;
    int           background_mode;                  /* LUX_EQ_BG_* */
    float         level_db;                         /* whole-curve gain fader */
    ShapeEqHandle handles[SHAPE_EQ_MAX_HANDLES];    /* typed curve handles */
} LuxEqConfig;

/* ============================================================================
 * LuxEqState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxEqConfig config;

    int  eq_active;      /* latch: a stream was shaped at least once since the
                          * last reset — NOT an activity indicator */

    /* Rack-LED heartbeat: bumped once per line the module actually CHANGED
     * (output != input). Blank paper carries no material to shape, so the
     * curve leaves it intact and the LED correctly falls back to idle.
     * See lux_reverb.h. */
    uint32_t active_ticks;
    int  last_bg_mode;   /* RESOLVED polarity the floor was learned in */

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE). */
    int  auto_bg_white;  /* current AUTO verdict (init: white/paper) */
    int  auto_locked;
    int  auto_lock_countdown;
    int  auto_max_mean;
    int  auto_min_mean;

    /* Paper level — EMA of the per-line 10th-percentile energy (see
     * lux_drive.c: the mean-based floor followed the ink mass on dense
     * streams). -1 = unseeded. */
    float floor_ema;

    /* Per-pixel LINEAR gain, rebuilt only when the handles / width change. */
    float         lut[LUX_EQ_MAX_PIXELS];
    ShapeEqHandle lut_handles[SHAPE_EQ_MAX_HANDLES]; /* handles it was built from */
    float         lut_level_db;          /* level fader it was built from */
    float         lut_span_oct;          /* octave span it was built for */
    int           lut_px;                /* pixel count it was built for; 0 = stale */

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_EQ_MAX_PIXELS];
    uint8_t out_g[LUX_EQ_MAX_PIXELS];
    uint8_t out_b[LUX_EQ_MAX_PIXELS];
} LuxEqState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void        lux_eq_init(LuxEqState *state);
void        lux_eq_reset(LuxEqState *state);   /* floor/AUTO re-armed, config untouched */
LuxEqConfig lux_eq_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or the curve is flat) the input pointers are
 * returned as-is (O(1) pass-through after the one-shot lazy re-arm).
 */
void lux_eq_process_frame(
    LuxEqState     *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxReverb/LuxEcho) ──────────── */
extern LuxEqState g_lux_eq_proc;
LuxEqState *lux_eq_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void        lux_eq_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_EQ_H */
