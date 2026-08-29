/*
 * shape_eq.h
 *
 * ShapeEq — typed-handle EQ curve evaluator, shared by every EQ surface
 * (LuxEq / LuxCentro / LuxDrive output banks, the sampler slot EQ and the
 * generator tabs). Replaces the positional 9-band Catmull-Rom spline
 * (lux_eq_curve_db): an EQ is now a stack of up to SHAPE_EQ_MAX_HANDLES
 * typed handles, each carrying three musical settings —
 *
 *   freq01   position on the log-frequency pixel axis (0 = low edge,
 *            1 = high edge; the curve is positional whatever the span)
 *   gain_db  height (Bell peak / Tilt endpoints; inert for the filters)
 *   width01  width or resonance (Bell bandwidth / LP-HP-DJ resonance;
 *            inert for Tilt)
 *
 * Contributions sum in dB; the final clamp is [SHAPE_EQ_DB_MIN, DB_MAX] —
 * the floor sits well below the ±24 dB band range so a filter can CLOSE
 * (≈ silence), while the UI keeps drawing on its ±24 dB grid with the
 * roll-off clipped at the plot bottom.
 *
 * Single source of truth: the RT LUT builders and the UI editor both sample
 * shape_eq_db, so the drawn curve IS the applied gain.
 *
 * RT-safety: Pure C, allocation-free, no libc beyond <math.h>.
 *
 * Author: zhonx
 * Created: 2026-08-23
 */

#ifndef SHAPE_EQ_H
#define SHAPE_EQ_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHAPE_EQ_MAX_HANDLES 4
#define SHAPE_EQ_DB_MAX      24.0f   /* UI grid / Bell-Tilt gain range */
#define SHAPE_EQ_DB_MIN     -60.0f   /* applied floor — a closed filter is silent */

/* Handle types. Off = no contribution (the slot is empty). */
#define SHAPE_EQ_OFF   0
#define SHAPE_EQ_BELL  1
#define SHAPE_EQ_LP    2
#define SHAPE_EQ_HP    3
#define SHAPE_EQ_DJ    4   /* one bipolar knob: freq01 0.5 = flat,
                            * < 0.5 the LP closes, > 0.5 the HP rises */
#define SHAPE_EQ_TILT  5
/* Tilt steepness: the ±24 dB gain param reaches ±(24·SCALE) dB half an
 * axis from the pivot — 2× the travel of the first cut (the applied
 * clamp caps the extremes). */
#define SHAPE_EQ_TILT_SCALE 2.0f
#define SHAPE_EQ_NUM_TYPES 6

/* Filter voicing. LP/HP/DJ: width01 = SLOPE, log-mapped 3..192 dB/oct
 * (0.5 → 24). DJ resonance rides gain_db (the handle's height — LP/HP
 * compose theirs by stacking a Bell at the cutoff). */
#define SHAPE_EQ_RESO_DB_MAX         24.0f  /* DJ bump height ceiling (= the gain range: 100 % reso = GAIN CC at full) */
#define SHAPE_EQ_RESO_SIGMA_OCT      0.35f  /* DJ bump width around cutoff    */
#define SHAPE_EQ_DJ_DEADZONE         0.05f  /* half-width of the flat centre  */

/* Filter slope from width01 (log map, centre = 24 dB/oct). */
static inline float shape_eq_filter_slope(float w01)
{
    if (w01 < 0.0f) w01 = 0.0f;
    if (w01 > 1.0f) w01 = 1.0f;
    return 3.0f * powf(64.0f, w01);   /* 3 .. 192 dB/oct, centre 24 */
}

/* DJ resonance from gain_db (0..1; negative gain = none). */
static inline float shape_eq_dj_reso(float gain_db)
{
    const float r = gain_db / SHAPE_EQ_RESO_DB_MAX;
    return (r < 0.0f) ? 0.0f : (r > 1.0f) ? 1.0f : r;
}

typedef struct {
    int   type;      /* SHAPE_EQ_* */
    float freq01;    /* 0..1 position on the log-f axis */
    float gain_db;   /* -24..+24 dB */
    float width01;   /* 0..1 (log bandwidth / resonance) */
} ShapeEqHandle;

/* Neutral handle — an empty slot. */
static inline void shape_eq_default(ShapeEqHandle *h)
{
    h->type = SHAPE_EQ_OFF; h->freq01 = 0.5f; h->gain_db = 0.0f; h->width01 = 0.5f;
}

/* Bell bandwidth: width01 0..1 → octaves, log-mapped 0.1..10 (0.5 → 1 oct). */
static inline float shape_eq_bell_octaves(float w01)
{
    if (w01 < 0.0f) w01 = 0.0f;
    if (w01 > 1.0f) w01 = 1.0f;
    return 0.1f * powf(100.0f, w01);
}

/* One filter edge: `slope` dB/oct past the cutoff + resonance bump AT the
 * cutoff. `above` picks the attenuated side (nonzero = LP, attenuate above). */
static inline float shape_eq_filter_db(float x01, float cut01, float slope,
                                       float reso01, float span_oct, int above)
{
    const float d_oct = (x01 - cut01) * span_oct;
    float db = 0.0f;
    if (above) { if (d_oct > 0.0f) db -= slope * d_oct; }
    else       { if (d_oct < 0.0f) db += slope * d_oct; }
    if (reso01 > 0.0f)
    {
        const float t = d_oct / SHAPE_EQ_RESO_SIGMA_OCT;
        db += reso01 * SHAPE_EQ_RESO_DB_MAX * expf(-0.5f * t * t);
    }
    return db;
}

