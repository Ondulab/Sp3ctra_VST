/*
 * lux_centro.c
 *
 * LuxCentro (CENTROID) — mass-to-barycentre simplifier (see lux_centro.h).
 *
 * Per-frame pipeline (energy space):
 *   1. e_in   = polarity(in)                          (bg conversion)
 *   2. FLOOR  : material = e_in - floor_ema; pixels with luminance material
 *               <= floor_level*255 are floor — the runs above it are the
 *               MASSES ("toute masse entre deux fonds")
 *   3. REDRAW : each mass → one window of `thickness_px` centred on its
 *               luminance barycentre, edge shaped by edge_soft (square →
 *               smoothed), per-channel energy preserved (amp = mass / area)
 *   4. EQ     : optional output gain curve on the composed material (shared
 *               LuxEq spline sampled into a per-pixel LUT; flat = bypass)
 *   5. out    = polarity(clamp((e_in - material) + eq[x] * ((1-s)*material
 *               + s*redraw))) — each pixel keeps its OWN pedestal, the
 *               background is never re-printed at the estimated floor
 *               (grey-bands fix, see lux_drive.c)
 *
 * RT-safety: Pure C, allocation-free, bounded O(N + masses * window).
 *
 * Author: zhonx
 * Created: 2026-08-03
 */

#include "lux_centro.h"
#include <math.h>
#include <string.h>

/* ── Instance pool (mirrors lux_harmo.c) ───────────────────────────────────────
 * Slot 0 is g_lux_centro_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxCentroState g_lux_centro_proc;
static LuxCentroState s_lux_centro_extra[CHAIN_MAX_CHAINS - 1];

LuxCentroState *lux_centro_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_centro_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_centro_extra[idx - 1];
}

void lux_centro_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_centro_init(lux_centro_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxCentroConfig lux_centro_config_default(void)
{
    LuxCentroConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.floor_level     = 0.10f;
    cfg.thickness_px    = 6.0f;
    cfg.edge_soft       = 0.0f;                 /* square edges */
    cfg.background_mode = LUX_CENTRO_BG_AUTO;
    cfg.eq_num_bands    = 2;   /* one straight line — matches the UI default */
    /* eq_band_gain_db[] all 0 dB — flat curve = output EQ bypassed */
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_CENTRO_BG_LOCK_LINES 96

void lux_centro_reset(LuxCentroState *state)
{
    if (!state) return;
    state->centro_active = 0;
    state->last_bg_mode  = -1;
    state->num_segs      = 0;
    state->eq_lut_px     = 0;   /* invalidate the output-EQ gain LUT */
    state->eq_lut_bands  = 0;
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_CENTRO_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
    /* The UI guide layers deliberately survive a reset — a brief
     * disable/enable keeps the rémanence on screen. */
}

void lux_centro_init(LuxCentroState *state)
{
    if (!state) return;
    state->config = lux_centro_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    memset(state->ui_in_now,  0, sizeof(state->ui_in_now));
    memset(state->ui_in_peak, 0, sizeof(state->ui_in_peak));
    state->ui_in_valid = 0;
    lux_centro_reset(state);
}

/* Resolve the background pole for this frame + report the PAPER's own energy
 * (*out_floor). Polarity: mean-based AUTO learn-then-LOCK. Paper level: EMA
 * of the per-line 10th-PERCENTILE energy — the canonical grey-bands fix (see
 * lux_drive_resolve_bg). The previous sampled-minimum estimator froze behind
 * its "line is background" gate and seeded at 0 on dense passages, so the
 * pedestal/material split drifted with the content. */
