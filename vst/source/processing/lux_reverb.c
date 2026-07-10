/*
 * lux_reverb.c
 *
 * LuxReverb — visual reverberation implementation (see lux_reverb.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. tail *= k            — exponential fade, k derived from decay_s and the
 *                             measured frame interval (-60 dB over decay_s).
 *   2. tail = blur3(tail)   — diffusion: neighbour mixing, rate scaled by dt so
 *                             the spread speed is frame-rate independent.
 *   3. tail = max(tail, in) — the incoming material re-excites the tail.
 *   4. out  = max(in, tail * mix).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#include "lux_reverb.h"
#include <string.h>
#include <math.h>
#include <sys/time.h>

/* ── Instance pool (mirrors lux_mask.c) ────────────────────────────────────────
 * Slot 0 is g_lux_reverb_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxReverbState g_lux_reverb_proc;
static LuxReverbState s_lux_reverb_extra[CHAIN_MAX_CHAINS - 1];

LuxReverbState *lux_reverb_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_reverb_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_reverb_extra[idx - 1];
}

void lux_reverb_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_reverb_init(lux_reverb_instance(i));
}

/* ── Timestamp helper (same clock as lux_mask) ─────────────────────────────── */
static uint64_t lux_reverb_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxReverbConfig lux_reverb_config_default(void)
{
    LuxReverbConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.background_mode = LUX_REVERB_BG_AUTO;
    cfg.decay_s         = 3.0f;
    cfg.diffusion       = 0.3f;
    cfg.mix             = 0.6f;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines). Long enough to see past a dense opening,
 * short enough that the verdict locks within the first ~0.1 s of stream. */
#define LUX_REVERB_BG_LOCK_LINES 96

void lux_reverb_init(LuxReverbState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(LuxReverbState));
    state->config       = lux_reverb_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    lux_reverb_reset(state);
}

