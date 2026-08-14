/*
 * lux_eq.h
 *
 * LuxEq — graphic equalizer on the image-line stream.
 *
 * The pixel axis IS the instrument's frequency axis (log-mapped, pixel 0 =
 * low_frequency, last pixel = high_frequency — see wave_generation.c), so a
 * "band" is simply a node on that axis: `num_bands` nodes (2..LUX_EQ_NUM_BANDS,
 * user-selectable, default 2 = one straight line) spread evenly over it and
 * the per-pixel gain is a smooth Catmull-Rom spline through them in dB
 * (lux_eq_curve_db — shared with the UI editor, so the drawn curve IS the
 * applied gain).
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

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_EQ_MAX_PIXELS  8192

/* MAXIMUM node count — one node per octave boundary of the default 8-octave
 * range (C2..~16.7k), matching ScoreEqComponent's grid. The ACTIVE count is
 * config.num_bands (2..LUX_EQ_NUM_BANDS). The curve is positional: the active
 * nodes spread evenly over the pixel axis whatever the configured span. */
#define LUX_EQ_NUM_BANDS   9
#define LUX_EQ_GAIN_DB_MAX 24.0f   /* band range: ±24 dB */

/* Background mode — which pole is the "material" (mirrors LUX_ECHO_BG_*). */
#define LUX_EQ_BG_BLACK  0   /* bright material on black background */
#define LUX_EQ_BG_WHITE  1   /* dark material on white background   */
#define LUX_EQ_BG_AUTO   2   /* detect from the stream (default)    */

/* Gain curve in dB at position x ∈ [0, n-1] over the n active node gains —
 * uniform Catmull-Rom spline (C1-smooth, interpolates every node; end nodes
 * duplicated). Single source of truth: the RT LUT builder and the UI editor
 * both sample THIS, so what is drawn is what is applied. Overshoot between
 * nodes is clamped to the band range (±24 dB). */
static inline float lux_eq_curve_db(const float g[/*n*/], int n, float x)
{
    if (n > LUX_EQ_NUM_BANDS) n = LUX_EQ_NUM_BANDS;
    if (n < 2)                return (n == 1) ? g[0] : 0.0f;
    const int last = n - 1;
    if (x <= 0.0f)          return g[0];
    if (x >= (float)last)   return g[last];
    /* Two nodes = ONE STRAIGHT LINE (the duplicated-endpoint spline below
     * would ease in/out into an S shape between them). */
    if (n == 2)             return g[0] + (g[1] - g[0]) * x;
    int k = (int)x;
    if (k > last - 1) k = last - 1;
    const float t  = x - (float)k;
    const float p0 = g[(k > 0) ? k - 1 : 0];
    const float p1 = g[k];
    const float p2 = g[k + 1];
    const float p3 = g[(k + 2 <= last) ? k + 2 : last];
    const float t2 = t * t, t3 = t2 * t;
    float db = 0.5f * ((2.0f * p1)
                     + (p2 - p0) * t
                     + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                     + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
    if (db >  LUX_EQ_GAIN_DB_MAX) db =  LUX_EQ_GAIN_DB_MAX;
    if (db < -LUX_EQ_GAIN_DB_MAX) db = -LUX_EQ_GAIN_DB_MAX;
    return db;
}

/* ============================================================================
 * LuxEqConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   background_mode;                  /* LUX_EQ_BG_* */
    int   num_bands;                        /* active nodes, 2..LUX_EQ_NUM_BANDS */
    float band_gain_db[LUX_EQ_NUM_BANDS];   /* -24..+24 dB per node */
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

    /* Per-pixel LINEAR gain, rebuilt only when the bands / width change. */
    float lut[LUX_EQ_MAX_PIXELS];
    float lut_gains[LUX_EQ_NUM_BANDS];   /* band values the LUT was built from */
    int   lut_bands;                     /* node count it was built for */
    int   lut_px;                        /* pixel count it was built for; 0 = stale */

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
