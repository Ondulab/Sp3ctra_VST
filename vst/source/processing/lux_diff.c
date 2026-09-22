/*
 * lux_diff.c
 *
 * LuxDiff — per-pixel reference subtraction implementation (see lux_diff.h).
 *
 * Per-frame pipeline (image thread):
 *   0. requests  — CAPTURE starts a learning window; the window accumulates
 *                  the raw line (per pixel, per channel) and, once full,
 *                  publishes the mean as the reference under the seqlock.
 *                  Runs whether or not the module is enabled.
 *   0b. TRACK     — outside a learning window, the follower lags the line
 *                  (16.16 fixed point, alpha = 1 / track_lines) and rewrites
 *                  the reference from it every line. Also runs bypassed.
 *   1. e_in      = polarity(in), e_ref = polarity(ref)      (bg conversion)
 *   2. d         = e_in - amount * e_ref
 *   3. e_out     = ADD ? max(0, d) : |d|
 *   4. out       = polarity(e_out)
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-09-04
 */

#include "lux_diff.h"
#include <string.h>

/* ── Instance pool (mirrors lux_eq.c) ──────────────────────────────────────────
 * Slot 0 is g_lux_diff_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxDiffState g_lux_diff_proc;
static LuxDiffState s_lux_diff_extra[CHAIN_MAX_CHAINS - 1];

LuxDiffState *lux_diff_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_diff_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_diff_extra[idx - 1];
}

void lux_diff_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_diff_init(lux_diff_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxDiffConfig lux_diff_config_default(void)
{
    LuxDiffConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.amount          = 1.0f;   /* full subtraction */
    cfg.mode            = LUX_DIFF_MODE_ADD;
    cfg.follow          = LUX_DIFF_FOLLOW_HOLD;
    cfg.track_lines     = 256;
    cfg.background_mode = LUX_DIFF_BG_AUTO;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_DIFF_BG_LOCK_LINES 96

void lux_diff_reset(LuxDiffState *state)
{
    if (!state) return;
    state->diff_active = 0;
    /* Re-arm the AUTO learning window. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_DIFF_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    /* The reference, a learning window in flight and the UI guide layers
     * deliberately survive a reset: a bypass/enable toggle (lazy re-arm) or
     * a chain move must never lose the user's capture. Slot loss clears the
     * reference explicitly (lux_diff_clear_reference from the host). */
}

void lux_diff_init(LuxDiffState *state)
{
    if (!state) return;
    state->config = lux_diff_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    memset(state->ui_in_now,  0, sizeof(state->ui_in_now));
    memset(state->ui_in_peak, 0, sizeof(state->ui_in_peak));
    state->ui_in_valid = 0;
    memset(state->ui_ref, 0, sizeof(state->ui_ref));
    state->ui_ref_bg   = -1;
    state->req_capture = 0;
    state->learning    = 0;
    state->learn_left  = 0;
    state->learn_px    = 0;
    state->ref_px      = 0;
    state->ref_valid   = 0;
    state->ref_gen     = 0;
    state->trk_valid   = 0;
    state->trk_px      = 0;
    state->trk_tick    = 0;
    lux_diff_reset(state);
}

/* Resolve the background pole for this frame — mean-based AUTO learn-then-LOCK
 * (mirrors lux_eq/lux_gain; polarity is a property of the SOURCE). */
static int lux_diff_resolve_bg(LuxDiffState *state,
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
    if (mode == LUX_DIFF_BG_BLACK) return 0;
    if (mode == LUX_DIFF_BG_WHITE) return 1;
    if (state->auto_locked)        return state->auto_bg_white;

    if (mean > state->auto_max_mean) state->auto_max_mean = mean;
    if (mean < state->auto_min_mean) state->auto_min_mean = mean;
    state->auto_bg_white = (state->auto_max_mean + state->auto_min_mean > 255) ? 1 : 0;
    if (--state->auto_lock_countdown <= 0)
        state->auto_locked = 1;
    return state->auto_bg_white;
}

/* The pole the reference guide should be built for OUTSIDE process_frame
 * (restore path): the configured pole, or the last learned one. */
static int lux_diff_current_bg(const LuxDiffState *state)
{
    const int mode = state->config.background_mode;
    if (mode == LUX_DIFF_BG_BLACK) return 0;
    if (mode == LUX_DIFF_BG_WHITE) return 1;
    return state->auto_bg_white;
}

/* ── UI guide profiles ─────────────────────────────────────────────────────────
 * Per-bin max of the input energy (luminance), folded EVERY line into two
 * release envelopes (now = fast fall, peak = slow rémanence — the falls are
 * per-line multiplicative so the ballistics scale with the stream's own line
 * rate). Only there to guide the user in the editor. */