/* Contribution of ONE handle at x01 ∈ [0, 1], in dB (unclamped). */
static inline float shape_eq_handle_db(const ShapeEqHandle *h, float x01,
                                       float span_oct)
{
    switch (h->type)
    {
        case SHAPE_EQ_BELL:
        {
            const float sigma = shape_eq_bell_octaves(h->width01) / 2.355f;
            const float t     = (x01 - h->freq01) * span_oct / sigma;
            return h->gain_db * expf(-0.5f * t * t);
        }
        case SHAPE_EQ_LP:
            return shape_eq_filter_db(x01, h->freq01,
                                      shape_eq_filter_slope(h->width01),
                                      0.0f, span_oct, 1);
        case SHAPE_EQ_HP:
            return shape_eq_filter_db(x01, h->freq01,
                                      shape_eq_filter_slope(h->width01),
                                      0.0f, span_oct, 0);
        case SHAPE_EQ_DJ:
        {
            const float k = 2.0f * h->freq01 - 1.0f;
            if (k <= -SHAPE_EQ_DJ_DEADZONE)
            {
                const float a = (-k - SHAPE_EQ_DJ_DEADZONE)
                              / (1.0f - SHAPE_EQ_DJ_DEADZONE);
                return shape_eq_filter_db(x01, 1.0f - a,
                                          shape_eq_filter_slope(h->width01),
                                          shape_eq_dj_reso(h->gain_db),
                                          span_oct, 1);
            }
            if (k >= SHAPE_EQ_DJ_DEADZONE)
            {
                const float a = (k - SHAPE_EQ_DJ_DEADZONE)
                              / (1.0f - SHAPE_EQ_DJ_DEADZONE);
                return shape_eq_filter_db(x01, a,
                                          shape_eq_filter_slope(h->width01),
                                          shape_eq_dj_reso(h->gain_db),
                                          span_oct, 0);
            }
            return 0.0f;   /* dead zone: exactly flat */
        }
        case SHAPE_EQ_TILT:
        {
            /* Line through the pivot; gain_db is the height half an axis
             * away. width01 rounds it into an S (tanh knee): 0 = straight,
             * 1 = shelf-like plateaus at ±gain. */
            const float d = 2.0f * (x01 - h->freq01);
            const float a = h->gain_db * SHAPE_EQ_TILT_SCALE;  /* dB at ±half-axis */
            if (h->width01 <= 0.001f)
                return a * d;
            const float k = 1.0f + 7.0f * h->width01;
            const float s = tanhf(k * d) / tanhf(k);
            return a * ((1.0f - h->width01) * d + h->width01 * s);
        }
        default:
            return 0.0f;
    }
}

/* Full curve at x01: global level + sum of every handle, clamped to the
 * applied range. `level_db` is the whole-EQ gain offset (the left-margin
 * fader — the "master fader" of the curve). */
static inline float shape_eq_db_level(const ShapeEqHandle *h, int n,
                                      float level_db, float x01,
                                      float span_oct)
{
    float db = level_db;
    for (int i = 0; i < n; ++i)
        db += shape_eq_handle_db(&h[i], x01, span_oct);
    if (db > SHAPE_EQ_DB_MAX) db = SHAPE_EQ_DB_MAX;
    if (db < SHAPE_EQ_DB_MIN) db = SHAPE_EQ_DB_MIN;
    return db;
}

static inline float shape_eq_db(const ShapeEqHandle *h, int n, float x01,
                                float span_oct)
{
    return shape_eq_db_level(h, n, 0.0f, x01, span_oct);
}

/* Nonzero when the handle contributes nothing anywhere (bypass predicate —
 * must stay consistent with shape_eq_handle_db so "flat" really is a
 * bit-identical pass-through). */
static inline int shape_eq_handle_flat(const ShapeEqHandle *h)
{
    switch (h->type)
    {
        case SHAPE_EQ_BELL:
        case SHAPE_EQ_TILT: return fabsf(h->gain_db) <= 0.01f;
        case SHAPE_EQ_LP:   return h->freq01 >= 0.999f;   /* fully open */
        case SHAPE_EQ_HP:   return h->freq01 <= 0.001f;
        case SHAPE_EQ_DJ:   return fabsf(2.0f * h->freq01 - 1.0f)
                                   < SHAPE_EQ_DJ_DEADZONE;
        default:            return 1;   /* Off */
    }
}

static inline int shape_eq_is_flat(const ShapeEqHandle *h, int n)
{
    for (int i = 0; i < n; ++i)
        if (!shape_eq_handle_flat(&h[i]))
            return 0;
    return 1;
}

/* Bypass predicate INCLUDING the global level fader. */
static inline int shape_eq_is_flat_level(const ShapeEqHandle *h, int n,
                                         float level_db)
{
    return fabsf(level_db) <= 0.01f && shape_eq_is_flat(h, n);
}

/* Field-wise equality (LUT dirty checks — no memcmp, padding-agnostic). */
static inline int shape_eq_handles_equal(const ShapeEqHandle *a,
                                         const ShapeEqHandle *b, int n)
{
    for (int i = 0; i < n; ++i)
        if (a[i].type    != b[i].type    || a[i].freq01  != b[i].freq01
         || a[i].gain_db != b[i].gain_db || a[i].width01 != b[i].width01)
            return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SHAPE_EQ_H */
