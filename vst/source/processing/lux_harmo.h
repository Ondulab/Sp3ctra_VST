/*
 * lux_harmo.h
 *
 * LuxHarmo (SCALE) — musical quantizer on the image-line stream.
 *
 * The pixel axis IS the instrument's frequency axis, log-mapped with a
 * CONSTANT pixel-per-semitone density (pps = pixel_count / (octaves * 12),
 * same coupling as LuxPitch). SCALE snaps that axis onto a musical grid:
 * the allowed degrees of (root, scale) anchored on the PHYSICAL pitch axis
 * (pixel 0 = axis_low_hz, so degree rows line up with the instrument's true
 * pitch classes — a printed A stays an A).
 *
 * Two modes:
 *   • MASK — comb filter: material within ±width/2 semitone of an allowed
 *     degree passes, the rest fades to the background floor (slope-softened
 *     edges). Subtractive, sparse.
 *   • WARP — energy reassignment: each row's material slides toward its
 *     nearest allowed degree (Voronoi cell). The cell is squeezed LINEARLY
 *     into a landing band of ±width/2 around the degree centre: width → 0
 *     collapses everything onto the centre row, width → cell size leaves the
 *     material (and its internal texture) in place. Energy-preserving
 *     scatter — dense streams bloom into chords instead of vanishing.
 *
 * Strength morphs continuously between the raw stream (0) and the fully
 * quantized grid (1) — applied at process time, so sweeping it never
 * rebuilds the grid. `glide_lines` smooths every musical jump: root/scale
 * changes crossfade old grid → new grid, and Strength steps (MIDI CC,
 * stepped automation, enable) slew linearly at the same rate — a full
 * 0 → 1 sweep takes exactly glide_lines frames, and enabling the module
 * fades the effect in instead of clicking.
 *
 * Gain/reassignment applies to the MATERIAL energy only (input minus the
 * background floor, same floor tracking as LuxEq/LuxEcho): quantizing must
 * re-print the strokes, never the paper. The chain inserts see the RAW
 * image, upstream of the synth's Negative — masking drives toward the
 * background pole, never toward black.
 *
 * Memory: per instance ≈ 190 KB (2× displacement + cell grids + scratch +
 * RGB out).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N). The displacement grids
 *            are rebuilt only when root/scale/axis geometry change.
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-07-18
 */

#ifndef LUX_HARMO_H
#define LUX_HARMO_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq (>6912 for 400 DPI CIS). */
#define LUX_HARMO_MAX_PIXELS  8192

/* Modes. */
#define LUX_HARMO_MODE_MASK  0   /* comb: off-grid material fades to floor  */
#define LUX_HARMO_MODE_WARP  1   /* reassign: material slides to the grid   */

/* Background mode — which pole is the "material" (mirrors LUX_EQ_BG_*). */
#define LUX_HARMO_BG_BLACK  0    /* bright material on black background */
#define LUX_HARMO_BG_WHITE  1    /* dark material on white background   */
#define LUX_HARMO_BG_AUTO   2    /* detect from the stream (default)    */

/* Scale presets — bit k of the mask = interval k (semitones above root).
 * Order is the UI/param order; masks live in lux_harmo.c. */
#define LUX_HARMO_SCALE_CHROMATIC   0
#define LUX_HARMO_SCALE_MAJOR       1
#define LUX_HARMO_SCALE_MINOR       2
#define LUX_HARMO_SCALE_HARM_MINOR  3
#define LUX_HARMO_SCALE_PENTA_MAJ   4
#define LUX_HARMO_SCALE_PENTA_MIN   5
#define LUX_HARMO_SCALE_BLUES       6
#define LUX_HARMO_SCALE_WHOLE_TONE  7
#define LUX_HARMO_SCALE_DORIAN      8
#define LUX_HARMO_SCALE_PHRYGIAN    9
#define LUX_HARMO_SCALE_LYDIAN      10
#define LUX_HARMO_SCALE_MIXOLYDIAN  11
#define LUX_HARMO_SCALE_FIFTHS      12
#define LUX_HARMO_SCALE_OCTAVES     13
#define LUX_HARMO_NUM_SCALES        14