static int lux_centro_resolve_bg(LuxCentroState *state,
                                 const uint8_t *in_r, const uint8_t *in_g,
                                 const uint8_t *in_b, int px, float *out_floor)
{
    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        sum += (uint32_t)(((int)in_r[i] + in_g[i] + in_b[i]) / 3);
        n   += 1;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;

    int bg_white;
    const int mode = state->config.background_mode;
    if (mode == LUX_CENTRO_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_CENTRO_BG_WHITE) bg_white = 1;
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

/* ── Output EQ (shared LuxEq curve) ────────────────────────────────────────────
 * Applied AFTER the redraw, on the composed output material — exactly a LuxEq
 * insert chained behind the module, sharing its floor tracking. */

static int lux_centro_eq_active_bands(const LuxCentroConfig *cfg)
{
    int n = cfg->eq_num_bands;
    if (n < 2)                n = 2;
    if (n > LUX_EQ_NUM_BANDS) n = LUX_EQ_NUM_BANDS;
    return n;
}

/* Nonzero when at least one active node is off 0 dB (flat = bypass). */
static int lux_centro_eq_shaping(const LuxCentroConfig *cfg)
{
    const int n = lux_centro_eq_active_bands(cfg);
    for (int b = 0; b < n; ++b)
        if (fabsf(cfg->eq_band_gain_db[b]) > 0.01f)
            return 1;
    return 0;
}

/* Rebuild the per-pixel linear-gain LUT when a band value, the node count or
 * the width changed — same LUT as lux_eq_update_lut, sampling the SHARED
 * Catmull-Rom spline (lux_eq_curve_db) so the CENTROID output EQ and the
 * LuxEq module apply the exact same curve. */
static void lux_centro_eq_update_lut(LuxCentroState *state, int px)
{
    const int n = lux_centro_eq_active_bands(&state->config);

    int dirty = (state->eq_lut_px != px) || (state->eq_lut_bands != n);
    for (int b = 0; b < n && !dirty; ++b)
        if (state->eq_lut_gains[b] != state->config.eq_band_gain_db[b])
            dirty = 1;
    if (!dirty)
        return;

    for (int b = 0; b < n; ++b)
        state->eq_lut_gains[b] = state->config.eq_band_gain_db[b];
    state->eq_lut_bands = n;
    state->eq_lut_px    = px;

    const float span = (px > 1) ? (float)(px - 1) : 1.0f;
    for (int i = 0; i < px; ++i)
    {
        const float x  = ((float)i / span) * (float)(n - 1);
        const float db = lux_eq_curve_db(state->eq_lut_gains, n, x);
        state->eq_lut[i] = powf(10.0f, db * (1.0f / 20.0f));
    }
}

/* ── UI guide profile ──────────────────────────────────────────────────────────
 * Per-bin max of the input material energy (luminance, above the tracked
 * background floor), folded EVERY line into two release envelopes:
 *   now  — fast fall: the stream as it breathes (shows the lows),
 *   peak — slow fall: rémanence of the recent maxima (shows the highs).
 * The falls are per-line multiplicative, so the ballistics scale with the
 * stream's own line rate. Only there to guide the user in the editor
 * (mirrors lux_drive.c). */
#define LUX_CENTRO_UI_NOW_FALL  0.97f    /* ~30 lines to fade   */
#define LUX_CENTRO_UI_PEAK_FALL 0.999f   /* ~1000 lines ≈ 1-2 s */

static void lux_centro_ui_capture(LuxCentroState *state,
                                  const uint8_t *in_r, const uint8_t *in_g,
                                  const uint8_t *in_b, int px,
                                  int bg_white, float floor_e)
{
    float line[LUX_CENTRO_UI_BINS] = { 0 };   /* bin max of THIS line */
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const float e = bg_white ? (float)(255 - v) : (float)v;
        float m = (e - floor_e) * (1.0f / 255.0f);
        if (m <= 0.0f) continue;
        if (m > 1.0f)  m = 1.0f;
        const int bin = (i * LUX_CENTRO_UI_BINS) / px;
        if (m > line[bin]) line[bin] = m;
    }
    for (int b = 0; b < LUX_CENTRO_UI_BINS; b++)
    {
        const float now  = state->ui_in_now[b]  * LUX_CENTRO_UI_NOW_FALL;
        const float peak = state->ui_in_peak[b] * LUX_CENTRO_UI_PEAK_FALL;
        state->ui_in_now[b]  = (line[b] > now)  ? line[b] : now;
        state->ui_in_peak[b] = (line[b] > peak) ? line[b] : peak;
    }
    state->ui_in_valid = 1;
}

/* Edge window of the redrawn line: 1 on the plateau (d <= p), smoothstep
 * down to 0 over the soft skirt (p < d < h). The caller builds p/h as a
 * PIVOT around the half-thickness point c: p = c - skirt, h = c + skirt —
 * the half-height point stays at c, so the equivalent width (the profile's
 * integral, (p + h) / 2 * 2 = 2c) is softness-invariant. */
