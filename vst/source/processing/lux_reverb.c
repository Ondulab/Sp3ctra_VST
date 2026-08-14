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
 *      (+ flush-to-zero: a fully decayed tail dies instead of lingering as
 *       float dust for tens of seconds.)
 *   3. tail = max(tail, m)  — the MATERIAL energy m = max(0, e_in - floor)
 *                             re-excites the tail (floor = tracked PAPER
 *                             level, 10th-percentile estimator — see
 *                             lux_drive.c), GATED a small margin above zero.
 *                             Storing the ABSOLUTE energy instead let the
 *                             paper pedestal live in the tail and re-print as
 *                             a veil tracking the ink mass (grey-bands bug);
 *                             gating on a FIXED absolute level failed the same
 *                             way — real paper sits above it (white ≈ 230,
 *                             energy ≈ 25).
 *   4. out  = max(e_in, min(e_in, floor) + tail * mix) — the tail prints on
 *      the pixel's OWN pedestal, rounded to nearest (truncation kept a 1-LSB
 *      background pedestal alive ~5 s after the tail became invisible).
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
    state->active_ticks  = 0;   /* seeded HERE, never in reset: a reset must not
                                 * fake a beat the UI would read as activity */
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
    state->tail_peak        = 0.0f;
    state->last_bg_resolved = -1;
    /* Re-arm the AUTO learning window — the stream may have changed. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_REVERB_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
}

/* Resolve the background pole for this frame + report the PAPER's own energy
 * (*out_floor). Polarity: AUTO learns during a short window after each reset,
 * then LOCKS: the extremes rule decides — on a white-background source even
 * the densest line leaves the max-mean high, on a black-background source
 * even the brightest line leaves it low, so `max + min > 255` picks the pole
 * the stream leans toward. Locking is what matters: a fortissimo passage
 * later must not flip the polarity and wipe the tail (that WAS the "reverb
 * stopped working" regression). Paper level: EMA of the per-line
 * 10th-PERCENTILE energy — the canonical grey-bands fix (see
 * lux_drive_resolve_bg): a low percentile finds the paper between the strokes
 * even on dense lines. */
static int lux_reverb_resolve_bg(LuxReverbState *state,
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
    if (mode == LUX_REVERB_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_REVERB_BG_WHITE) bg_white = 1;
    else if (state->auto_locked)          bg_white = state->auto_bg_white;
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

/* ── Frame processing ──────────────────────────────────────────────────────── */

/* Re-excitation gate (energy LSB, 0..255): MATERIAL energy (input minus the
 * tracked paper level) must rise this far to enter the tail. Sensor noise,
 * paper texture and compression artefacts live in the first few LSB above the
 * paper; the DRY path is never gated, so they still pass through for exactly
 * one line, as in the source — the gate only stops the tail from sustaining
 * them. Faint material below the gate simply gets no reverb. (The gate was
 * ABSOLUTE before the floor tracking: real paper sits above 8 — white ≈ 230,
 * energy ≈ 25 — so the whole background lived in the tail.) */
#define LUX_REVERB_EXCITE_FLOOR 8.0f

/* Tail flush threshold (energy LSB): 10× below the 0.5-LSB rounding
 * visibility limit — the tail dies shortly after it stops contributing,
 * instead of decaying through float dust for tens of seconds. */
#define LUX_REVERB_TAIL_EPS 0.05f

/* Decay + diffuse + inject + mix for one channel. `d` is the per-frame
 * neighbour-mixing amount (0..0.45, stable 3-tap kernel that sums to 1).
 * The tail carries MATERIAL energy only (input minus the tracked paper
 * level), and prints on each pixel's OWN pedestal min(e_in, floor) — the
 * paper is never stored into the tail nor re-printed at the estimated floor
 * (grey-bands fix, see lux_drive.c).
 * Returns nonzero when the tail actually altered the line (rack LED).
 * `*peak` accumulates the max POST-update tail energy across channels — what
 * the next line will print, i.e. the runout's "still ringing" measure. */
static int lux_reverb_channel(
    float *tail, const uint8_t *in, uint8_t *out,
    int px, int bg_white, float floor_e, float k, float d, float mix,
    float *peak)
{
    float prev = 0.0f;   /* pre-blur value of tail[i-1] (edge clamps to 0) */
    int   diff = 0;      /* OR of out^in — a silent tail leaves the line intact */

    for (int i = 0; i < px; i++)
    {
        const float e_in = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f) e_mat = 0.0f;

        /* 1-2. fade then diffuse in place (uses the pre-blur left neighbour so
         * energy doesn't cascade rightwards within a single frame). */
        const float cur   = tail[i] * k;
        const float right = (i + 1 < px) ? tail[i + 1] * k : 0.0f;
        float t = cur * (1.0f - d) + (prev + right) * (0.5f * d);
        prev = cur;
        if (t < LUX_REVERB_TAIL_EPS) t = 0.0f;

        /* 3. re-excite with the material only (gated — see
         * LUX_REVERB_EXCITE_FLOOR). */
        if (e_mat > t && e_mat > LUX_REVERB_EXCITE_FLOOR) t = e_mat;
        tail[i] = t;
        if (t > *peak) *peak = t;

        /* 4. dry/wet blend on the pixel's own pedestal (max keeps uint8
         * semantics saturation-free), rounded to nearest so a sub-half-LSB
         * tail is truly invisible. */
        const float e_ped = (e_in < floor_e) ? e_in : floor_e;
        float e_out = e_ped + t * mix;
        if (e_in > e_out) e_out = e_in;
        if (e_out > 255.0f) e_out = 255.0f;

        out[i] = bg_white ? (uint8_t)(255.5f - e_out) : (uint8_t)(e_out + 0.5f);
        diff |= out[i] ^ in[i];
    }
    return diff;
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

    /* Polarity flip (mode edit or AUTO re-latch) → the tail's energies and
     * the learned floor live in the other pole: clear them, keep the clock
     * running. */
    float floor_e = 0.0f;
    const int bg_white = lux_reverb_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    if (state->last_bg_resolved != bg_white)
    {
        memset(state->tail_r, 0, sizeof(state->tail_r));
        memset(state->tail_g, 0, sizeof(state->tail_g));
        memset(state->tail_b, 0, sizeof(state->tail_b));
        state->floor_ema    = -1.0f;
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

    float peak = 0.0f;
    int diff = lux_reverb_channel(state->tail_r, in_r, state->out_r, px, bg_white, floor_e, k, d, mix, &peak);
    diff |= lux_reverb_channel(state->tail_g, in_g, state->out_g, px, bg_white, floor_e, k, d, mix, &peak);
    diff |= lux_reverb_channel(state->tail_b, in_b, state->out_b, px, bg_white, floor_e, k, d, mix, &peak);
    state->tail_peak = peak;
    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}

/* ── Tail runout ───────────────────────────────────────────────────────────── */
int lux_reverb_tail_alive(const LuxReverbState *state)
{
    if (!state) return 0;

    const LuxReverbConfig *cfg = &state->config;
    if (!cfg->enabled || cfg->mix <= 0.001f || !state->tail_active)
        return 0;

    /* Same visibility threshold as the output rounding (out is round-to-
     * nearest, so a wet level below half an LSB prints nothing). */
    const float mix = (cfg->mix > 1.0f) ? 1.0f : cfg->mix;
    return (state->tail_peak * mix >= 0.5f) ? 1 : 0;
}