/* 12-bit interval mask of a scale preset (bit 0 = root, always set). */
uint16_t lux_harmo_scale_mask(int scale);

/* ============================================================================
 * LuxHarmoConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   mode;             /* LUX_HARMO_MODE_* */
    int   root;             /* 0..11 semitones above C */
    int   scale;            /* LUX_HARMO_SCALE_* */
    float strength;         /* 0..1 — raw → fully quantized morph */
    float width_st;         /* tooth width in semitones, 0.05..1:
                             * MASK = comb passband, WARP = landing band
                             * the cell is squeezed into */
    float slope;            /* 0..1 edge steepness (1 = sharp comb)     */
    int   glide_lines;      /* grid-change crossfade length in frames   */
    int   background_mode;  /* LUX_HARMO_BG_* */
    float axis_low_hz;      /* pixel-0 frequency (g_sp3ctra_config.low_
                             * frequency); <= 0 falls back to C2 65.406 */
} LuxHarmoConfig;

/* ============================================================================
 * LuxHarmoState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxHarmoConfig config;

    int  harmo_active;   /* nonzero while quantizing a stream (rack LED) */
    int  last_bg_mode;   /* RESOLVED polarity the floor was learned in */

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE). */
    int  auto_bg_white;
    int  auto_locked;
    int  auto_lock_countdown;
    int  auto_max_mean;
    int  auto_min_mean;

    /* Background's own energy, slow EMA fed ONLY by near-background lines
     * (see lux_echo.c). -1 = unseeded. */
    float floor_ema;

    /* Displacement grids: disp[i] = (nearest allowed degree centre − i) in
     * pixels, at strength 1. MASK derives its comb gain from |disp|/pps;
     * WARP scatters material along it, scaled by the cell squeeze (below).
     * `cur` is the live grid; `old` is the previous one while a glide
     * crossfade is running (xfade < 1). */
    float disp_cur[LUX_HARMO_MAX_PIXELS];
    float disp_old[LUX_HARMO_MAX_PIXELS];

    /* Cell half-width in pixels on pixel i's side of its degree centre
     * (distance centre → Voronoi boundary). WARP squeezes the cell linearly
     * into the ±width/2 landing band: eff_disp = disp * (1 − h/cell). Built
     * with the matching disp grid (same dirty tracking). */
    float cell_cur[LUX_HARMO_MAX_PIXELS];
    float cell_old[LUX_HARMO_MAX_PIXELS];
    float xfade;         /* 0..1 — blend old → cur (1 = settled) */
    int   grid_valid;    /* cur was built at least once */
    float grid_pps;      /* pixels per semitone the grids were built with */

    /* Strength slew (glide-rate limited follower of config.strength).
     * Reset to 0 so (re)enabling fades the effect in click-free. */
    float strength_smooth;

    /* Grid dirty-tracking (rebuild only when one of these changes). */
    int   grid_root, grid_scale, grid_px, grid_octaves;
    float grid_low_hz;

    /* Per-frame scratch: blended effective gain (MASK, shared by the three
     * channels) or weighted material accumulator (WARP, per channel). */
    float scratch[LUX_HARMO_MAX_PIXELS];

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_HARMO_MAX_PIXELS];
    uint8_t out_g[LUX_HARMO_MAX_PIXELS];
    uint8_t out_b[LUX_HARMO_MAX_PIXELS];
} LuxHarmoState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void           lux_harmo_init(LuxHarmoState *state);
void           lux_harmo_reset(LuxHarmoState *state);  /* floor/AUTO/grids re-armed, config untouched */
LuxHarmoConfig lux_harmo_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or strength is 0) the input pointers are returned
 * as-is (O(1) pass-through after the one-shot lazy re-arm).
 */
void lux_harmo_process_frame(
    LuxHarmoState  *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxEq) ──────────────────────── */
extern LuxHarmoState g_lux_harmo_proc;
LuxHarmoState *lux_harmo_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void           lux_harmo_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_HARMO_H */