#define LUX_DIFF_UI_NOW_FALL  0.97f    /* ~30 lines to fade   */
#define LUX_DIFF_UI_PEAK_FALL 0.999f   /* ~1000 lines ≈ 1-2 s */

static void lux_diff_ui_capture(LuxDiffState *state,
                                const uint8_t *in_r, const uint8_t *in_g,
                                const uint8_t *in_b, int px, int bg_white)
{
    float line[LUX_DIFF_UI_BINS] = { 0 };   /* bin max of THIS line */
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const int   e = bg_white ? 255 - v : v;
        const float m   = (float)e * (1.0f / 255.0f);
        const int   bin = (i * LUX_DIFF_UI_BINS) / px;
        if (m > line[bin]) line[bin] = m;
    }
    for (int b = 0; b < LUX_DIFF_UI_BINS; b++)
    {
        const float now  = state->ui_in_now[b]  * LUX_DIFF_UI_NOW_FALL;
        const float peak = state->ui_in_peak[b] * LUX_DIFF_UI_PEAK_FALL;
        state->ui_in_now[b]  = (line[b] > now)  ? line[b] : now;
        state->ui_in_peak[b] = (line[b] > peak) ? line[b] : peak;
    }
    state->ui_in_valid = 1;
}

/* The reference's own profile — same binning as the live layers so the editor
 * can overlay them. Static: rebuilt only when the reference or pole changes. */
static void lux_diff_build_ui_ref(LuxDiffState *state, int bg_white)
{
    memset(state->ui_ref, 0, sizeof(state->ui_ref));
    const int px = state->ref_px;
    if (px <= 0) { state->ui_ref_bg = bg_white; return; }
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)state->ref_r[i] + state->ref_g[i] + state->ref_b[i]) / 3;
        const int   e = bg_white ? 255 - v : v;
        const float m   = (float)e * (1.0f / 255.0f);
        const int   bin = (i * LUX_DIFF_UI_BINS) / px;
        if (m > state->ui_ref[bin]) state->ui_ref[bin] = m;
    }
    state->ui_ref_bg = bg_white;
}

/* ── Reference control ─────────────────────────────────────────────────────── */
void lux_diff_request_capture(LuxDiffState *state)
{
    if (!state) return;
    state->req_capture = 1;
}

void lux_diff_clear_reference(LuxDiffState *state)
{
    if (!state) return;
    /* Int stores only — safe from any thread. The image thread reads
     * ref_valid once per line, so the subtraction stops on the next line. */
    state->ref_gen++;                 /* odd — writer inside */
    state->ref_valid = 0;
    state->ref_px    = 0;
    memset(state->ui_ref, 0, sizeof(state->ui_ref));
    state->ui_ref_bg = -1;
    state->ref_gen++;                 /* even — published */
    state->trk_valid = 0;             /* TRACK re-seeds from the next line */
}

int lux_diff_copy_reference(const LuxDiffState *state,
                            uint8_t *r, uint8_t *g, uint8_t *b, int cap)
{
    if (!state || !r || !g || !b || cap <= 0) return 0;
    for (int tries = 0; tries < 16; tries++)
    {
        const uint32_t g1 = state->ref_gen;
        if (g1 & 1u) continue;                /* writer inside — retry */
        if (!state->ref_valid) return 0;
        int px = state->ref_px;
        if (px <= 0) return 0;
        if (px > cap) px = cap;
        memcpy(r, state->ref_r, (size_t)px);
        memcpy(g, state->ref_g, (size_t)px);
        memcpy(b, state->ref_b, (size_t)px);
        const uint32_t g2 = state->ref_gen;
        if (g1 == g2) return px;              /* consistent snapshot */
    }
    return 0;                                 /* torn 16× — give up, no ref */
}

void lux_diff_set_reference(LuxDiffState *state,
                            const uint8_t *r, const uint8_t *g,
                            const uint8_t *b, int px)
{
    if (!state) return;
    if (!r || !g || !b || px <= 0) { lux_diff_clear_reference(state); return; }
    if (px > LUX_DIFF_MAX_PIXELS) px = LUX_DIFF_MAX_PIXELS;

    state->ref_gen++;                 /* odd — writer inside */
    state->ref_valid = 0;             /* the image thread bypasses meanwhile */
    memcpy(state->ref_r, r, (size_t)px);
    memcpy(state->ref_g, g, (size_t)px);
    memcpy(state->ref_b, b, (size_t)px);
    state->ref_px    = px;
    lux_diff_build_ui_ref(state, lux_diff_current_bg(state));
    state->ref_valid = 1;
    state->ref_gen++;                 /* even — published */
    state->trk_valid = 0;             /* TRACK re-seeds from this reference */
}

