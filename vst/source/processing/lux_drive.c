/*
 * lux_drive.c
 *
 * LuxDrive — gain / saturation / floor implementation (see lux_drive.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. e_in  = polarity(in)                      (bg conversion)
 *   2. m     = max(0, e_in - floor)              (floor = tracked PAPER level,
 *                                                 10th-percentile estimator)
 *   3. e_out = (e_in - m) + T(m)                 (material-only transfer — the
 *                                                 pixel's own pedestal stays)
 *   4. out   = polarity(clamp(e_out))
 *
 * T is the shared écrêtage→gamma curve (lux_drive_transfer), sampled into a
 * 257-entry LUT rebuilt only when a config value changes; per-pixel lookup
 * is a linear interpolation between adjacent entries. COLOUR saturation and
 * INVERT then run as joint-RGB passes on the composed output.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-08-03
 */

#include "lux_drive.h"
#include <math.h>
#include <string.h>

/* ── Instance pool (mirrors lux_eq.c) ──────────────────────────────────────────
 * Slot 0 is g_lux_drive_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxDriveState g_lux_drive_proc;
static LuxDriveState s_lux_drive_extra[CHAIN_MAX_CHAINS - 1];

LuxDriveState *lux_drive_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_drive_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_drive_extra[idx - 1];
}

void lux_drive_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_drive_init(lux_drive_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxDriveConfig lux_drive_config_default(void)
{
    LuxDriveConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.gamma           = 1.0f;   /* transparent */
    cfg.saturation      = 0.0f;
    cfg.floor_level     = 0.0f;
    cfg.invert_mode     = LUX_DRIVE_INV_OFF;
    cfg.background_mode = LUX_DRIVE_BG_AUTO;
    cfg.contrast_min    = 1.0f;   /* off — the audio knob's 0.21 is opt-in */
    cfg.contrast_power  = 0.5f;   /* additive_contrast_adjustment_power default */
    cfg.eq_num_bands    = 2;   /* one straight line — matches the UI default */
    /* eq_band_gain_db[] all 0 dB — flat curve = output EQ bypassed */
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_DRIVE_BG_LOCK_LINES 96

void lux_drive_reset(LuxDriveState *state)
{
    if (!state) return;
    state->drive_active = 0;
    state->last_bg_mode = -1;
    state->lut_valid    = 0;    /* invalidate the transfer LUT */
    state->eq_lut_px    = 0;    /* invalidate the output-EQ gain LUT */
    state->eq_lut_bands = 0;
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_DRIVE_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
    state->contrast_ema        = -1.0f;
    /* The UI guide layers deliberately survive a reset — a brief
     * disable/enable keeps the rémanence on screen. */
}

void lux_drive_init(LuxDriveState *state)
{
    if (!state) return;
    state->config = lux_drive_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    memset(state->ui_in_now,  0, sizeof(state->ui_in_now));
    memset(state->ui_in_peak, 0, sizeof(state->ui_in_peak));
    state->ui_in_valid = 0;
    lux_drive_reset(state);
}

/* Resolve the background pole for this frame + report the PAPER's own energy
 * (*out_floor). Polarity: mean-based AUTO learn-then-LOCK (mirrors lux_eq).
 * Paper level: EMA of the per-line 10th-PERCENTILE energy — NOT the mean-
 * based floor the other FX use: the mean is contaminated by the material, so
 * on dense streams the "paper" estimate followed the ink mass (tens of
 * levels), and every processed line re-printed its background at that false,
 * content-dependent level — grey vertical bands tracking the black mass. A
 * low percentile finds the paper between the strokes even on dense lines. */
