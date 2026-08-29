/*
 * lux_gain.c
 *
 * LuxGain — per-line energy gain implementation (see lux_gain.h).
 *
 * Per-frame pipeline (energy space):
 *   1. e_in  = polarity(in)                    (bg conversion)
 *   2. e_out = jointclip(e_in * gain)          (ONE clip factor / 3 channels)
 *   3. out   = polarity(e_out)
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-08-16
 */

#include "lux_gain.h"
#include <math.h>
#include <string.h>

/* ── Instance pool (mirrors lux_eq.c) ──────────────────────────────────────────
 * Slot 0 is g_lux_gain_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxGainState g_lux_gain_proc;
static LuxGainState s_lux_gain_extra[CHAIN_MAX_CHAINS - 1];

LuxGainState *lux_gain_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_gain_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_gain_extra[idx - 1];
}

void lux_gain_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_gain_init(lux_gain_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxGainConfig lux_gain_config_default(void)
{
    LuxGainConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.gain_lin        = 1.0f;   /* unity — pass-through */
    cfg.background_mode = LUX_GAIN_BG_AUTO;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_GAIN_BG_LOCK_LINES 96

void lux_gain_reset(LuxGainState *state)
{
    if (!state) return;
    state->gain_active = 0;
    /* Re-arm the AUTO learning window. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_GAIN_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    /* The UI guide layers deliberately survive a reset — a brief
     * disable/enable keeps the rémanence on screen. */
}

void lux_gain_init(LuxGainState *state)
{
    if (!state) return;
    state->config = lux_gain_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    memset(state->ui_in_now,  0, sizeof(state->ui_in_now));
    memset(state->ui_in_peak, 0, sizeof(state->ui_in_peak));
    state->ui_in_valid = 0;
    lux_gain_reset(state);
}

/* Resolve the background pole for this frame — mean-based AUTO learn-then-LOCK
 * (mirrors lux_eq/lux_drive; polarity is a property of the SOURCE). */
static int lux_gain_resolve_bg(LuxGainState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int px)
{
    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        sum += (uint32_t)in_r[i] + in_g[i] + in_b[i];
        n   += 3;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;

    const int mode = state->config.background_mode;
    if (mode == LUX_GAIN_BG_BLACK) return 0;
    if (mode == LUX_GAIN_BG_WHITE) return 1;
    if (state->auto_locked)        return state->auto_bg_white;

    if (mean > state->auto_max_mean) state->auto_max_mean = mean;
    if (mean < state->auto_min_mean) state->auto_min_mean = mean;
    state->auto_bg_white = (state->auto_max_mean + state->auto_min_mean > 255) ? 1 : 0;
    if (--state->auto_lock_countdown <= 0)
        state->auto_locked = 1;
    return state->auto_bg_white;
}

/* ── UI guide profile ──────────────────────────────────────────────────────────
 * Per-bin max of the input energy (luminance), folded EVERY line into two
 * release envelopes (now = fast fall, peak = slow rémanence — the falls are
 * per-line multiplicative so the ballistics scale with the stream's own line
 * rate). Only there to guide the user in the editor. */
#define LUX_GAIN_UI_NOW_FALL  0.97f    /* ~30 lines to fade   */
#define LUX_GAIN_UI_PEAK_FALL 0.999f   /* ~1000 lines ≈ 1-2 s */

static void lux_gain_ui_capture(LuxGainState *state,
                                const uint8_t *in_r, const uint8_t *in_g,
                                const uint8_t *in_b, int px, int bg_white)
{
    float line[LUX_GAIN_UI_BINS] = { 0 };   /* bin max of THIS line */
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const int   e = bg_white ? 255 - v : v;
        const float m   = (float)e * (1.0f / 255.0f);
        const int   bin = (i * LUX_GAIN_UI_BINS) / px;
        if (m > line[bin]) line[bin] = m;
    }
    for (int b = 0; b < LUX_GAIN_UI_BINS; b++)
    {
        const float now  = state->ui_in_now[b]  * LUX_GAIN_UI_NOW_FALL;
        const float peak = state->ui_in_peak[b] * LUX_GAIN_UI_PEAK_FALL;
        state->ui_in_now[b]  = (line[b] > now)  ? line[b] : now;
        state->ui_in_peak[b] = (line[b] > peak) ? line[b] : peak;
    }
    state->ui_in_valid = 1;
}

void lux_gain_process_frame(
    LuxGainState  *state,
    const uint8_t *in_r,
    const uint8_t *in_g,
    const uint8_t *in_b,
    int            pixel_count,
    int            luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    (void)luxstral_num_octaves;

    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxGainConfig *cfg = &state->config;
    if (!cfg->enabled)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity. */
        if (state->gain_active)
            lux_gain_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_GAIN_MAX_PIXELS) px = LUX_GAIN_MAX_PIXELS;

    const int bg_white = lux_gain_resolve_bg(state, in_r, in_g, in_b, px);
    state->gain_active = 1;

    /* Capture BEFORE the identity early-out: the editor must show the live
     * stream whenever the block is ON, even at unity gain. */
    lux_gain_ui_capture(state, in_r, in_g, in_b, px, bg_white);

    const float g = cfg->gain_lin;
    if (fabsf(g - 1.0f) < 1e-3f)
        return;   /* unity — pass-through, the capture above already ran */

    int diff = 0;   /* OR of out^in — an all-background line passes through */
    for (int i = 0; i < px; i++)
    {
        /* ONE joint clip factor across the channels: a boost can push a
         * channel past full scale, and clipping each channel independently
         * would crush the ratio between them — a coloured line saturating
         * grey. Scaling all three by the same factor saturates toward the
         * line's own hue instead (the lux_centro compose rule). */
        float mat[3];
        mat[0] = (float)(bg_white ? 255 - in_r[i] : in_r[i]) * g;
        mat[1] = (float)(bg_white ? 255 - in_g[i] : in_g[i]) * g;
        mat[2] = (float)(bg_white ? 255 - in_b[i] : in_b[i]) * g;

        float t = 1.0f;
        for (int ch = 0; ch < 3; ch++)
            if (mat[ch] > 255.0f)
            {
                const float tc = 255.0f / mat[ch];
                if (tc < t) t = tc;
            }

        uint8_t o[3];
        for (int ch = 0; ch < 3; ch++)
        {
            float e_out = t * mat[ch];
            if (e_out > 255.0f) e_out = 255.0f;
            if (e_out < 0.0f)   e_out = 0.0f;
            const int q = (int)(e_out + 0.5f);
            o[ch] = (uint8_t)(bg_white ? 255 - q : q);
        }
        state->out_r[i] = o[0];
        state->out_g[i] = o[1];
        state->out_b[i] = o[2];
        diff |= (o[0] ^ in_r[i]) | (o[1] ^ in_g[i]) | (o[2] ^ in_b[i]);
    }

    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
