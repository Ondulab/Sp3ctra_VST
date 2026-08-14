/*
 * lux_centro.h
 *
 * LuxCentro (CENTROID) — mass-to-barycentre simplifier on the image-line
 * stream.
 *
 * Three operations, chained:
 *   1. FLOOR — écrêtage: a user threshold (relative to the tracked
 *      background floor) splits the line into MASSES: contiguous runs of
 *      material above it, separated by floor ("entre deux fonds").
 *      The gate is a true écrêtage: everything below the threshold is
 *      ERASED — the output is the redrawn lines on CLEAN PAPER (the pure
 *      background pole, 255 in white mode / 0 in black mode). Floor high
 *      enough that no mass survives = blank paper. Printing the constant
 *      pole (never the ESTIMATED floor, whose per-line wander painted
 *      light vertical bands — the grey-bands bug) keeps the background
 *      stable by construction.
 *   2. BARYCENTRE — each mass is re-printed as ONE line centred on its
 *      energy barycentre, with a user thickness and a user edge shape.
 *      The edge profile PIVOTS at half height around the half-thickness
 *      point: edge_soft 0 = square band, 1 = smooth bump — the plateau
 *      shrinks as the skirt grows outward, so the EQUIVALENT width (the
 *      profile's integral) stays the thickness at every softness. The skirt
 *      is never thinner than 1 px, so a 1-px line genuinely softens into a
 *      small bump instead of keeping a hard centre pixel. Rendering is 4×
 *      supersampled (sub-pixel barycentres land smoothly). The line's
 *      colour is the mass's MOST SIGNIFICANT sample — the RGB of its
 *      peak-material-luminance pixel (strong black, strong colour…), NOT
 *      the zone average and NOT an energy/area spread: thickness widens
 *      the stroke at constant colour, it never dims or dilutes it. If
 *      overlapping skirts (or an EQ boost) overflow full scale, the clip
 *      factor is shared by the three channels — the line saturates toward
 *      its own hue, never towards grey (per-channel clipping crushed the
 *      colour).
 *   3. EQ — an optional output gain curve applied AFTER the redraw, on the
 *      composed output material: the redrawn lines change LEVEL along the
 *      pixel/frequency axis, never position. Exactly a LuxEq insert chained
 *      behind the module (same node model, same shared Catmull-Rom spline —
 *      lux_eq_curve_db), without the extra chain block. Flat curve = bypass.
 *
 * The tracked background floor only feeds the GATE (which runs are masses);
 * the output paper is the constant pole, and the line colour is sampled
 * from the source pixels themselves.
 *
 * Memory: per instance ≈ 250 KB (3× per-channel accumulator + segment
 * tables + EQ gain LUT + RGB out).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N + masses * thickness).
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-08-03
 */

#ifndef LUX_CENTRO_H
#define LUX_CENTRO_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */
#include "lux_eq.h"       /* LUX_EQ_NUM_BANDS + lux_eq_curve_db — the output
                           * EQ IS the LuxEq curve (single source of truth) */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxHarmo (>6912 for 400 DPI CIS). */
#define LUX_CENTRO_MAX_PIXELS   8192

/* Worst case: material/floor alternating every other pixel. */
#define LUX_CENTRO_MAX_SEGMENTS (LUX_CENTRO_MAX_PIXELS / 2)

/* Widest supported line redraw (thickness 64 px + full soft skirt margin). */
#define LUX_CENTRO_MAX_WIN      144

/* Background mode — which pole is the "material" (mirrors LUX_HARMO_BG_*). */
#define LUX_CENTRO_BG_BLACK  0   /* bright material on black background */
#define LUX_CENTRO_BG_WHITE  1   /* dark material on white background   */
#define LUX_CENTRO_BG_AUTO   2   /* detect from the stream (default)    */

/* UI guide profile resolution — matches the editor's view width. */
#define LUX_CENTRO_UI_BINS   128