void lux_reverb_reset(LuxReverbState *state)
{
    if (!state) return;
    memset(state->tail_r, 0, sizeof(state->tail_r));
    memset(state->tail_g, 0, sizeof(state->tail_g));
    memset(state->tail_b, 0, sizeof(state->tail_b));
    state->last_frame_ts_us = 0;
    state->last_pixel_count = 0;
    state->tail_active      = 0;
    state->last_bg_resolved = -1;
    /* Re-arm the AUTO learning window — the stream may have changed. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_REVERB_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
}

/* Resolve the background pole for this frame. AUTO learns during a short
 * window after each reset, then LOCKS: the extremes rule decides — on a
 * white-background source even the densest line leaves the max-mean high,
 * on a black-background source even the brightest line leaves it low, so
 * `max + min > 255` picks the pole the stream leans toward. Locking is what
 * matters: a fortissimo passage later must not flip the polarity and wipe
 * the tail (that WAS the "reverb stopped working" regression). */
static int lux_reverb_resolve_bg(LuxReverbState *state,
                                 const uint8_t *in_r, const uint8_t *in_g,
                                 const uint8_t *in_b, int px)
{
    const int mode = state->config.background_mode;
    if (mode == LUX_REVERB_BG_BLACK) return 0;
    if (mode == LUX_REVERB_BG_WHITE) return 1;

    if (state->auto_locked)
        return state->auto_bg_white;

    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        sum += (uint32_t)in_r[i] + in_g[i] + in_b[i];
        n   += 3;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;
    if (mean > state->auto_max_mean) state->auto_max_mean = mean;
    if (mean < state->auto_min_mean) state->auto_min_mean = mean;

    state->auto_bg_white = (state->auto_max_mean + state->auto_min_mean > 255) ? 1 : 0;
    if (--state->auto_lock_countdown <= 0)
        state->auto_locked = 1;
    return state->auto_bg_white;
}

/* ── Frame processing ──────────────────────────────────────────────────────── */

/* Decay + diffuse + inject + mix for one channel. `d` is the per-frame
 * neighbour-mixing amount (0..0.45, stable 3-tap kernel that sums to 1). */
static void lux_reverb_channel(
    float *tail, const uint8_t *in, uint8_t *out,
    int px, int bg_white, float k, float d, float mix)
{
    float prev = 0.0f;   /* pre-blur value of tail[i-1] (edge clamps to 0) */

    for (int i = 0; i < px; i++)
    {
        const float e_in = bg_white ? (float)(255 - in[i]) : (float)in[i];

        /* 1-2. fade then diffuse in place (uses the pre-blur left neighbour so
         * energy doesn't cascade rightwards within a single frame). */
        const float cur   = tail[i] * k;
        const float right = (i + 1 < px) ? tail[i + 1] * k : 0.0f;
        float t = cur * (1.0f - d) + (prev + right) * (0.5f * d);
        prev = cur;

        /* 3. re-excite. */
        if (e_in > t) t = e_in;
        tail[i] = t;

        /* 4. dry/wet blend (max keeps uint8 semantics saturation-free). */
        float e_out = t * mix;
        if (e_in > e_out) e_out = e_in;
        if (e_out > 255.0f) e_out = 255.0f;

        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
    }
}

void lux_reverb_process_frame(
    LuxReverbState *state,
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

    const LuxReverbConfig *cfg = &state->config;
    if (!cfg->enabled || cfg->mix <= 0.001f)
    {
        /* Lazy one-shot clear so a re-enable doesn't resurrect stale matter. */
        if (state->tail_active)
            lux_reverb_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_REVERB_MAX_PIXELS) px = LUX_REVERB_MAX_PIXELS;

    /* Geometry change → the tail no longer maps onto the line: start clean. */
    if (state->last_pixel_count != px && state->tail_active)
        lux_reverb_reset(state);
    state->last_pixel_count = px;
    state->tail_active      = 1;

    /* Polarity flip (mode edit or AUTO re-latch) → the tail's energies live in
     * the other pole: clear them, keep the clock running. */
    const int bg_white = lux_reverb_resolve_bg(state, in_r, in_g, in_b, px);
    if (state->last_bg_resolved != bg_white)
    {
        memset(state->tail_r, 0, sizeof(state->tail_r));
        memset(state->tail_g, 0, sizeof(state->tail_g));
        memset(state->tail_b, 0, sizeof(state->tail_b));
        state->last_bg_resolved = bg_white;
    }

    /* Frame interval (clamped: a stream stall must not flush the tail in one
     * frame, and the very first frame has no reference). */
    const uint64_t now = lux_reverb_get_timestamp_us();
    float dt_s = 0.001f;
    if (state->last_frame_ts_us > 0)
    {
        const uint64_t elapsed = now - state->last_frame_ts_us;
        dt_s = (float)elapsed * 1e-6f;
        if (dt_s < 0.0001f) dt_s = 0.0001f;
        if (dt_s > 0.1f)    dt_s = 0.1f;
    }
    state->last_frame_ts_us = now;

    /* -60 dB over decay_s. */
    const float decay_s = (cfg->decay_s > 0.05f) ? cfg->decay_s : 0.05f;
    const float k = powf(10.0f, -3.0f * dt_s / decay_s);

    /* Diffusion amount per frame, frame-rate independent, kernel-stable. */
    float d = cfg->diffusion * dt_s * 240.0f;
    if (d < 0.0f)   d = 0.0f;
    if (d > 0.45f)  d = 0.45f;

    const float mix = (cfg->mix > 1.0f) ? 1.0f : cfg->mix;

    lux_reverb_channel(state->tail_r, in_r, state->out_r, px, bg_white, k, d, mix);
    lux_reverb_channel(state->tail_g, in_g, state->out_g, px, bg_white, k, d, mix);
    lux_reverb_channel(state->tail_b, in_b, state->out_b, px, bg_white, k, d, mix);

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
