/*
 * lux_harmo.c
 *
 * LuxHarmo (SCALE) — musical quantizer implementation (see lux_harmo.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. e_in  = polarity(in)                          (bg conversion)
 *   2. MASK: e_out = floor + geff[x] * (e_in - floor)
 *      WARP: material scattered along x + s*disp[x], floor re-added
 *   3. out   = polarity(clamp(e_out))
 *
 * The displacement grid disp[x] = nearest-allowed-degree centre − x (pixels)
 * is rebuilt only when root/scale/axis geometry change; strength/width/slope
 * are applied at process time so knob gestures never rebuild. Root/scale
 * changes crossfade old grid → new grid over `glide_lines` frames.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-18
 */

#include "lux_harmo.h"
#include <math.h>
#include <string.h>

/* ── Instance pool (mirrors lux_eq.c) ──────────────────────────────────────────
 * Slot 0 is g_lux_harmo_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxHarmoState g_lux_harmo_proc;
static LuxHarmoState s_lux_harmo_extra[CHAIN_MAX_CHAINS - 1];

LuxHarmoState *lux_harmo_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_harmo_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_harmo_extra[idx - 1];
}

void lux_harmo_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_harmo_init(lux_harmo_instance(i));
}

/* ── Scale presets — bit k = interval k above the root (bit 0 always set) ──── */
static const uint16_t k_scale_masks[LUX_HARMO_NUM_SCALES] = {
    0x0FFF,  /* CHROMATIC   — every semitone (WARP: snap-to-semitone grid)  */
    0x0AB5,  /* MAJOR       — 0 2 4 5 7 9 11 */
    0x05AD,  /* MINOR       — 0 2 3 5 7 8 10 (natural) */
    0x09AD,  /* HARM MINOR  — 0 2 3 5 7 8 11 */
    0x0295,  /* PENTA MAJ   — 0 2 4 7 9      */
    0x04A9,  /* PENTA MIN   — 0 3 5 7 10     */
    0x04E9,  /* BLUES       — 0 3 5 6 7 10   */
    0x0555,  /* WHOLE TONE  — 0 2 4 6 8 10   */
    0x06AD,  /* DORIAN      — 0 2 3 5 7 9 10 */
    0x05AB,  /* PHRYGIAN    — 0 1 3 5 7 8 10 */
    0x0AD5,  /* LYDIAN      — 0 2 4 6 7 9 11 */
    0x06B5,  /* MIXOLYDIAN  — 0 2 4 5 7 9 10 */
    0x0081,  /* FIFTHS      — 0 7            */
    0x0001,  /* OCTAVES     — 0              */
};