float lux_diff_learn_progress(const LuxDiffState *state)
{
    if (!state || !state->learning) return 0.0f;
    const float left = (float)state->learn_left / (float)LUX_DIFF_LEARN_LINES;
    return 1.0f - (left < 0.0f ? 0.0f : left > 1.0f ? 1.0f : left);
}

/* ── Learning window (image thread) ────────────────────────────────────────── */
static void lux_diff_learn_start(LuxDiffState *state, int px)
{
    state->learning   = 1;
    state->learn_left = LUX_DIFF_LEARN_LINES;
    state->learn_px   = px;
    memset(state->acc_r, 0, sizeof(uint16_t) * (size_t)px);
    memset(state->acc_g, 0, sizeof(uint16_t) * (size_t)px);
    memset(state->acc_b, 0, sizeof(uint16_t) * (size_t)px);
}

static void lux_diff_learn_line(LuxDiffState *state,
                                const uint8_t *in_r, const uint8_t *in_g,
                                const uint8_t *in_b, int px, int bg_white)
{
    if (state->req_capture)
    {
        state->req_capture = 0;
        lux_diff_learn_start(state, px);
    }
    if (!state->learning) return;
    if (px != state->learn_px)       /* width changed mid-window — restart */
        lux_diff_learn_start(state, px);

    for (int i = 0; i < px; i++)
    {
        state->acc_r[i] = (uint16_t)(state->acc_r[i] + in_r[i]);
        state->acc_g[i] = (uint16_t)(state->acc_g[i] + in_g[i]);
        state->acc_b[i] = (uint16_t)(state->acc_b[i] + in_b[i]);
    }
    if (--state->learn_left > 0) return;

    /* Window full — publish the mean as the reference (seqlock). */
    state->ref_gen++;                 /* odd — writer inside */
    for (int i = 0; i < px; i++)
    {
        state->ref_r[i] = (uint8_t)((state->acc_r[i] + LUX_DIFF_LEARN_LINES / 2) / LUX_DIFF_LEARN_LINES);
        state->ref_g[i] = (uint8_t)((state->acc_g[i] + LUX_DIFF_LEARN_LINES / 2) / LUX_DIFF_LEARN_LINES);
        state->ref_b[i] = (uint8_t)((state->acc_b[i] + LUX_DIFF_LEARN_LINES / 2) / LUX_DIFF_LEARN_LINES);
    }
    state->ref_px    = px;
    lux_diff_build_ui_ref(state, bg_white);
    state->ref_valid = 1;
    state->ref_gen++;                 /* even — published */
    state->learning  = 0;
    state->trk_valid = 0;             /* TRACK re-seeds from the fresh mean */
}

/* ── TRACK follower (image thread) ─────────────────────────────────────────── */
static void lux_diff_track_line(LuxDiffState *state,
                                const uint8_t *in_r, const uint8_t *in_g,
                                const uint8_t *in_b, int px, int bg_white)
{
    if (state->config.follow != LUX_DIFF_FOLLOW_TRACK || state->learning)
        return;   /* HOLD, or a CAPTURE window has priority */

    if (!state->trk_valid || state->trk_px != px)
    {
        /* Seed — from the armed reference when it matches the width, else
         * from THIS line (the scene becomes the reference at once). */
        if (state->ref_valid && state->ref_px == px)
        {
            for (int i = 0; i < px; i++)
            {
                state->trk_r[i] = (uint32_t)state->ref_r[i] << 16;
                state->trk_g[i] = (uint32_t)state->ref_g[i] << 16;
                state->trk_b[i] = (uint32_t)state->ref_b[i] << 16;
            }
        }
        else
        {
            state->ref_gen++;             /* odd — writer inside */
            for (int i = 0; i < px; i++)
            {
                state->trk_r[i] = (uint32_t)in_r[i] << 16;
                state->trk_g[i] = (uint32_t)in_g[i] << 16;
                state->trk_b[i] = (uint32_t)in_b[i] << 16;
                state->ref_r[i] = in_r[i];
                state->ref_g[i] = in_g[i];
                state->ref_b[i] = in_b[i];
            }
            state->ref_px    = px;
            lux_diff_build_ui_ref(state, bg_white);
            state->ref_valid = 1;
            state->ref_gen++;             /* even — published */
        }
        state->trk_valid = 1;
        state->trk_px    = px;
        state->trk_tick  = 0;
        return;
    }

    /* First-order lag, alpha = 1 / track_lines, 16.16 fixed point. */
    int T = state->config.track_lines;
    if (T < 1) T = 1;
    if (T > 65536) T = 65536;
    const int64_t a16 = 65536 / T;      /* >= 1 */
    for (int i = 0; i < px; i++)
    {
        int32_t d;
        d = ((int32_t)in_r[i] << 16) - (int32_t)state->trk_r[i];
        state->trk_r[i] = (uint32_t)((int32_t)state->trk_r[i] + (int32_t)(((int64_t)d * a16) >> 16));
        d = ((int32_t)in_g[i] << 16) - (int32_t)state->trk_g[i];
        state->trk_g[i] = (uint32_t)((int32_t)state->trk_g[i] + (int32_t)(((int64_t)d * a16) >> 16));
        d = ((int32_t)in_b[i] << 16) - (int32_t)state->trk_b[i];
        state->trk_b[i] = (uint32_t)((int32_t)state->trk_b[i] + (int32_t)(((int64_t)d * a16) >> 16));
        /* The reference follows — no generation bump: a tracked reference
         * is persisted as a seed only, never as an edit per line. */
        state->ref_r[i] = (uint8_t)((state->trk_r[i] + 32768u) >> 16);
        state->ref_g[i] = (uint8_t)((state->trk_g[i] + 32768u) >> 16);
        state->ref_b[i] = (uint8_t)((state->trk_b[i] + 32768u) >> 16);
    }
    /* Keep the editor's dashed reference alive at a fraction of the line rate. */
    if ((++state->trk_tick & 15u) == 0)
        lux_diff_build_ui_ref(state, bg_white);
}

