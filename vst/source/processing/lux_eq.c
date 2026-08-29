/*
 * lux_eq.c
 *
 * LuxEq — graphic equalizer implementation (see lux_eq.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. e_in  = polarity(in)                      (bg conversion)
 *   2. m     = max(0, e_in - floor)              (floor = tracked PAPER level,
 *                                                 10th-percentile estimator)
 *   3. e_out = (e_in - m) + lut[x] * m           (material-only gain — the
 *                                                 pixel's own pedestal stays)
 *   4. out   = polarity(clamp(e_out))
 *
 * The per-pixel LUT samples the shared typed-handle evaluator (shape_eq_db —
 * pixel axis == log-frequency axis), converted to linear gain. It is rebuilt
 * only when a handle or the width/span changes.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-05
 */

#include "lux_eq.h"
#include <math.h>
#include <string.h>

/* ── Instance pool (mirrors lux_echo.c) ────────────────────────────────────────
 * Slot 0 is g_lux_eq_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxEqState g_lux_eq_proc;
static LuxEqState s_lux_eq_extra[CHAIN_MAX_CHAINS - 1];

LuxEqState *lux_eq_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_eq_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_eq_extra[idx - 1];
}

void lux_eq_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_eq_init(lux_eq_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxEqConfig lux_eq_config_default(void)
{
    LuxEqConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.background_mode = LUX_EQ_BG_AUTO;
    for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h)
        shape_eq_default(&cfg.handles[h]);   /* all Off — flat curve */
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_EQ_BG_LOCK_LINES 96

void lux_eq_reset(LuxEqState *state)
{
    if (!state) return;
    state->eq_active    = 0;
    state->last_bg_mode = -1;
    state->lut_px       = 0;    /* invalidate the gain LUT */
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_EQ_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
}

void lux_eq_init(LuxEqState *state)
{
    if (!state) return;
    state->config = lux_eq_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    lux_eq_reset(state);
}

/* Resolve the background pole for this frame + report the PAPER's own energy
 * (*out_floor). Polarity: mean-based AUTO learn-then-LOCK. Paper level: EMA of
 * the per-line 10th-PERCENTILE energy — NOT the mean-based floor (see
 * lux_drive_resolve_bg: the mean is contaminated by the material, so on dense
 * streams the "paper" estimate followed the ink mass — grey vertical bands
 * tracking the black mass). A low percentile finds the paper between the
 * strokes even on dense lines. */
static int lux_eq_resolve_bg(LuxEqState *state,
                             const uint8_t *in_r, const uint8_t *in_g,
                             const uint8_t *in_b, int px, float *out_floor)
{
    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        sum += (uint32_t)in_r[i] + in_g[i] + in_b[i];
        n   += 3;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;

    int bg_white;
    const int mode = state->config.background_mode;
    if (mode == LUX_EQ_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_EQ_BG_WHITE) bg_white = 1;
    else if (state->auto_locked)      bg_white = state->auto_bg_white;
    else
    {
        if (mean > state->auto_max_mean) state->auto_max_mean = mean;
        if (mean < state->auto_min_mean) state->auto_min_mean = mean;
        state->auto_bg_white = (state->auto_max_mean + state->auto_min_mean > 255) ? 1 : 0;
        if (--state->auto_lock_countdown <= 0)
            state->auto_locked = 1;
        bg_white = state->auto_bg_white;
    }

    /* 10th-percentile energy over a 32-bin histogram of sampled pixels. */
    int hist[32] = { 0 };
    int ns = 0;
    for (int i = 0; i < px; i += 4)
    {
        const int v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const int e = bg_white ? 255 - v : v;
        hist[e >> 3]++;
        ns++;
    }
    const int target = ns / 10;
    int acc = 0, bin = 0;
    for (; bin < 31; ++bin)
    {
        acc += hist[bin];
        if (acc > target)
            break;
    }
    const float inst_floor = (float)(bin * 8 + 4);

    /* EMA every line (1/16) — a percentile needs no "line is background"
     * gate, and reseeds honestly right after a reset. */
    if (state->floor_ema < 0.0f)
        state->floor_ema = inst_floor;
    else
        state->floor_ema += (inst_floor - state->floor_ema) * (1.0f / 16.0f);

    *out_floor = state->floor_ema;
    return bg_white;
}

/* Rebuild the per-pixel linear-gain LUT when a handle, the pixel count or the
 * octave span changed. The curve is positional (x01 == pixel axis == log-
 * frequency axis); the dB value is the shared typed-handle evaluator
 * (shape_eq_db). */
static void lux_eq_update_lut(LuxEqState *state, int px, float span_oct)
{
    if (state->lut_px == px && state->lut_span_oct == span_oct
        && state->lut_level_db == state->config.level_db
        && shape_eq_handles_equal(state->lut_handles, state->config.handles,
                                  SHAPE_EQ_MAX_HANDLES))
        return;

    for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h)
        state->lut_handles[h] = state->config.handles[h];
    state->lut_level_db = state->config.level_db;
    state->lut_span_oct = span_oct;
    state->lut_px       = px;

    const float span = (px > 1) ? (float)(px - 1) : 1.0f;
    for (int i = 0; i < px; ++i)
    {
        const float db = shape_eq_db_level(state->lut_handles,
                                           SHAPE_EQ_MAX_HANDLES,
                                           state->lut_level_db,
                                           (float)i / span, span_oct);
        state->lut[i] = powf(10.0f, db * (1.0f / 20.0f));
    }
}

/* Apply the gain curve to one channel (energy space, material only).
 * The pixel's OWN pedestal is preserved: e_out = e_in - m + lut[x] * m.
 * Background pixels (m = 0) pass through bit-identical — the paper is never
 * re-printed at the estimated floor (doing so painted grey bands that tracked
 * the ink mass whenever the estimate drifted with the content — same fix as
 * lux_drive_channel).
 * Returns nonzero when the curve actually altered the line (rack LED). */
static int lux_eq_channel(const uint8_t *in, uint8_t *out, const float *lut,
                          int px, int bg_white, float floor_e)
{
    int diff = 0;   /* OR of out^in — a material-free line passes through */

    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f) e_mat = 0.0f;
        float e_out = (e_in - e_mat) + lut[i] * e_mat;
        if (e_out > 255.0f) e_out = 255.0f;
        if (e_out < 0.0f)   e_out = 0.0f;
        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
        diff  |= out[i] ^ in[i];
    }
    return diff;
}

void lux_eq_process_frame(
    LuxEqState     *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxEqConfig *cfg = &state->config;
    const int flat = shape_eq_is_flat_level(cfg->handles, SHAPE_EQ_MAX_HANDLES,
                                            cfg->level_db);
    if (!cfg->enabled || flat)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity/floor. */
        if (state->eq_active)
            lux_eq_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_EQ_MAX_PIXELS) px = LUX_EQ_MAX_PIXELS;

    float floor_e = 0.0f;
    const int bg_white = lux_eq_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    /* The floor was learned in one polarity — a flip invalidates it (the LUT
     * is polarity-agnostic and stays). */
    if (state->last_bg_mode != bg_white)
    {
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->eq_active = 1;

    lux_eq_update_lut(state, px,
                      (float)((luxstral_num_octaves > 0) ? luxstral_num_octaves
                                                         : 8));

    int diff = lux_eq_channel(in_r, state->out_r, state->lut, px, bg_white, floor_e);
    diff |= lux_eq_channel(in_g, state->out_g, state->lut, px, bg_white, floor_e);
    diff |= lux_eq_channel(in_b, state->out_b, state->lut, px, bg_white, floor_e);
    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
