/*
 * lux_gain.h
 *
 * LuxGain (GAIN in the UI) — per-line energy gain as a chain insert.
 *
 * Multiplies the material energy of every pixel by one linear factor
 * (set in dB from the UI): boost pulls the line toward full scale,
 * attenuation pushes it toward the background. The background itself
 * (energy 0) passes bit-identical — 0 × g = 0, no floor tracker needed.
 *
 * Per line, in energy space (background pole = 0):
 *
 *   e_out_c = jointclip(e_c[i] * gain)
 *
 * Overflow uses ONE joint clip factor across the three channels (the
 * lux_centro compose rule): clipping each channel independently would crush
 * the R:G:B ratio and drift the hue toward grey — a boosted line must
 * saturate toward its OWN hue.
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
 * Created: 2026-08-16
 */

#ifndef LUX_GAIN_H
#define LUX_GAIN_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxDrive (>6912 for 400 DPI CIS). */
#define LUX_GAIN_MAX_PIXELS  8192

/* Background mode — which pole is the "material" (mirrors LUX_EQ_BG_*). */
#define LUX_GAIN_BG_BLACK  0   /* bright material on black background */
#define LUX_GAIN_BG_WHITE  1   /* dark material on white background   */
#define LUX_GAIN_BG_AUTO   2   /* detect from the stream (default)    */

/* UI guide profile resolution — matches the editor's view width. */
#define LUX_GAIN_UI_BINS   128

/* ============================================================================
 * LuxGainConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    float gain_lin;         /* linear energy factor (UI edits in dB;
                             * 1 = unity = pass-through) */
    int   background_mode;  /* LUX_GAIN_BG_* */
} LuxGainConfig;

/* ============================================================================
 * LuxGainState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxGainConfig config;

    int  gain_active;    /* latch: a stream was processed at least once since
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
     * LUX_GAIN_UI_BINS bins and refreshed EVERY line as two layers:
     *   ui_in_now  — fast release: the stream as it breathes (the lows),
     *   ui_in_peak — slow release: rémanence of the recent maxima (the highs).
     * The editor reads both at its repaint rate — no publication window. */
    float ui_in_now [LUX_GAIN_UI_BINS];
    float ui_in_peak[LUX_GAIN_UI_BINS];
    int   ui_in_valid;      /* a stream line was captured at least once */

    /* Preallocated output buffers. */
    uint8_t out_r[LUX_GAIN_MAX_PIXELS];
    uint8_t out_g[LUX_GAIN_MAX_PIXELS];
    uint8_t out_b[LUX_GAIN_MAX_PIXELS];
} LuxGainState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void          lux_gain_init(LuxGainState *state);
void          lux_gain_reset(LuxGainState *state);   /* AUTO re-armed, config untouched */
LuxGainConfig lux_gain_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or the gain is unity) the input pointers are
 * returned as-is (O(1) pass-through after the one-shot lazy re-arm).
 */
void lux_gain_process_frame(
    LuxGainState  *state,
    const uint8_t *in_r,
    const uint8_t *in_g,
    const uint8_t *in_b,
    int            pixel_count,
    int            luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxEq/LuxDrive) ─────────────── */
extern LuxGainState g_lux_gain_proc;
LuxGainState *lux_gain_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void          lux_gain_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_GAIN_H */