/* ============================================================================
 * LuxCentroConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float floor_level;      /* 0..1 — écrêtage threshold, fraction of full
                             * scale (255) ABOVE the tracked background floor:
                             * material below it is floor, runs above it are
                             * the masses */
    float thickness_px;     /* 1..64 — EQUIVALENT width of each redrawn
                             * barycentre line (profile integral, softness-
                             * invariant) */
    float edge_soft;        /* 0..1 — edge shape of the redrawn line, pivoting
                             * at half height: 0 = square band, 1 = smooth
                             * bump (skirt >= 1 px so thin lines soften too) */
    int   background_mode;  /* LUX_CENTRO_BG_* */

    /* Output EQ — gain curve applied AFTER the redraw, on the composed
     * output material (levels move, barycentre positions don't). Same node
     * model as LuxEq: eq_num_bands active nodes spread evenly over the
     * pixel axis, curve = the shared lux_eq_curve_db spline. */
    int   eq_num_bands;                        /* active nodes, 2..LUX_EQ_NUM_BANDS */
    float eq_band_gain_db[LUX_EQ_NUM_BANDS];   /* -24..+24 dB per node */
} LuxCentroConfig;

/* ============================================================================
 * LuxCentroState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxCentroConfig config;

    int  centro_active;  /* latch: a stream was simplified at least once since
                          * the last reset — NOT an activity indicator */

    /* Rack-LED heartbeat: bumped once per line the module actually CHANGED
     * (output != input) — see lux_reverb.h. */
    uint32_t active_ticks;
    int  last_bg_mode;   /* RESOLVED polarity the floor was learned in */

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE). */
    int  auto_bg_white;
    int  auto_locked;
    int  auto_lock_countdown;
    int  auto_max_mean;
    int  auto_min_mean;

    /* Paper level — EMA of the per-line 10th-percentile energy (see
     * lux_drive.c: grey-bands fix). -1 = unseeded. */
    float floor_ema;

    /* Per-frame mass tables (segment pass → render pass, never published):
     * seg_a/seg_b = inclusive pixel bounds, seg_pos = luminance barycentre,
     * seg_col = the line's colour reference — raw background-relative
     * energies of the mass's peak-material-luminance pixel (its most
     * significant sample). */
    int   num_segs;
    int   seg_a  [LUX_CENTRO_MAX_SEGMENTS];
    int   seg_b  [LUX_CENTRO_MAX_SEGMENTS];
    float seg_pos[LUX_CENTRO_MAX_SEGMENTS];
    float seg_col[3][LUX_CENTRO_MAX_SEGMENTS];

    /* Per-frame scratch: simplified material accumulator, one per channel. */
    float accum[3][LUX_CENTRO_MAX_PIXELS];

    /* Output-EQ per-pixel LINEAR gain, rebuilt only when the bands / width
     * change (mirrors LuxEq's LUT). eq_lut_px == 0 = stale AND doubles as
     * the "output EQ currently shaping" flag (cleared whenever the curve is
     * flat) — the UI live glow reads it. */
    float eq_lut[LUX_CENTRO_MAX_PIXELS];
    float eq_lut_gains[LUX_EQ_NUM_BANDS];   /* band values the LUT was built from */
    int   eq_lut_bands;                     /* node count it was built for */
    int   eq_lut_px;                        /* pixel count it was built for */

    /* UI guide — live profile of the input MATERIAL energy (0..1, above the
     * tracked background floor), downsampled to LUX_CENTRO_UI_BINS bins and
     * refreshed EVERY line as two layers:
     *   ui_in_now  — fast release: the stream as it breathes (the lows),
     *   ui_in_peak — slow release: rémanence of the recent maxima (the highs).
     * The editor reads both at its repaint rate — no publication window. */
    float ui_in_now [LUX_CENTRO_UI_BINS];
    float ui_in_peak[LUX_CENTRO_UI_BINS];
    int   ui_in_valid;      /* a stream line was captured at least once */

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_CENTRO_MAX_PIXELS];
    uint8_t out_g[LUX_CENTRO_MAX_PIXELS];
    uint8_t out_b[LUX_CENTRO_MAX_PIXELS];
} LuxCentroState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void            lux_centro_init(LuxCentroState *state);
void            lux_centro_reset(LuxCentroState *state);  /* floor/AUTO re-armed, config untouched */
LuxCentroConfig lux_centro_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled the input pointers are returned as-is (O(1)
 * pass-through after the one-shot lazy re-arm).
 */
void lux_centro_process_frame(
    LuxCentroState *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxHarmo) ───────────────────── */
extern LuxCentroState g_lux_centro_proc;
LuxCentroState *lux_centro_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void            lux_centro_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_CENTRO_H */