static int lux_drive_resolve_bg(LuxDriveState *state,
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
    if (mode == LUX_DRIVE_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_DRIVE_BG_WHITE) bg_white = 1;
    else if (state->auto_locked)         bg_white = state->auto_bg_white;
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
 * Applied AFTER the transfer, on the driven output material — exactly a LuxEq
 * insert chained behind the module (mirrors lux_centro.c). */

static int lux_drive_eq_active_bands(const LuxDriveConfig *cfg)
{
    int n = cfg->eq_num_bands;
    if (n < 2)                n = 2;
    if (n > LUX_EQ_NUM_BANDS) n = LUX_EQ_NUM_BANDS;
    return n;
}

/* Nonzero when at least one active node is off 0 dB (flat = bypass). */
static int lux_drive_eq_shaping(const LuxDriveConfig *cfg)
{
    const int n = lux_drive_eq_active_bands(cfg);
    for (int b = 0; b < n; ++b)
        if (fabsf(cfg->eq_band_gain_db[b]) > 0.01f)
            return 1;
    return 0;
}

/* Rebuild the per-pixel linear-gain LUT when a band value, the node count or
 * the width changed — sampling the SHARED Catmull-Rom spline (lux_eq_curve_db)
 * so the LEVELS output EQ and the LuxEq module apply the exact same curve. */
static void lux_drive_eq_update_lut(LuxDriveState *state, int px)
{
    const int n = lux_drive_eq_active_bands(&state->config);

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
 * paper level), folded EVERY line into two release envelopes:
 *   now  — fast fall: the stream as it breathes (shows the lows),
 *   peak — slow fall: rémanence of the recent maxima (shows the highs).
 * The falls are per-line multiplicative, so the ballistics scale with the
 * stream's own line rate. Only there to guide the user in the editor. */
#define LUX_DRIVE_UI_NOW_FALL  0.97f    /* ~30 lines to fade   */
#define LUX_DRIVE_UI_PEAK_FALL 0.999f   /* ~1000 lines ≈ 1-2 s */

static void lux_drive_ui_capture(LuxDriveState *state,
                                 const uint8_t *in_r, const uint8_t *in_g,
                                 const uint8_t *in_b, int px,
                                 int bg_white, float floor_e)
{
    float line[LUX_DRIVE_UI_BINS] = { 0 };   /* bin max of THIS line */
    for (int i = 0; i < px; i++)
    {
        const int   v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const float e = bg_white ? (float)(255 - v) : (float)v;
        float m = (e - floor_e) * (1.0f / 255.0f);
        if (m <= 0.0f) continue;
        if (m > 1.0f)  m = 1.0f;
        const int bin = (i * LUX_DRIVE_UI_BINS) / px;
        if (m > line[bin]) line[bin] = m;
    }
    for (int b = 0; b < LUX_DRIVE_UI_BINS; b++)
    {
        const float now  = state->ui_in_now[b]  * LUX_DRIVE_UI_NOW_FALL;
        const float peak = state->ui_in_peak[b] * LUX_DRIVE_UI_PEAK_FALL;
        state->ui_in_now[b]  = (line[b] > now)  ? line[b] : now;
        state->ui_in_peak[b] = (line[b] > peak) ? line[b] : peak;
    }
    state->ui_in_valid = 1;
}

/* ── CONTRAST MIN (visual port of the LuxStral OUT knob) ───────────────────────
 * Per-line variance contrast, the img_stage_calculate_contrast law verbatim:
 *   ratio  = sqrt(variance) / sqrt(1/4)      (1/4 = max variance of a [0,1] set)
 *   factor = cmin + (1 - cmin) * ratio^power ∈ [cmin, 1]
 * Measured on the line's grayscale (stride 4). Variance is invariant under
 * inversion, so no polarity/floor handling is needed — the RAW-side measure
 * the audio path uses reads through bit-identical. */
static float lux_drive_line_contrast(const uint8_t *in_r, const uint8_t *in_g,
                                     const uint8_t *in_b, int px,
                                     float cmin, float power)
{
    float sum = 0.0f, sum_sq = 0.0f;
    int   n   = 0;
    for (int i = 0; i < px; i += 4)
    {
        const float v = ((float)in_r[i] + in_g[i] + in_b[i])
                        * (1.0f / (3.0f * 255.0f));
        sum    += v;
        sum_sq += v * v;
        n++;
    }
    if (n == 0)
        return 1.0f;

    const float mean = sum / (float)n;
    float variance = (sum_sq / (float)n) - (mean * mean);
    if (variance < 0.0f) variance = 0.0f;

    const float ratio  = sqrtf(variance) * 2.0f;   /* / sqrt(1/4) */
    const float factor = cmin + (1.0f - cmin) * powf(ratio, power);
    if (factor > 1.0f) return 1.0f;
    if (factor < cmin) return cmin;
    return factor;
}

/* Rebuild the transfer LUT when a config value changed. Entry k maps input
 * material energy k (0..255); entry 256 duplicates 255 so the per-pixel
 * linear interpolation never reads past the end. */
static void lux_drive_update_lut(LuxDriveState *state)
{
    const LuxDriveConfig *cfg = &state->config;

    if (state->lut_valid
        && state->lut_gamma == cfg->gamma
        && state->lut_floor == cfg->floor_level)
        return;

    state->lut_gamma = cfg->gamma;
    state->lut_floor = cfg->floor_level;
    state->lut_valid = 1;

    const float thr = cfg->floor_level * 255.0f;
    for (int k = 0; k < 256; ++k)
        state->lut[k] = lux_drive_transfer((float)k, cfg->gamma, thr);
    state->lut[256] = state->lut[255];
}

/* Apply the transfer to one channel (energy space, material only).
 * The pixel's OWN pedestal is preserved: e_out = e_in - m + eq[x] * T(m).
 * Background pixels (m = 0) pass through bit-identical — the paper is never
 * re-printed at the estimated floor (doing so painted grey bands that tracked
 * the ink mass whenever the estimate drifted with the content).
 * `eqlut` is the optional per-pixel output-EQ gain (NULL = flat curve);
 * `mat_gain` is the per-line CONTRAST MIN factor (1 = off) — both scale the
 * MATERIAL only, the pedestal stays.
 * Returns nonzero when the transfer actually altered the line (rack LED). */
static int lux_drive_channel(const uint8_t *in, uint8_t *out, const float *lut,
                             const float *eqlut, float mat_gain, int px,
                             int bg_white, float floor_e)
{
    int diff = 0;   /* OR of out^in — a material-free line passes through */

    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f)   e_mat = 0.0f;
        if (e_mat > 255.0f) e_mat = 255.0f;
        const int   k = (int)e_mat;
        const float t = e_mat - (float)k;
        float mat_out = (lut[k] + (lut[k + 1] - lut[k]) * t) * mat_gain;
        if (eqlut) mat_out *= eqlut[i];
        float e_out = (e_in - e_mat) + mat_out;
        if (e_out > 255.0f) e_out = 255.0f;
        if (e_out < 0.0f)   e_out = 0.0f;
        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
        diff  |= out[i] ^ in[i];
    }
    return diff;
}