/* ── Frame processing ──────────────────────────────────────────────────────── */
void lux_diff_process_frame(
    LuxDiffState  *state,
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

    int px = pixel_count;
    if (px > LUX_DIFF_MAX_PIXELS) px = LUX_DIFF_MAX_PIXELS;

    const LuxDiffConfig *cfg = &state->config;

    /* The learning window runs whether or not the block is ON: the user can
     * capture the scene with the block bypassed, then switch it on. */
    lux_diff_learn_line(state, in_r, in_g, in_b, px, lux_diff_current_bg(state));
    lux_diff_track_line(state, in_r, in_g, in_b, px, lux_diff_current_bg(state));

    if (!cfg->enabled)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity. */
        if (state->diff_active)
            lux_diff_reset(state);
        return;
    }

    const int bg_white = lux_diff_resolve_bg(state, in_r, in_g, in_b, px);
    state->diff_active = 1;

    /* Capture BEFORE the identity early-out: the editor must show the live
     * stream whenever the block is ON, even with no reference armed. */
    lux_diff_ui_capture(state, in_r, in_g, in_b, px, bg_white);
    if (state->ref_valid && state->ui_ref_bg != bg_white)
        lux_diff_build_ui_ref(state, bg_white);   /* pole flipped — re-guide */

    if (!state->ref_valid || cfg->amount < 1e-4f)
        return;   /* nothing to subtract — pass-through, the capture above ran */

    const int rpx  = state->ref_px;
    if (rpx <= 0) return;
    const int same = (rpx == px);
    int a_q8 = (int)(cfg->amount * 256.0f + 0.5f);
    if (a_q8 > 256) a_q8 = 256;
    const int abs_mode = (cfg->mode == LUX_DIFF_MODE_ABS);

    int diff = 0;   /* OR of out^in — an unchanged line passes through */
    for (int i = 0; i < px; i++)
    {
        /* Nearest-neighbour resample when the reference was taken at another
         * width (guard only — the chain width is constant in practice). */
        const int j = same ? i : (int)(((uint32_t)i * (uint32_t)rpx) / (uint32_t)px);

        const uint8_t in[3]  = { in_r[i], in_g[i], in_b[i] };
        const uint8_t ref[3] = { state->ref_r[j], state->ref_g[j], state->ref_b[j] };
        uint8_t o[3];
        for (int ch = 0; ch < 3; ch++)
        {
            const int e_in  = bg_white ? 255 - in[ch]  : in[ch];
            const int e_ref = bg_white ? 255 - ref[ch] : ref[ch];
            int d = e_in - ((a_q8 * e_ref) >> 8);
            int e_out = abs_mode ? (d < 0 ? -d : d) : (d < 0 ? 0 : d);
            if (e_out > 255) e_out = 255;
            o[ch] = (uint8_t)(bg_white ? 255 - e_out : e_out);
        }
        state->out_r[i] = o[0];
        state->out_g[i] = o[1];
        state->out_b[i] = o[2];
        diff |= (o[0] ^ in[0]) | (o[1] ^ in[1]) | (o[2] ^ in[2]);
    }

    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
