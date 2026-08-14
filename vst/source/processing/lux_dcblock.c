/*
 * lux_dcblock.c
 *
 * LuxDcBlock — per-line mean removal implementation (see lux_dcblock.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. e_in   = polarity(in)                     (bg conversion)
 *   2. mean_c = Σ e_in / N                       (per channel — full line)
 *   3. e_out  = clamp(e_in - amount * mean_c)    (toward the background only)
 *   4. out    = polarity(e_out)
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-08-04
 */

#include "lux_dcblock.h"
#include <string.h>

/* ── Instance pool (mirrors lux_eq.c) ──────────────────────────────────────────
 * Slot 0 is g_lux_dcblock_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxDcBlockState g_lux_dcblock_proc;
static LuxDcBlockState s_lux_dcblock_extra[CHAIN_MAX_CHAINS - 1];

LuxDcBlockState *lux_dcblock_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_dcblock_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_dcblock_extra[idx - 1];
}

void lux_dcblock_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_dcblock_init(lux_dcblock_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxDcBlockConfig lux_dcblock_config_default(void)
{
    LuxDcBlockConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.amount          = 1.0f;   /* full removal — the OUT sends' DC Blocking */
    cfg.background_mode = LUX_DCBLOCK_BG_AUTO;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_DCBLOCK_BG_LOCK_LINES 96

void lux_dcblock_reset(LuxDcBlockState *state)
{
    if (!state) return;
    state->dc_active = 0;
    /* Re-arm the AUTO learning window. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_DCBLOCK_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    /* The UI guide layers (and the DC line) deliberately survive a reset — a
     * brief disable/enable keeps the rémanence on screen. */
}

void lux_dcblock_init(LuxDcBlockState *state)
{
    if (!state) return;
    state->config = lux_dcblock_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    memset(state->ui_in_now,  0, sizeof(state->ui_in_now));
    memset(state->ui_in_peak, 0, sizeof(state->ui_in_peak));
    state->ui_in_valid = 0;
    state->ui_dc       = 0.0f;
    lux_dcblock_reset(state);
}

/* Resolve the background pole for this frame — mean-based AUTO learn-then-LOCK
 * (mirrors lux_eq/lux_drive; polarity is a property of the SOURCE). */
static int lux_dcblock_resolve_bg(LuxDcBlockState *state,
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
    if (mode == LUX_DCBLOCK_BG_BLACK) return 0;
    if (mode == LUX_DCBLOCK_BG_WHITE) return 1;
    if (state->auto_locked)           return state->auto_bg_white;

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
#define LUX_DCBLOCK_UI_NOW_FALL  0.97f    /* ~30 lines to fade   */
#define LUX_DCBLOCK_UI_PEAK_FALL 0.999f   /* ~1000 lines ≈ 1-2 s */

static void lux_dcblock_ui_capture(LuxDcBlockState *state,
                                   const uint8_t *in_r, const uint8_t *in_g,
                                   const uint8_t *in_b, int px, int bg_white)
{
    float line[LUX_DCBLOCK_UI_BINS] = { 0 };   /* bin max of THIS line */
    uint32_t sum = 0;
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const int   e = bg_white ? 255 - v : v;
        sum += (uint32_t)e;
        const float m   = (float)e * (1.0f / 255.0f);
        const int   bin = (i * LUX_DCBLOCK_UI_BINS) / px;
        if (m > line[bin]) line[bin] = m;
    }
    for (int b = 0; b < LUX_DCBLOCK_UI_BINS; b++)
    {
        const float now  = state->ui_in_now[b]  * LUX_DCBLOCK_UI_NOW_FALL;
        const float peak = state->ui_in_peak[b] * LUX_DCBLOCK_UI_PEAK_FALL;
        state->ui_in_now[b]  = (line[b] > now)  ? line[b] : now;
        state->ui_in_peak[b] = (line[b] > peak) ? line[b] : peak;
    }
    /* Smoothed luminance DC — the editor's reference line. */
    const float dc = (px > 0) ? (float)sum / ((float)px * 255.0f) : 0.0f;
    state->ui_dc += (dc - state->ui_dc) * (1.0f / 16.0f);
    state->ui_in_valid = 1;
}

/* Subtract `d` (energy units) from one channel. Background pixels (energy 0)
 * pass through bit-identical — the subtraction only pushes toward the pole.
 * Returns nonzero when the pass actually altered the line (rack LED). */
static int lux_dcblock_channel(const uint8_t *in, uint8_t *out, int px,
                               int bg_white, int d)
{
    int diff = 0;   /* OR of out^in — a DC-free line passes through */

    for (int i = 0; i < px; i++)
    {
        const int e_in  = bg_white ? 255 - in[i] : in[i];
        int       e_out = e_in - d;
        if (e_out < 0) e_out = 0;
        out[i] = (uint8_t)(bg_white ? 255 - e_out : e_out);
        diff  |= out[i] ^ in[i];
    }
    return diff;
}

void lux_dcblock_process_frame(
    LuxDcBlockState *state,
    const uint8_t   *in_r,
    const uint8_t   *in_g,
    const uint8_t   *in_b,
    int              pixel_count,
    int              luxstral_num_octaves,
    const uint8_t  **out_r,
    const uint8_t  **out_g,
    const uint8_t  **out_b)
{
    (void)luxstral_num_octaves;

    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxDcBlockConfig *cfg = &state->config;
    if (!cfg->enabled || cfg->amount < 1e-4f)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity. */
        if (state->dc_active)
            lux_dcblock_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_DCBLOCK_MAX_PIXELS) px = LUX_DCBLOCK_MAX_PIXELS;

    const int bg_white = lux_dcblock_resolve_bg(state, in_r, in_g, in_b, px);
    state->dc_active = 1;

    lux_dcblock_ui_capture(state, in_r, in_g, in_b, px, bg_white);

    /* Per-channel mean energy of the FULL line — each channel loses its own
     * continuous component (a uniform colour wash is DC too). */
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < px; i++)
    {
        sr += (uint32_t)(bg_white ? 255 - in_r[i] : in_r[i]);
        sg += (uint32_t)(bg_white ? 255 - in_g[i] : in_g[i]);
        sb += (uint32_t)(bg_white ? 255 - in_b[i] : in_b[i]);
    }
    /* Rounded once per line — a per-pixel constant, so integer math stays
     * exact and the identity case (amount ≈ 0 or empty line) costs nothing. */
    const float a  = cfg->amount;
    const int   dr = (int)(a * ((float)sr / (float)px) + 0.5f);
    const int   dg = (int)(a * ((float)sg / (float)px) + 0.5f);
    const int   db = (int)(a * ((float)sb / (float)px) + 0.5f);

    if ((dr | dg | db) == 0)
        return;   /* background-only line — nothing to remove, pass through */

    int diff = lux_dcblock_channel(in_r, state->out_r, px, bg_white, dr);
    diff |= lux_dcblock_channel(in_g, state->out_g, px, bg_white, dg);
    diff |= lux_dcblock_channel(in_b, state->out_b, px, bg_white, db);

    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
