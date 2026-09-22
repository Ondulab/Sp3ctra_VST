/*
 * lux_reverb.c
 *
 * LuxReverb — visual reverberation implementation (see lux_reverb.h).
 *
 * The stored tail is a peak envelope of background-subtracted material that
 * fades LINEARLY in energy (a constant LSB/s — the audio decay is exponential
 * through the synth's dB law, see the header) at a per-pixel rate rising
 * toward the treble edge (damping LUT, lux_reverb_damp_rate).
 * Diffusion filters the wet output only: feeding a spatial blur back into the
 * envelope repeatedly spreads a note into unrelated pitches and builds hiss.
 * Each colour tracks its own paper level, with a soft excitation knee to keep
 * sensor noise out of the sustained envelope. The dry image stays intact.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#include "lux_reverb.h"
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
/* clock_gettime/CLOCK_MONOTONIC come from the shim, normally pulled in via
 * <pthread.h>, but this TU is pthread-free so it must be explicit. */
#include "sp3ctra_win_compat.h"
#endif

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

/* Monotonic time: wall-clock corrections must not change the decay. */
static uint64_t lux_reverb_get_timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
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
    cfg.damping         = 0.5f;   /* treble edge fades 8× faster */
    cfg.damp_type       = LUX_REVERB_DAMP_AIR;
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
    state->damp_key_px   = -1;  /* LUT built on the first processed line */
    lux_reverb_reset(state);
}

/* Per-pixel fade-rate LUT — rebuilt only when the law, its strength, the
 * line geometry or the axis span moved (≤ 8192 exp2f, never per line). */
static void lux_reverb_build_damp(LuxReverbState *state, int px, float span_oct)
{
    float amount = state->config.damping;
    if (!(amount > 0.0f)) amount = 0.0f;
    if (amount > 1.0f)    amount = 1.0f;
    const int type = state->config.damp_type;
    if (state->damp_key_px == px && state->damp_key_type == type
        && state->damp_key_amount == amount && state->damp_key_span == span_oct)
        return;
    const float inv = (px > 1) ? 1.0f / (float)(px - 1) : 0.0f;
    for (int i = 0; i < px; ++i)
        state->damp_rate[i] = lux_reverb_damp_rate(type, amount, (float)i * inv, span_oct);
    state->damp_key_px     = px;
    state->damp_key_type   = type;
    state->damp_key_amount = amount;
    state->damp_key_span   = span_oct;
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
    for (int c = 0; c < 3; ++c) state->floor_ema[c] = -1.0f;
}

/* Learn the source polarity briefly, then lock it until reset. Dense musical
 * passages must not flip the pole or wipe the tail. */
static int lux_reverb_resolve_bg(LuxReverbState *state,
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

    return bg_white;
}

/* Exact per-colour 10th percentile. RGB averaging mistakes a tinted paper or
 * a channel offset for material; coarse histogram bins add up to 4 LSB error.
 * Inspect every pixel to avoid aliasing periodic sensor patterns. An upward
 * background step is followed immediately so it cannot excite a full-width
 * tail. Downward movement is smoothed over 16 ms, independent of line rate. */
static float lux_reverb_floor(const uint8_t *in, int px, int bg_white,
                              float *tracked, float follow)
{
    int hist[256] = { 0 };
    for (int i = 0; i < px; ++i)
        ++hist[bg_white ? 255 - in[i] : in[i]];
    int acc = 0, level = 0;
    for (; level < 255; ++level)
    {
        acc += hist[level];
        if (acc > px / 10) break;
    }
    const float floor = (float)level;
    if (*tracked < 0.0f || floor > *tracked)
        *tracked = floor;
    else
        *tracked += (floor - *tracked) * follow;
    return *tracked;
}

/* ── Frame processing ──────────────────────────────────────────────────────── */

/* Only the wet excitation is gated. Smoothstep over 8..16 LSB avoids the
 * former hard jump from zero to 9 LSB when a noisy pixel crosses the gate. */
#define LUX_REVERB_EXCITE_FLOOR 8.0f

static float lux_reverb_excitation(float material)
{
    if (material <= LUX_REVERB_EXCITE_FLOOR) return 0.0f;
    if (material >= 2.0f * LUX_REVERB_EXCITE_FLOOR) return material;
    const float x = (material - LUX_REVERB_EXCITE_FLOOR) / LUX_REVERB_EXCITE_FLOOR;
    return material * x * x * (3.0f - 2.0f * x);
}

/* Fade and excite at the original pitches, then diffuse the wet readout.
 * `step` = the energy lost this line by an undamped pixel (255 · dt / decay);
 * `damp` scales it per pixel. A tail that reaches zero IS zero — no float
 * dust, no flush threshold. The normalized three-tap readout has bounded
 * width at every decay setting and line rate. It never gets fed back into
 * the stored tail. */