static inline float lux_centro_edge_win(float d, float h, float p)
{
    if (d <= p) return 1.0f;
    if (d >= h) return 0.0f;
    const float t = (d - p) / (h - p);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* FLOOR + mass pass: split the line into masses (runs of luminance material
 * above the écrêtage threshold), collecting the luminance barycentre of
 * each plus its colour reference — the raw background-relative energies of
 * the pixel with the strongest material luminance (the mass's most
 * significant sample: strong black, strong colour…). */
static void lux_centro_find_masses(LuxCentroState *state,
                                   const uint8_t *in_r, const uint8_t *in_g,
                                   const uint8_t *in_b, int px,
                                   int bg_white, float floor_e, float thr)
{
    int   n      = 0;
    int   open   = 0;     /* a mass is being accumulated */
    int   a      = 0;
    float wsum   = 0.0f;  /* Σ luminance material            */
    float wxsum  = 0.0f;  /* Σ position * luminance material */
    float best   = -1.0f; /* peak material luminance of the open mass */
    float c_r = 0.0f, c_g = 0.0f, c_b = 0.0f;

    for (int i = 0; i <= px; i++)
    {
        float lum = -1.0f;
        float rr = 0.0f, rg = 0.0f, rb = 0.0f;   /* raw bg-relative energy */
        if (i < px)
        {
            rr = bg_white ? (float)(255 - in_r[i]) : (float)in_r[i];
            rg = bg_white ? (float)(255 - in_g[i]) : (float)in_g[i];
            rb = bg_white ? (float)(255 - in_b[i]) : (float)in_b[i];
            float er = rr - floor_e, eg = rg - floor_e, eb = rb - floor_e;
            if (er < 0.0f) er = 0.0f;
            if (eg < 0.0f) eg = 0.0f;
            if (eb < 0.0f) eb = 0.0f;
            lum = (er + eg + eb) * (1.0f / 3.0f);
        }

        if (lum > thr)   /* material pixel — open/extend the current mass */
        {
            if (!open) { open = 1; a = i; wsum = wxsum = 0.0f; best = -1.0f; }
            wsum  += lum;
            wxsum += lum * (float)i;
            if (lum > best) { best = lum; c_r = rr; c_g = rg; c_b = rb; }
        }
        else if (open)   /* floor pixel (or end of line) — close the mass */
        {
            open = 0;
            if (n < LUX_CENTRO_MAX_SEGMENTS && wsum > 0.0f)
            {
                state->seg_a[n]      = a;
                state->seg_b[n]      = i - 1;
                state->seg_pos[n]    = wxsum / wsum;
                state->seg_col[0][n] = c_r;
                state->seg_col[1][n] = c_g;
                state->seg_col[2][n] = c_b;
                n++;
            }
        }
    }
    state->num_segs = n;
}

/* REDRAW pass: print each mass as one line centred on its barycentre, into
 * the per-channel accumulators, at the mass's colour reference (its most
 * significant sample) — constant colour at any thickness.
 *
 * Geometry: c = half the EQUIVALENT width; the soft skirt pivots around c
 * (p = c - skirt, h = c + skirt) so softening never changes the equivalent
 * width. The skirt floor of 1 px lets a 1-px line soften into a real bump
 * instead of keeping a hard centre pixel with a fixed one-pixel gradient. */
static void lux_centro_redraw(LuxCentroState *state, int px,
                              float thickness, float edge_soft)
{
    float c = 0.5f * thickness;
    if (c < 0.5f) c = 0.5f;
    if (edge_soft < 0.0f) edge_soft = 0.0f;
    if (edge_soft > 1.0f) edge_soft = 1.0f;
    const float skirt = edge_soft * ((c > 1.0f) ? c : 1.0f);
    float h = c + skirt;
    const float h_max = 0.5f * (float)(LUX_CENTRO_MAX_WIN - 4);
    if (h > h_max) h = h_max;
    float p = c - skirt;
    if (p < 0.0f) p = 0.0f;

    for (int ch = 0; ch < 3; ch++)
        memset(state->accum[ch], 0, (size_t)px * sizeof(float));

    for (int s = 0; s < state->num_segs; s++)
    {
        const float pos = state->seg_pos[s];
        int x0 = (int)floorf(pos - h);
        int x1 = (int)ceilf (pos + h);
        if (x0 < 0)      x0 = 0;
        if (x1 > px - 1) x1 = px - 1;

        /* Window weights (channel-independent), 4× supersampled per pixel
         * (box-filter coverage: thin lines and fractional barycentres render
         * smoothly instead of snapping to the grid). The window is a PROFILE
         * (plateau = 1), not an energy budget: each channel prints the
         * mass's colour reference at full strength on the plateau — widening
         * the stroke never dims it. */
        static const float kSub[4] = { -0.375f, -0.125f, 0.125f, 0.375f };
        float win[LUX_CENTRO_MAX_WIN];
        float area = 0.0f;
        const int n = x1 - x0 + 1;
        for (int k = 0; k < n && k < LUX_CENTRO_MAX_WIN; k++)
        {
            float w = 0.0f;
            for (int u = 0; u < 4; u++)
                w += lux_centro_edge_win(fabsf((float)(x0 + k) + kSub[u] - pos), h, p);
            w *= 0.25f;
            win[k] = w;
            area  += w;
        }

        if (area <= 1e-6f)   /* degenerate window — print the colour on the nearest row */
        {
            int c = (int)(pos + 0.5f);
            if (c < 0)      c = 0;
            if (c > px - 1) c = px - 1;
            for (int ch = 0; ch < 3; ch++)
                state->accum[ch][c] += state->seg_col[ch][s];
            continue;
        }

        for (int ch = 0; ch < 3; ch++)
        {
            const float amp = state->seg_col[ch][s];
            float *acc = state->accum[ch];
            for (int k = 0; k < n && k < LUX_CENTRO_MAX_WIN; k++)
                acc[x0 + k] += amp * win[k];
        }
    }
}

void lux_centro_process_frame(
    LuxCentroState *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    (void)luxstral_num_octaves;   /* pixel-space module — no pitch axis needed */

    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxCentroConfig *cfg = &state->config;
    if (!cfg->enabled)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity/floor. */
        if (state->centro_active)
            lux_centro_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_CENTRO_MAX_PIXELS) px = LUX_CENTRO_MAX_PIXELS;

    float floor_e = 0.0f;
    const int bg_white = lux_centro_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    /* The floor was learned in one polarity — a flip invalidates it. */
    if (state->last_bg_mode != bg_white)
    {
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->centro_active = 1;

    lux_centro_ui_capture(state, in_r, in_g, in_b, px, bg_white, floor_e);

    const float thr = ((cfg->floor_level < 0.0f) ? 0.0f
                     : (cfg->floor_level > 1.0f) ? 1.0f : cfg->floor_level) * 255.0f;

    lux_centro_find_masses(state, in_r, in_g, in_b, px, bg_white, floor_e, thr);
    lux_centro_redraw(state, px, cfg->thickness_px, cfg->edge_soft);

    /* Output EQ — flat curve marks the LUT stale (doubles as the UI's
     * "output EQ shaping" flag) and costs nothing per pixel. */
    const float *eqlut = 0;
    if (lux_centro_eq_shaping(cfg))
    {
        lux_centro_eq_update_lut(state, px);
        eqlut = state->eq_lut;
    }
    else
        state->eq_lut_px = 0;

    /* Compose: the redrawn lines on CLEAN PAPER — écrêtage erases everything
     * below the threshold, so the output is eq * lines over the pure
     * background pole. Printing the CONSTANT pole (never the estimated
     * floor, whose per-line wander painted light vertical bands — the
     * grey-bands bug) keeps the paper stable by construction; no mass
     * surviving = blank paper. */
    const uint8_t *ins[3]  = { in_r, in_g, in_b };
    uint8_t       *outs[3] = { state->out_r, state->out_g, state->out_b };
    int diff = 0;   /* OR of out^in — an untouched line keeps the LED idle */
    for (int i = 0; i < px; i++)
    {
        const float eq_g = eqlut ? eqlut[i] : 1.0f;

        /* ONE joint clip factor across the channels: overlapping skirts or
         * an EQ boost can push a channel past full scale, and clipping each
         * channel independently crushed the ratio between them — a purple
         * line saturating grey/black. Scaling all three by the same factor
         * saturates toward the line's own hue instead. */
        float mat[3];
        float t = 1.0f;
        for (int ch = 0; ch < 3; ch++)
        {
            mat[ch] = state->accum[ch][i] * eq_g;
            if (mat[ch] > 255.0f)
            {
                const float tc = 255.0f / mat[ch];
                if (tc < t) t = tc;
            }
        }
        for (int ch = 0; ch < 3; ch++)
        {
            float e_out = t * mat[ch];
            if (e_out > 255.0f) e_out = 255.0f;
            if (e_out < 0.0f)   e_out = 0.0f;
            const uint8_t o = bg_white ? (uint8_t)(255.0f - e_out)
                                       : (uint8_t)e_out;
            outs[ch][i] = o;
            diff       |= o ^ ins[ch][i];
        }
    }
    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