/* COLOUR saturation pass — joint on the composed RGB output: each channel is
 * scaled around the pixel's own luminance. k = 0 collapses to B&W, 1 is
 * untouched, LUX_DRIVE_CHROMA_MAX is hyper-vibrant. Neutral pixels (paper,
 * grey strokes) equal their luminance and pass bit-identical, so the stage
 * needs no floor/polarity handling.
 * Returns nonzero when the pass actually altered the line (rack LED). */
static int lux_drive_chroma(uint8_t *r, uint8_t *g, uint8_t *b, int px,
                            float sat)
{
    const float k = (sat >= 0.0f) ? 1.0f + sat * (LUX_DRIVE_CHROMA_MAX - 1.0f)
                                  : 1.0f + sat;
    int diff = 0;
    for (int i = 0; i < px; i++)
    {
        const float lum = ((float)r[i] + g[i] + b[i]) * (1.0f / 3.0f);
        float vr = lum + k * ((float)r[i] - lum);
        float vg = lum + k * ((float)g[i] - lum);
        float vb = lum + k * ((float)b[i] - lum);
        if (vr < 0.0f) vr = 0.0f; if (vr > 255.0f) vr = 255.0f;
        if (vg < 0.0f) vg = 0.0f; if (vg > 255.0f) vg = 255.0f;
        if (vb < 0.0f) vb = 0.0f; if (vb > 255.0f) vb = 255.0f;
        const uint8_t nr = (uint8_t)vr, ng = (uint8_t)vg, nb = (uint8_t)vb;
        diff |= (nr ^ r[i]) | (ng ^ g[i]) | (nb ^ b[i]);
        r[i] = nr; g[i] = ng; b[i] = nb;
    }
    return diff;
}

/* Final joint-RGB inversion — mirrors the VideoScroll modes.
 *   NEGATIVE — flip each channel (255 - c).
 *   LUMA     — invert the HSL lightness only: a uniform per-channel shift by
 *              (255 - max - min) [chroma is unchanged by L → 1-L, so only
 *              the offset moves — see VideoScrollRenderCore]. Hue and colour
 *              saturation are preserved.
 * Returns nonzero when the pass actually altered the line (rack LED). */
