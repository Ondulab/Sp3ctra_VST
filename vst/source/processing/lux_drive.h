/*
 * lux_drive.h
 *
 * LuxDrive (LEVELS in the UI) — gain / saturation / floor stage on the
 * image-line stream.
 *
 * Operations chained on the ABSOLUTE energy from the background pole
 * (2026-08-16 — the tracked paper pedestal is GONE from this module; the
 * other FX keep the LuxEq/LuxEcho/LuxCentro material convention):
 *
 *   1. FLOOR — écrêtage bas ABSOLU, CONTINUOUS (a "levels" black point
 *      anchored on the pole): energy measured from the true background pole
 *      is shifted down by the threshold then re-stretched so full scale
 *      stays full scale. At/below the threshold → the EXACT pole (255-pure
 *      white on a white stream); floor = 100 % clips everything. This is
 *      the one floor shape that can silence the paper under LuxStral's
 *      inverse-dB decode, where ONLY the exact pole is silent: the old
 *      pedestal-relative floor classified near-white sensor/JPEG noise as
 *      untouchable paper and could never remove its sound. The trade is
 *      assumed: texture below the threshold (CIS illumination bands
 *      included) is ERASED, visually and audibly. Still deliberately NOT a
 *      hard gate: a binary cut posterizes the texture around the threshold.
 *      Grey-bands safety holds because nothing is re-printed at an
 *      ESTIMATED level — the anchor is the pole itself, so the curve is a
 *      deterministic per-value tone map (a Photoshop "Levels", not a
 *      content-tracking repaint).
 *   2. GAMMA — power law on the black-pointed energy, both ends ANCHORED
 *      (pole stays pole, full scale stays full scale — no clipping, ever).
 *      PHOTO convention pow(x, 1/γ), same direction as the VideoScroll
 *      gamma: γ > 1 lifts the faint end toward the masses (brighter), γ < 1
 *      thins it out. The Photoshop Levels "middle slider"; a flat OUTPUT-EQ
 *      curve covers global gain. NOTE: anchored at the pole, so with the
 *      floor at 0 a γ > 1 lifts the paper grain too — set the floor first.
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
 *      driven energy — a flat/blurred stream dims on screen exactly as it
 *      drops in volume. 1 = off; variance is polarity-invariant, so the
 *      measure needs no background handling.
 *
 *   e     = polarity(in)                      (ABSOLUTE energy from the pole)
 *   e_out = eq[x] * T(e)                      (clamped to 0..255 — e at/below
 *                                              the floor threshold lands on
 *                                              the EXACT pole)
 *   T(e)  = 255 * (blackpoint(e)/255)^(1/γ)   (shared with the UI editor, so
 *                                              the drawn curve IS the
 *                                              applied transfer)
 *   out_c = lum + k(saturation) * (c - lum)   (chroma stage, per pixel on
 *                                              the composed RGB output)
 *
 * eq[x] is an optional OUTPUT EQ applied after the transfer, on the driven
 * energy: exactly a LuxEq insert chained behind the module (same typed-handle
 * model, same shared evaluator — shape_eq_db), without the extra chain block.
 * Flat curve = bypass. Mirrors the CENTROID output EQ.
 *
 * Energy space: `background_mode` picks the reference pole — polarity ONLY,
 * AUTO detects it from the stream (mirrors LuxEq/LuxCentro — the chain
 * inserts see the RAW image, upstream of the synth's Negative).
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
#include "shape_eq.h"     /* ShapeEqHandle + shape_eq_db — the output EQ IS
                           * the LuxEq curve (single source of truth) */

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

/* Transfer curve on ABSOLUTE energy m ∈ [0, 255], measured from the
 * background pole (0 = the pole itself). Single source of truth: the RT LUT
 * builder and the UI editor both sample THIS, so what is drawn is what is
 * applied.
 *   gamma — power law on the black-pointed energy, PHOTO convention
 *           pow(x, 1/gamma) — same direction as the VideoScroll display
 *           gamma: > 1 lifts the faint end (brighter), < 1 thins it.
 *           Both ends anchored: 0 → 0 and 255 → 255, so the curve never clips.
 *   thr   — écrêtage threshold in energy units (floor_level * 255);
 *           at/below → 0, i.e. the EXACT pole (the only value LuxStral's
 *           inverse-dB decode reads as silence)
 * The floor is a CONTINUOUS black point (subtract + re-stretch, full scale
 * stays full scale) — never a hard gate, which would binarize the texture
 * around the threshold. floor = 100 % still clips everything. */
static inline float lux_drive_transfer(float m, float gamma, float thr)
{
    if (thr >= 255.0f) return 0.0f;      /* floor at max: everything clipped */
    m = (m - thr) * (255.0f / (255.0f - thr));
    if (m <= 0.0f) return 0.0f;          /* at/below the black point → background */
    if (m > 255.0f) m = 255.0f;
    if (gamma > 0.0f && gamma != 1.0f)
        m = 255.0f * powf(m * (1.0f / 255.0f), 1.0f / gamma);
    return m;
}

/* ============================================================================
 * LuxDriveConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float gamma;            /* 0.1..10 mid-tone power law, photo convention
                             * pow(x, 1/gamma): > 1 brightens (1 = linear) */
    float saturation;       /* -1..1 — COLOUR saturation: -1 = B&W,
                             * 0 = untouched, +1 = chroma ×LUX_DRIVE_CHROMA_MAX */
    float floor_level;      /* 0..1 — écrêtage threshold, fraction of full
                             * scale (255) from the background pole (ABSOLUTE
                             * black point: at/below → the exact pole) */
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
     * material (levels move along the pixel/frequency axis). Same typed-
     * handle model as LuxEq: curve = the shared shape_eq_db (mirrors
     * LuxCentro). */
    float         eq_level_db;                      /* whole-curve gain fader */
    ShapeEqHandle eq_handles[SHAPE_EQ_MAX_HANDLES];
} LuxDriveConfig;

/* ============================================================================
 * LuxDriveState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxDriveConfig config;

    int  drive_active;   /* latch: the powered block saw a stream since the
                          * last reset (bg polarity learned, view live) — NOT
                          * an "is shaping" indicator; the rack LED heartbeat
                          * is active_ticks */

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

    /* Output-EQ per-pixel LINEAR gain, rebuilt only when the handles / width
     * change (mirrors LuxCentro). eq_lut_px == 0 = stale AND doubles as the
     * "output EQ currently shaping" flag (cleared whenever the curve is
     * flat) — the UI live glow reads it. */
    float         eq_lut[LUX_DRIVE_MAX_PIXELS];
    ShapeEqHandle eq_lut_handles[SHAPE_EQ_MAX_HANDLES]; /* handles it was built from */
    float         eq_lut_level_db;          /* level fader it was built from */
    float         eq_lut_span_oct;          /* octave span it was built for */
    int           eq_lut_px;                /* pixel count it was built for */

    /* UI guide — live profile of the input energy (0..1, ABSOLUTE from the
     * background pole), downsampled to LUX_DRIVE_UI_BINS bins and
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