uint16_t lux_harmo_scale_mask(int scale)
{
    if (scale < 0 || scale >= LUX_HARMO_NUM_SCALES)
        scale = LUX_HARMO_SCALE_CHROMATIC;
    return k_scale_masks[scale];
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxHarmoConfig lux_harmo_config_default(void)
{
    LuxHarmoConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.mode            = LUX_HARMO_MODE_WARP;
    cfg.root            = 0;                      /* C */
    cfg.scale           = LUX_HARMO_SCALE_MAJOR;
    cfg.strength        = 1.0f;
    cfg.width_st        = 0.35f;
    cfg.slope           = 0.5f;
    cfg.glide_lines     = 64;
    cfg.background_mode = LUX_HARMO_BG_AUTO;
    cfg.axis_low_hz     = 0.0f;                   /* → C2 fallback */
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_ECHO_BG_LOCK_LINES. */
#define LUX_HARMO_BG_LOCK_LINES 96

void lux_harmo_reset(LuxHarmoState *state)
{
    if (!state) return;
    state->harmo_active = 0;
    state->last_bg_mode = -1;
    state->grid_valid   = 0;    /* invalidate the displacement grids */
    state->grid_px      = 0;
    state->xfade        = 1.0f;
    state->strength_smooth = 0.0f;   /* re-enable fades in from raw */
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_HARMO_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
}

void lux_harmo_init(LuxHarmoState *state)
{
    if (!state) return;
    state->config = lux_harmo_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    lux_harmo_reset(state);
}

/* Resolve the background pole for this frame + report the background's OWN
 * energy (*out_floor). Polarity/gate mirror lux_eq_resolve_bg, but the floor
 * estimate is the sampled MINIMUM energy of the line, NOT its mean: every
 * output row is rebuilt as floor + processed material, so a mean-based floor
 * (which averages the material in) over-estimates on dense bright-on-black
 * streams and prints a uniform grey veil on every empty row. The darkest
 * sampled pixel IS the background; under-estimating is harmless (slight
 * material overcount), over-estimating is the grey-background bug. */
static int lux_harmo_resolve_bg(LuxHarmoState *state,
                                const uint8_t *in_r, const uint8_t *in_g,
                                const uint8_t *in_b, int px, float *out_floor)
{
    uint32_t sum = 0;
    int      n   = 0;
    int      min_pm = 255, max_pm = 0;   /* sampled per-pixel channel means */
    for (int i = 0; i < px; i += 8)
    {
        const int pm = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        sum += (uint32_t)pm;
        n   += 1;
        if (pm < min_pm) min_pm = pm;
        if (pm > max_pm) max_pm = pm;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;

    int bg_white;
    const int mode = state->config.background_mode;
    if (mode == LUX_HARMO_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_HARMO_BG_WHITE) bg_white = 1;
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

    /* Floor = the sampled pixel CLOSEST to the background pole. The mean
     * gate still skips lines with no visible background at all (fully dense
     * passages must not drag the floor up). */
    const int   line_is_bg = bg_white ? (mean > 143) : (mean < 111);
    float inst_floor = bg_white ? (float)(255 - max_pm) : (float)min_pm;
    if (inst_floor < 0.0f) inst_floor = 0.0f;
    if (state->floor_ema < 0.0f)
        state->floor_ema = line_is_bg ? inst_floor : 0.0f;   /* seed */
    else if (line_is_bg)
        state->floor_ema += (inst_floor - state->floor_ema) * (1.0f / 64.0f);

    *out_floor = state->floor_ema;
    return bg_white;
}

/* Build disp[] = nearest allowed degree centre − pixel, on the PHYSICAL pitch
 * axis: pixel 0 sits at axis_low_hz, one degree centre per scale note. The
 * centres are ascending, so a single two-pointer walk covers every pixel.
 * cell[] gets the cell half-width (centre → Voronoi boundary) on the pixel's
 * side of its centre — the span WARP's landing-band squeeze normalizes by. */
static void lux_harmo_build_disp(float *disp, float *cell, int px, float pps,
                                 float midi_low, int root, uint16_t mask)
{
    /* Degree centres over the axis span + one tooth of margin each side. */
    float centers[168];
    int   k = 0;
    const int n_lo = (int)floorf(midi_low) - 1;
    const int n_hi = (int)ceilf(midi_low + (float)px / pps) + 1;
    for (int n = n_lo; n <= n_hi && k < (int)(sizeof(centers) / sizeof(centers[0])); ++n)
    {
        const int cls = ((n - root) % 12 + 12) % 12;
        if ((mask >> cls) & 1u)
            centers[k++] = ((float)n - midi_low) * pps;
    }
    if (k == 0)   /* unreachable (bit 0 always set) — neutral grid */
    {
        for (int i = 0; i < px; ++i) { disp[i] = 0.0f; cell[i] = (float)px; }
        return;
    }

    int c = 0;
    for (int i = 0; i < px; ++i)
    {
        const float x = (float)i;
        while (c + 1 < k && x > 0.5f * (centers[c] + centers[c + 1]))
            c++;
        disp[i] = centers[c] - x;
        /* Half-width toward the neighbour on THIS pixel's side; the axis
         * edges act as an open cell (no squeeze reference → full size). */
        if (x < centers[c])
            cell[i] = (c > 0) ? 0.5f * (centers[c] - centers[c - 1]) : (float)px;
        else
            cell[i] = (c + 1 < k) ? 0.5f * (centers[c + 1] - centers[c]) : (float)px;
    }
}

/* Rebuild the grids when root/scale/axis geometry changed. Root/scale edits
 * crossfade old → new over glide_lines; geometry changes (pixel count,
 * octaves, low frequency) snap — blending grids of two different axes would
 * smear energy across unrelated rows. */
static void lux_harmo_update_grid(LuxHarmoState *state, int px, int octaves)
{
    const LuxHarmoConfig *cfg = &state->config;
    const float low = (cfg->axis_low_hz > 0.0f) ? cfg->axis_low_hz : 65.406f;

    const int geom_dirty = (state->grid_px != px)
                        || (state->grid_octaves != octaves)
                        || (state->grid_low_hz != low);
    const int music_dirty = (state->grid_root != cfg->root)
                         || (state->grid_scale != cfg->scale);
    if (state->grid_valid && !geom_dirty && !music_dirty)
        return;

    const float pps      = (float)px / ((float)octaves * 12.0f);
    const float midi_low = 69.0f + 12.0f * log2f(low / 440.0f);

    if (state->grid_valid && !geom_dirty && cfg->glide_lines > 0)
    {
        /* Fold the current grid into `old` and restart the fade — a change
         * landing mid-fade completes the previous one instantly. */
        memcpy(state->disp_old, state->disp_cur, (size_t)px * sizeof(float));
        memcpy(state->cell_old, state->cell_cur, (size_t)px * sizeof(float));
        state->xfade = 0.0f;
    }
    else
        state->xfade = 1.0f;

    lux_harmo_build_disp(state->disp_cur, state->cell_cur, px, pps, midi_low,
                         cfg->root, lux_harmo_scale_mask(cfg->scale));

    state->grid_valid   = 1;
    state->grid_pps     = pps;
    state->grid_px      = px;
    state->grid_octaves = octaves;
    state->grid_low_hz  = low;
    state->grid_root    = cfg->root;
    state->grid_scale   = cfg->scale;
}

/* Comb gain (0..1, strength NOT applied) for a displacement, in semitones:
 * 1 within ±width/2 of a degree centre, smoothstep to 0 over a slope-
 * controlled edge span. */
static inline float lux_harmo_comb_gain(float disp_px, float pps,
                                        float half_st, float soft_st)
{
    const float dsem = fabsf(disp_px) / pps;
    float t = (dsem - half_st) / soft_st;
    if (t <= 0.0f) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* MASK — apply the blended comb (scratch) to one channel, material only. */
static void lux_harmo_mask_channel(const uint8_t *in, uint8_t *out,
                                   const float *geff, int px,
                                   int bg_white, float floor_e)
{
    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float e_mat = e_in - floor_e;
        if (e_mat < 0.0f) e_mat = 0.0f;
        float e_out = floor_e + geff[i] * e_mat;
        if (e_out > 255.0f) e_out = 255.0f;
        if (e_out < 0.0f)   e_out = 0.0f;
        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
    }
}

/* WARP — scatter one channel's material into `accum` with weight w (linear
 * split between the two neighbour rows). The full-snap displacement is scaled
 * by the landing-band squeeze: the whole Voronoi cell maps linearly into
 * ±h_px around the degree centre, so width → 0 collapses onto the centre row
 * and width → cell size leaves the material where it is. */
static void lux_harmo_scatter(const uint8_t *in, const float *disp,
                              const float *cell, float h_px,
                              float *accum, int px, float s, float w,
                              int bg_white, float floor_e)
{
    for (int i = 0; i < px; i++)
    {
        const float e_in = bg_white ? (float)(255 - in[i]) : (float)in[i];
        float m = e_in - floor_e;
        if (m <= 0.0f) continue;
        m *= w;

        float squeeze = 1.0f - h_px / cell[i];
        if (squeeze < 0.0f) squeeze = 0.0f;
        float d = (float)i + s * squeeze * disp[i];
        if (d < 0.0f)             d = 0.0f;
        if (d > (float)(px - 1))  d = (float)(px - 1);
        const int   i0 = (int)d;
        const float f  = d - (float)i0;
        accum[i0] += m * (1.0f - f);
        if (i0 + 1 < px)
            accum[i0 + 1] += m * f;
    }
}

void lux_harmo_process_frame(
    LuxHarmoState  *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const LuxHarmoConfig *cfg = &state->config;
    const float s_target = (cfg->strength < 0.0f) ? 0.0f
                         : (cfg->strength > 1.0f) ? 1.0f : cfg->strength;
    /* Pass through only once the slewed strength has ALSO landed on 0 —
     * turning the knob to 0 rides the glide down instead of snapping. */
    if (!cfg->enabled
        || (s_target <= 0.001f && state->strength_smooth <= 0.001f))
    {
        /* Lazy one-shot re-arm so a re-enable relearns the AUTO polarity/floor. */
        if (state->harmo_active)
            lux_harmo_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_HARMO_MAX_PIXELS) px = LUX_HARMO_MAX_PIXELS;
    const int octaves = (luxstral_num_octaves > 0) ? luxstral_num_octaves : 8;

    float floor_e = 0.0f;
    const int bg_white = lux_harmo_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    /* The floor was learned in one polarity — a flip invalidates it (the
     * grids are polarity-agnostic and stay). */
    if (state->last_bg_mode != bg_white)
    {
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->harmo_active = 1;

    lux_harmo_update_grid(state, px, octaves);

    /* Advance the old → new grid crossfade (one step per processed line). */
    float x = state->xfade;
    if (x < 1.0f)
    {
        const int glide = (cfg->glide_lines > 0) ? cfg->glide_lines : 1;
        x += 1.0f / (float)glide;
        if (x > 1.0f) x = 1.0f;
        state->xfade = x;
    }
    const int fading = (x < 1.0f);

    /* Strength slew — same rate as the grid fade: a full 0 → 1 sweep takes
     * glide_lines frames. Glide 0 = instant (consistent with the grids). */
    float s = s_target;
    if (cfg->glide_lines > 0)
    {
        const float step = 1.0f / (float)cfg->glide_lines;
        float d = s_target - state->strength_smooth;
        if (d >  step) d =  step;
        if (d < -step) d = -step;
        state->strength_smooth += d;
        s = state->strength_smooth;
    }
    else
        state->strength_smooth = s_target;

    if (cfg->mode == LUX_HARMO_MODE_MASK)
    {
        /* Blended effective gain, once per frame, shared by the 3 channels.
         * Edge span: sharp comb at slope 1, wide soft skirt at slope 0. */
        const float half = 0.5f * ((cfg->width_st > 0.01f) ? cfg->width_st : 0.01f);
        const float soft = 0.02f + (1.0f - cfg->slope) * 0.60f;
        const float pps  = state->grid_pps;
        float *geff = state->scratch;
        for (int i = 0; i < px; i++)
        {
            float g = lux_harmo_comb_gain(state->disp_cur[i], pps, half, soft);
            if (fading)
            {
                const float go = lux_harmo_comb_gain(state->disp_old[i], pps, half, soft);
                g = go + x * (g - go);
            }
            geff[i] = 1.0f - s * (1.0f - g);
        }
        lux_harmo_mask_channel(in_r, state->out_r, geff, px, bg_white, floor_e);
        lux_harmo_mask_channel(in_g, state->out_g, geff, px, bg_white, floor_e);
        lux_harmo_mask_channel(in_b, state->out_b, geff, px, bg_white, floor_e);
    }
    else
    {
        /* WARP — per channel: weighted scatter (both grids while fading),
         * then floor re-added and polarity restored. */
        const float h_px = 0.5f * ((cfg->width_st > 0.01f) ? cfg->width_st : 0.01f)
                         * state->grid_pps;
        const uint8_t *ins[3]  = { in_r, in_g, in_b };
        uint8_t       *outs[3] = { state->out_r, state->out_g, state->out_b };
        for (int ch = 0; ch < 3; ch++)
        {
            float *accum = state->scratch;
            memset(accum, 0, (size_t)px * sizeof(float));
            if (fading)
                lux_harmo_scatter(ins[ch], state->disp_old, state->cell_old,
                                  h_px, accum, px, s,
                                  1.0f - x, bg_white, floor_e);
            lux_harmo_scatter(ins[ch], state->disp_cur, state->cell_cur,
                              h_px, accum, px, s,
                              fading ? x : 1.0f, bg_white, floor_e);
            for (int i = 0; i < px; i++)
            {
                float e_out = floor_e + accum[i];
                if (e_out > 255.0f) e_out = 255.0f;
                if (e_out < 0.0f)   e_out = 0.0f;
                outs[ch][i] = bg_white ? (uint8_t)(255.0f - e_out)
                                       : (uint8_t)e_out;
            }
        }
    }

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}
