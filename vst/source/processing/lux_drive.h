/*
 * lux_drive.h
 *
 * LuxDrive (LEVELS in the UI) — gain / saturation / floor stage on the
 * image-line stream.
 *
 * Three operations, chained on the MATERIAL energy (input minus the tracked
 * background floor, same convention as LuxEq/LuxEcho/LuxCentro — driving must
 * re-print the strokes, never brighten the paper):
 *
 *   1. FLOOR — écrêtage bas, CONTINUOUS (a "levels" black point): material
 *      energy is shifted down by the threshold (relative to the tracked
 *      background floor) then re-stretched so full scale stays full scale.
 *      At/below the threshold → background; floor = 100 % clips everything.
 *      Deliberately NOT a hard gate: a binary cut posterizes the paper
 *      texture around the threshold — the CIS fixed-pattern illumination
 *      bands popped out as crisp vertical stripes the moment the floor
 *      touched them.
 *   2. GAMMA — power law on the material energy, both ends ANCHORED (0 stays
 *      0, full scale stays full scale — no clipping, ever): γ < 1 lifts the
 *      faint material toward the masses, γ > 1 thins it out. The Photoshop
 *      Levels "middle slider"; a flat OUTPUT-EQ curve covers global gain.
 *   3. SATURATION — COLOUR saturation (chroma), bipolar around 0: each
 *      output pixel's channels are scaled around their own luminance,
 *      -1 = black & white, 0 = untouched, +1 = hyper-vibrant (chroma ×3).
 *      Applied JOINTLY on RGB after the tone transfer + EQ — neutral pixels
 *      (paper, grey strokes) are their own luminance and pass unchanged.
 *   4. INVERT — final joint-RGB inversion (mirrors the VideoScroll modes):
 *      Off / Negative (255 - each channel) / Luminance (invert the HSL
 *      lightness only — uniform shift by 255 - max - min, hue and colour
 *      saturation preserved).
 *   5. CONTRAST MIN — the visual port of the per-OUT audio knob (LuxStral
 *      contrastMin, img_stage_calculate_contrast law): per-line variance
 *      contrast maps to a factor in [contrast_min, 1] that scales the
 *      MATERIAL energy — a flat/blurred stream dims on screen exactly as it
 *      drops in volume. 1 = off; variance is polarity-invariant, so the
 *      measure needs no background handling.
 *
 *   m     = max(0, e_in - floor)              (floor = tracked PAPER level)
 *   e_out = (e_in - m) + eq[x] * T(m)         (clamped to 0..255 — the
 *                                              pixel's own pedestal stays:
 *                                              the paper is never re-printed)
 *   T(m)  = 255 * (blackpoint(m)/255)^gamma   (shared with the UI editor, so
 *                                              the drawn curve IS the
 *                                              applied transfer)
 *   out_c = lum + k(saturation) * (c - lum)   (chroma stage, per pixel on
 *                                              the composed RGB output)
 *
 * eq[x] is an optional OUTPUT EQ applied after the transfer, on the driven
 * material only: exactly a LuxEq insert chained behind the module (same node
 * model, same shared Catmull-Rom spline — lux_eq_curve_db), without the extra
 * chain block. Flat curve = bypass. Mirrors the CENTROID output EQ.
 *
 * Energy space: `background_mode` picks which pole carries the material
 * (mirrors LuxEq/LuxCentro — the chain inserts see the RAW image, upstream
 * of the synth's Negative).
 *
 * Memory: per instance ≈ 57 KB (transfer LUT + EQ gain LUT + RGB out
 * buffers) — no history.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N). The transfer LUT is
 *            rebuilt only when a config value changes.
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-08-03
 */

#ifndef LUX_DRIVE_H
#define LUX_DRIVE_H

#include <math.h>
#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */
#include "lux_eq.h"       /* LUX_EQ_NUM_BANDS + lux_eq_curve_db — the output
                           * EQ IS the LuxEq curve (single source of truth) */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxCentro (>6912 for 400 DPI CIS). */
#define LUX_DRIVE_MAX_PIXELS  8192

/* Transfer LUT resolution — one entry per input energy step (0..255) plus a
 * duplicated top entry so linear interpolation never reads past the end. */
#define LUX_DRIVE_LUT_SIZE    257

/* Gamma range — matches the project's other gamma controls (log-centred 1). */
#define LUX_DRIVE_GAMMA_MIN 0.1f
#define LUX_DRIVE_GAMMA_MAX 10.0f

/* Chroma multiplier at saturation = +1 (0 at -1, 1 at 0 — "hyper-vibrant"
 * pushes each channel 3× away from the pixel's luminance). */
#define LUX_DRIVE_CHROMA_MAX 3.0f

/* Background mode — which pole is the "material" (mirrors LUX_EQ_BG_*). */
#define LUX_DRIVE_BG_BLACK  0   /* bright material on black background */
#define LUX_DRIVE_BG_WHITE  1   /* dark material on white background   */
#define LUX_DRIVE_BG_AUTO   2   /* detect from the stream (default)    */

/* UI guide profile resolution — matches the editor's view width. */
#define LUX_DRIVE_UI_BINS   128

/* Output inversion mode — mirrors the VideoScroll "invertMode" choices. */
#define LUX_DRIVE_INV_OFF      0
#define LUX_DRIVE_INV_NEGATIVE 1   /* 255 - each channel                    */
#define LUX_DRIVE_INV_LUMA     2   /* invert HSL lightness, hue/sat kept    */