static int lux_reverb_channel(
    float *tail, const uint8_t *in, uint8_t *out,
    int px, int bg_white, float floor_e, float step, const float *damp,
    float d, float mix, float *peak)
{
    int diff = 0;
    for (int i = 0; i < px; ++i)
    {
        const float e_in = bg_white ? (float)(255 - in[i]) : (float)in[i];
        const float excitation = lux_reverb_excitation(e_in - floor_e);
        float t = tail[i] - step * damp[i];
        if (t < 0.0f) t = 0.0f;
        if (excitation > t) t = excitation;
        tail[i] = t;
    }

    for (int i = 0; i < px; ++i)
    {
        const float e_in = bg_white ? (float)(255 - in[i]) : (float)in[i];
        const float left = i > 0 ? tail[i - 1] : 0.0f;
        const float right = i + 1 < px ? tail[i + 1] : 0.0f;
        const float t = tail[i] * (1.0f - d) + (left + right) * (0.5f * d);
        if (t > *peak) *peak = t;

        /* Dry/wet blend on the pixel's own pedestal (max keeps uint8
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

void lux_reverb_process_frame_at(
    LuxReverbState *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    uint64_t        now,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
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
    const int had_frame = state->tail_active;
    state->last_pixel_count = px;
    state->tail_active      = 1;

    /* Use elapsed monotonic time without a minimum dt: a burst of queued
     * lines must not age the tail faster than real time. A stream stall must
     * age it fully, not revive an almost unchanged envelope on resumption. */
    float dt_s = 0.001f;
    if (had_frame)
    {
        if (now < state->last_frame_ts_us) now = state->last_frame_ts_us;
        dt_s = (float)(now - state->last_frame_ts_us) * 1e-6f;
    }
    state->last_frame_ts_us = now;

    const int bg_white = lux_reverb_resolve_bg(state, in_r, in_g, in_b, px);
    if (state->last_bg_resolved != bg_white)
    {
        memset(state->tail_r, 0, sizeof(state->tail_r));
        memset(state->tail_g, 0, sizeof(state->tail_g));
        memset(state->tail_b, 0, sizeof(state->tail_b));
        for (int c = 0; c < 3; ++c) state->floor_ema[c] = -1.0f;
        state->last_bg_resolved = bg_white;
    }
    const float follow = -expm1f(-dt_s / 0.016f);
    const float floor_r = lux_reverb_floor(in_r, px, bg_white, &state->floor_ema[0], follow);
    const float floor_g = lux_reverb_floor(in_g, px, bg_white, &state->floor_ema[1], follow);
    const float floor_b = lux_reverb_floor(in_b, px, bg_white, &state->floor_ema[2], follow);

    /* Full scale (255) to the pole over decay_s, linearly. */
    const float decay_s = (cfg->decay_s > 0.05f) ? cfg->decay_s : 0.05f;
    const float step = 255.0f * dt_s / decay_s;

    /* Per-pixel fade rates for the damping law (cached). */
    const float span_oct = (luxstral_num_octaves > 0) ? (float)luxstral_num_octaves : 8.0f;
    lux_reverb_build_damp(state, px, span_oct);
    const float *damp = state->damp_rate;

    /* A fixed, bounded wet spread, independent of the processing cadence. */
    float d = cfg->diffusion * 0.45f;
    if (d < 0.0f) d = 0.0f;
    if (d > 0.45f) d = 0.45f;

    const float mix = (cfg->mix > 1.0f) ? 1.0f : cfg->mix;

    float peak = 0.0f;
    int diff = lux_reverb_channel(state->tail_r, in_r, state->out_r, px, bg_white, floor_r, step, damp, d, mix, &peak);
    diff |= lux_reverb_channel(state->tail_g, in_g, state->out_g, px, bg_white, floor_g, step, damp, d, mix, &peak);
    diff |= lux_reverb_channel(state->tail_b, in_b, state->out_b, px, bg_white, floor_b, step, damp, d, mix, &peak);
    state->tail_peak = peak;
    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}

/* Production entry point; explicit-time entry above also supports offline
 * rendering/tests without sleeping or depending on scheduler jitter. */
void lux_reverb_process_frame(
    LuxReverbState *state,
    const uint8_t *in_r, const uint8_t *in_g, const uint8_t *in_b,
    int pixel_count, int luxstral_num_octaves,
    const uint8_t **out_r, const uint8_t **out_g, const uint8_t **out_b)
{
    lux_reverb_process_frame_at(state, in_r, in_g, in_b,
        pixel_count, luxstral_num_octaves, lux_reverb_get_timestamp_us(),
        out_r, out_g, out_b);
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