static int lux_drive_invert(uint8_t *r, uint8_t *g, uint8_t *b, int px,
                            int mode)
{
    int diff = 0;
    if (mode == LUX_DRIVE_INV_NEGATIVE)
    {
        for (int i = 0; i < px; i++)
        {
            const uint8_t nr = (uint8_t)(255 - r[i]);
            const uint8_t ng = (uint8_t)(255 - g[i]);
            const uint8_t nb = (uint8_t)(255 - b[i]);
            diff |= (nr ^ r[i]) | (ng ^ g[i]) | (nb ^ b[i]);
            r[i] = nr; g[i] = ng; b[i] = nb;
        }
    }
    else if (mode == LUX_DRIVE_INV_LUMA)
    {
        for (int i = 0; i < px; i++)
        {
            int mx = r[i], mn = r[i];
            if (g[i] > mx) mx = g[i]; if (g[i] < mn) mn = g[i];
            if (b[i] > mx) mx = b[i]; if (b[i] < mn) mn = b[i];
            const int delta = 255 - mx - mn;   /* new range stays in 0..255 */
            const uint8_t nr = (uint8_t)(r[i] + delta);
            const uint8_t ng = (uint8_t)(g[i] + delta);
            const uint8_t nb = (uint8_t)(b[i] + delta);
            diff |= (nr ^ r[i]) | (ng ^ g[i]) | (nb ^ b[i]);
            r[i] = nr; g[i] = ng; b[i] = nb;
        }
    }
    return diff;
}

void lux_drive_process_frame(
    LuxDriveState  *state,
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

    const LuxDriveConfig *cfg = &state->config;
    if (!cfg->enabled)
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity/floor. */
        if (state->drive_active)
            lux_drive_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_DRIVE_MAX_PIXELS) px = LUX_DRIVE_MAX_PIXELS;

    float floor_e = 0.0f;
    const int bg_white = lux_drive_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    /* The floor was learned in one polarity — a flip invalidates it (the LUT
     * is polarity-agnostic and stays). */
    if (state->last_bg_mode != bg_white)
    {
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->drive_active = 1;

    /* The editor shows the REAL stream whenever the block is powered — an
     * identity transfer only skips the processing below, never the view
     * (CENTROID parity: its editor goes live the moment the block is on). */
    lux_drive_ui_capture(state, in_r, in_g, in_b, px, bg_white, floor_e);

    const int identity = (fabsf(cfg->gamma - 1.0f) < 1e-3f
                          && fabsf(cfg->saturation) < 1e-4f
                          && cfg->floor_level < 1e-4f
                          && cfg->invert_mode == LUX_DRIVE_INV_OFF
                          && cfg->contrast_min >= 0.999f
                          && !lux_drive_eq_shaping(cfg));
    if (identity)
    {
        state->contrast_ema = -1.0f;   /* the knob is off in an identity transfer */
        state->eq_lut_px    = 0;       /* keep the EQ live-glow flag honest */
        return;                        /* O(1) pass-through — the view stays live */
    }

    lux_drive_update_lut(state);

    /* Output EQ — flat curve marks the LUT stale (doubles as the UI's
     * "output EQ shaping" flag) and costs nothing per pixel. */
    const float *eqlut = 0;
    if (lux_drive_eq_shaping(cfg))
    {
        lux_drive_eq_update_lut(state, px);
        eqlut = state->eq_lut;
    }
    else
        state->eq_lut_px = 0;

    /* CONTRAST MIN — smoothed (EMA 1/8) so the dimming follows the stream
     * without per-line shimmer; unseeded while the knob sits at 1 (off). */
    float mat_gain = 1.0f;
    if (cfg->contrast_min < 0.999f)
    {
        const float inst = lux_drive_line_contrast(in_r, in_g, in_b, px,
                                                   cfg->contrast_min,
                                                   cfg->contrast_power);
        if (state->contrast_ema < 0.0f)
            state->contrast_ema = inst;
        else
            state->contrast_ema += (inst - state->contrast_ema) * (1.0f / 8.0f);
        mat_gain = state->contrast_ema;
    }
    else
        state->contrast_ema = -1.0f;

    int diff = lux_drive_channel(in_r, state->out_r, state->lut, eqlut, mat_gain, px, bg_white, floor_e);
    diff |= lux_drive_channel(in_g, state->out_g, state->lut, eqlut, mat_gain, px, bg_white, floor_e);
    diff |= lux_drive_channel(in_b, state->out_b, state->lut, eqlut, mat_gain, px, bg_white, floor_e);

    /* COLOUR saturation — joint chroma stage on the composed output. */
    if (fabsf(cfg->saturation) >= 1e-4f)
        diff |= lux_drive_chroma(state->out_r, state->out_g, state->out_b,
                                 px, cfg->saturation);

    /* INVERT — final output inversion (Negative / Luminance). */
    if (cfg->invert_mode != LUX_DRIVE_INV_OFF)
        diff |= lux_drive_invert(state->out_r, state->out_g, state->out_b,
                                 px, cfg->invert_mode);

    if (diff)
        state->active_ticks++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
