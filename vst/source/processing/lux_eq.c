/*
 * lux_eq.c
 *
 * LuxEq — graphic equalizer implementation (see lux_eq.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. e_in  = polarity(in)                      (bg conversion)
 *   2. e_out = floor + lut[x] * (e_in - floor)   (material-only gain)
 *   3. out   = polarity(clamp(e_out))
 *
 * The per-pixel LUT samples the shared Catmull-Rom dB spline through the band
 * nodes (lux_eq_curve_db — nodes evenly spread over the pixel axis ==
 * log-frequency axis), converted to linear gain. It is rebuilt only when a
 * band value or the width changes.
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
    /* band_gain_db[] all 0 dB — flat curve */
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
    lux_eq_reset(state);
}

/* Resolve the background pole for this frame + report the background's OWN
 * energy (*out_floor) — same logic as lux_echo_resolve_bg: AUTO learns over a
 * short window then LOCKS, and the floor is a slow EMA fed ONLY by
 * near-background lines so dense passages don't balloon it. */
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

    /* Floor: follow the background only while the line is mostly background. */
    const int   line_is_bg = bg_white ? (mean > 143) : (mean < 111);
    float inst_floor = bg_white ? (float)(255 - mean) : (float)mean;
    if (inst_floor < 0.0f) inst_floor = 0.0f;
    if (state->floor_ema < 0.0f)
        state->floor_ema = line_is_bg ? inst_floor : 0.0f;   /* seed */
    else if (line_is_bg)
        state->floor_ema += (inst_floor - state->floor_ema) * (1.0f / 64.0f);

    *out_floor = state->floor_ema;
    return bg_white;
}

/* Rebuild the per-pixel linear-gain LUT when a band value or the width
 * changed. Nodes spread evenly over the pixel axis (== log-frequency axis);
 * the dB curve is the shared Catmull-Rom spline (lux_eq_curve_db). */
static void lux_eq_update_lut(LuxEqState *state, int px)
{
    int dirty = (state->lut_px != px);
    for (int b = 0; b < LUX_EQ_NUM_BANDS && !dirty; ++b)
        if (state->lut_gains[b] != state->config.band_gain_db[b])
            dirty = 1;
    if (!dirty)
        return;

    for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
        state->lut_gains[b] = state->config.band_gain_db[b];
    state->lut_px = px;

    const float span = (px > 1) ? (float)(px - 1) : 1.0f;
    for (int i = 0; i < px; ++i)
    {
        const float x  = ((float)i / span) * (float)(LUX_EQ_NUM_BANDS - 1);
        const float db = lux_eq_curve_db(state->lut_gains, x);
        state->lut[i] = powf(10.0f, db * (1.0f / 20.0f));
    }
}

/* Apply the gain curve to one channel (energy space, material only). */
static void lux_eq_channel(const uint8_t *in, uint8_t *out, const float *lut,
                           int px, int bg_white, float floor_e)
{
    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f) e_mat = 0.0f;
        float e_out = floor_e + lut[i] * e_mat;
        if (e_out > 255.0f) e_out = 255.0f;
        if (e_out < 0.0f)   e_out = 0.0f;
        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
    }
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
    (void)luxstral_num_octaves;

    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxEqConfig *cfg = &state->config;
    int flat = 1;
    for (int b = 0; b < LUX_EQ_NUM_BANDS && flat; ++b)
        if (fabsf(cfg->band_gain_db[b]) > 0.01f)
            flat = 0;
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

    lux_eq_update_lut(state, px);

    lux_eq_channel(in_r, state->out_r, state->lut, px, bg_white, floor_e);
    lux_eq_channel(in_g, state->out_g, state->lut, px, bg_white, floor_e);
    lux_eq_channel(in_b, state->out_b, state->lut, px, bg_white, floor_e);

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
