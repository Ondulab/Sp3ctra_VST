/*
 * lux_echo.c
 *
 * LuxEcho — echo / delay implementation (see lux_echo.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. delayed = ring[write_pos - delay_lines]   (zeros until the ring fills)
 *   2. out     = sat255(in + mix      * delayed)
 *   3. ring[write_pos++] = sat255(in + feedback * delayed)
 *
 * The ring is never memset: reads are gated by `write_pos >= delay`, so a
 * re-anchor (write_pos = 0) makes stale slots unreachable without touching
 * the ~6 MB of history on the RT thread.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#include "lux_echo.h"
#include <string.h>

/* ── Instance pool (mirrors lux_mask.c) ────────────────────────────────────────
 * Slot 0 is g_lux_echo_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxEchoState g_lux_echo_proc;
static LuxEchoState s_lux_echo_extra[CHAIN_MAX_CHAINS - 1];

LuxEchoState *lux_echo_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_echo_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_echo_extra[idx - 1];
}

void lux_echo_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_echo_init(lux_echo_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxEchoConfig lux_echo_config_default(void)
{
    LuxEchoConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.background_mode = LUX_ECHO_BG_AUTO;
    cfg.delay_lines     = 48;
    cfg.feedback        = 0.35f;
    cfg.mix             = 0.6f;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_REVERB_BG_LOCK_LINES. */
#define LUX_ECHO_BG_LOCK_LINES 96

/* Deliberately NO memset of `ring`: the BSS pool is zero at load, re-anchoring
 * write_pos makes old slots unreachable, and clearing ~6 MB per instance would
 * both stall the caller and commit every zero-fill page of unused instances. */
void lux_echo_reset(LuxEchoState *state)
{
    if (!state) return;
    state->write_pos    = 0;
    state->ring_active  = 0;
    state->last_bg_mode = -1;
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_ECHO_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
}

void lux_echo_init(LuxEchoState *state)
{
    if (!state) return;
    state->config = lux_echo_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    lux_echo_reset(state);
}

/* Resolve the background pole for this frame (see lux_reverb_resolve_bg — AUTO
 * learns over a short window then LOCKS: a dense fortissimo line must never
 * flip the polarity and invalidate the ring) and report the background's OWN
 * energy (*out_floor): the paper is never exactly at the zero pole (white ≈
 * 230, energy ≈ 25), and adding that offset onto itself at every repeat would
 * veil the whole image — the ring must carry the MATERIAL energy only (input
 * minus floor). The floor is a slow EMA fed ONLY by near-background lines, so
 * dense passages don't balloon it and shave the material out of the ring. */
static int lux_echo_resolve_bg(LuxEchoState *state,
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
    if (mode == LUX_ECHO_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_ECHO_BG_WHITE) bg_white = 1;
    else if (state->auto_locked)        bg_white = state->auto_bg_white;
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

/* ── Frame processing ──────────────────────────────────────────────────────── */

/* Mix + feedback for one channel. `del`/`del_px` may be NULL/0 (ring not yet
 * filled up to the delay): repeats are silent but the line is still recorded.
 * The ring carries MATERIAL energy only (input minus the background floor), so
 * repeats re-print the strokes without stacking the paper's own level. */
static void lux_echo_channel(
    const uint8_t *in, const uint8_t *del, int del_px,
    uint8_t *out, uint8_t *fb,
    int px, int bg_white, float floor_e, float mix, float feedback)
{
    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        const float e_del = (del && i < del_px) ? (float)del[i] : 0.0f;

        float e_out = e_in + mix * e_del;
        if (e_out > 255.0f) e_out = 255.0f;

        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f) e_mat = 0.0f;
        float e_fb = e_mat + feedback * e_del;
        if (e_fb > 255.0f) e_fb = 255.0f;

        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
        fb[i]  = (uint8_t)e_fb;
    }
}

void lux_echo_process_frame(
    LuxEchoState   *state,
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

    const LuxEchoConfig *cfg = &state->config;
    if (!cfg->enabled || (cfg->mix <= 0.001f && cfg->feedback <= 0.001f))
    {
        /* Lazy one-shot re-anchor so a re-enable doesn't replay stale lines. */
        if (state->ring_active)
            lux_echo_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_ECHO_MAX_PIXELS) px = LUX_ECHO_MAX_PIXELS;

    /* The ring stores energy-space lines: a polarity flip invalidates it.
     * Re-anchor the ring + floor ONLY — a full reset would re-arm the AUTO
     * learning window and throw away the extremes it just learned from. */
    float floor_e = 0.0f;
    const int bg_white = lux_echo_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    if (state->last_bg_mode != bg_white)
    {
        state->write_pos    = 0;
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->ring_active = 1;

    int delay = cfg->delay_lines;
    if (delay < 1)                  delay = 1;
    if (delay > LUX_ECHO_MAX_DELAY) delay = LUX_ECHO_MAX_DELAY;

    const LuxEchoSlot *dslot = NULL;
    if (state->write_pos >= (uint32_t)delay)
        dslot = &state->ring[(state->write_pos - (uint32_t)delay) & LUX_ECHO_RING_MASK];

    LuxEchoSlot *wslot = &state->ring[state->write_pos & LUX_ECHO_RING_MASK];

    const float mix = (cfg->mix > 1.0f) ? 1.0f : cfg->mix;
    float feedback = cfg->feedback;
    if (feedback < 0.0f)   feedback = 0.0f;
    if (feedback > 0.95f)  feedback = 0.95f;

    const int del_px = dslot ? dslot->pixel_count : 0;

    lux_echo_channel(in_r, dslot ? dslot->r : NULL, del_px,
                     state->out_r, wslot->r, px, bg_white, floor_e, mix, feedback);
    lux_echo_channel(in_g, dslot ? dslot->g : NULL, del_px,
                     state->out_g, wslot->g, px, bg_white, floor_e, mix, feedback);
    lux_echo_channel(in_b, dslot ? dslot->b : NULL, del_px,
                     state->out_b, wslot->b, px, bg_white, floor_e, mix, feedback);

    wslot->pixel_count = px;
    state->write_pos++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