/* Transfer curve on material energy m ∈ [0, 255]. Single source of truth:
 * the RT LUT builder and the UI editor both sample THIS, so what is drawn is
 * what is applied.
 *   gamma — power-law exponent on the black-pointed material (both ends
 *           anchored: 0 → 0 and 255 → 255, so the curve never clips)
 *   thr   — écrêtage threshold in energy units (floor_level * 255)
 * The floor is a CONTINUOUS black point (subtract + re-stretch, full scale
 * stays full scale) — never a hard gate, which would binarize the paper
 * texture around the threshold. floor = 100 % still clips everything. */
static inline float lux_drive_transfer(float m, float gamma, float thr)
{
    if (thr >= 255.0f) return 0.0f;      /* floor at max: everything clipped */
    m = (m - thr) * (255.0f / (255.0f - thr));
    if (m <= 0.0f) return 0.0f;          /* at/below the black point → background */
    if (m > 255.0f) m = 255.0f;
    if (gamma != 1.0f)
        m = 255.0f * powf(m * (1.0f / 255.0f), gamma);
    return m;
}

/* ============================================================================
 * LuxDriveConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float gamma;            /* 0.1..10 mid-tone power law (1 = linear) */
    float saturation;       /* -1..1 — COLOUR saturation: -1 = B&W,
                             * 0 = untouched, +1 = chroma ×LUX_DRIVE_CHROMA_MAX */
    float floor_level;      /* 0..1 — écrêtage threshold, fraction of full
                             * scale (255) ABOVE the tracked background floor */
    int   invert_mode;      /* LUX_DRIVE_INV_* — final output inversion */
    int   background_mode;  /* LUX_DRIVE_BG_* */

    /* CONTRAST MIN — visual port of the LuxStral OUT contrastMin knob.
     * Per-line variance contrast scales the material by a factor in
     * [contrast_min, 1] (same law as img_stage_calculate_contrast).
     * 1 = off. contrast_power mirrors the audio side's
     * additive_contrast_adjustment_power (synced, same curve). */
    float contrast_min;
    float contrast_power;

    /* Output EQ — gain curve applied AFTER the transfer, on the driven
     * material (levels move along the pixel/frequency axis). Same node model
     * as LuxEq: eq_num_bands active nodes spread evenly over the pixel axis,
     * curve = the shared lux_eq_curve_db spline (mirrors LuxCentro). */
    int   eq_num_bands;                        /* active nodes, 2..LUX_EQ_NUM_BANDS */
    float eq_band_gain_db[LUX_EQ_NUM_BANDS];   /* -24..+24 dB per node */
} LuxDriveConfig;

/* ============================================================================
 * LuxDriveState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxDriveConfig config;

    int  drive_active;   /* latch: the powered block saw a stream since the
                          * last reset (bg/floor learned, view live) — NOT an
                          * "is shaping" indicator; the rack LED heartbeat is
                          * active_ticks */

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

    /* PAPER level: EMA (1/16, every line) of the per-line 10th-percentile
     * energy. Not the mean-based floor the other FX use — the mean is
     * contaminated by the material, and a content-tracking floor painted
     * grey bands following the ink mass. -1 = unseeded. */
    float floor_ema;

    /* CONTRAST MIN — smoothed line-contrast factor (EMA 1/8) actually
     * applied to the material this frame. -1 = unseeded / knob at 1 (off);
     * the editor reads it to dim the accent output curve live. */
    float contrast_ema;

    /* Transfer LUT (input material energy → output material energy),
     * rebuilt only when the config values change. */
    float lut[LUX_DRIVE_LUT_SIZE];
    float lut_gamma;        /* config values the LUT was built from */
    float lut_floor;
    int   lut_valid;        /* 0 = stale */

    /* Output-EQ per-pixel LINEAR gain, rebuilt only when the bands / width
     * change (mirrors LuxCentro). eq_lut_px == 0 = stale AND doubles as the
     * "output EQ currently shaping" flag (cleared whenever the curve is
     * flat) — the UI live glow reads it. */
    float eq_lut[LUX_DRIVE_MAX_PIXELS];
    float eq_lut_gains[LUX_EQ_NUM_BANDS];   /* band values the LUT was built from */
    int   eq_lut_bands;                     /* node count it was built for */
    int   eq_lut_px;                        /* pixel count it was built for */

    /* UI guide — live profile of the input MATERIAL energy (0..1, above the
     * tracked paper level), downsampled to LUX_DRIVE_UI_BINS bins and
     * refreshed EVERY line as two layers:
     *   ui_in_now  — fast release: the stream as it breathes (the lows),
     *   ui_in_peak — slow release: rémanence of the recent maxima (the highs).
     * The editor reads both at its repaint rate — no publication window. */
    float ui_in_now [LUX_DRIVE_UI_BINS];
    float ui_in_peak[LUX_DRIVE_UI_BINS];
    int   ui_in_valid;      /* a stream line was captured at least once */

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_DRIVE_MAX_PIXELS];
    uint8_t out_g[LUX_DRIVE_MAX_PIXELS];
    uint8_t out_b[LUX_DRIVE_MAX_PIXELS];
} LuxDriveState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void           lux_drive_init(LuxDriveState *state);
void           lux_drive_reset(LuxDriveState *state);   /* floor/AUTO re-armed, config untouched */
LuxDriveConfig lux_drive_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled the input pointers are returned as-is (O(1)
 * pass-through after the one-shot lazy re-arm). An identity transfer on a
 * POWERED block also passes the pointers through, but still tracks the
 * background and feeds the editor's live view (CENTROID parity — the editor
 * shows the real stream as soon as the block is on).
 */
void lux_drive_process_frame(
    LuxDriveState  *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxEq/LuxCentro) ────────────── */
extern LuxDriveState g_lux_drive_proc;
LuxDriveState *lux_drive_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void           lux_drive_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_DRIVE_H */
