/*
 * lux_env_shape.h
 *
 * Shared envelope segment shaping function for LuxPitch / LuxMask ADSR.
 *
 * A single bipolar "curve" parameter bends a normalised segment between its two
 * endpoints while keeping it monotonic and pinned at f(0)=0, f(1)=1:
 *
 *     curve = 0   → linear
 *     curve > 0   → convex  (slow start, fast finish — "exp"  ease-in)
 *     curve < 0   → concave (fast start, slow finish — "log"  ease-out)
 *
 * The SAME function is used by the DSP (lux_pitch.c / lux_mask.c) and by the
 * UI curve drawing (EnvelopeEditorComponent) so what you SEE is exactly the
 * shape the engine applies.  Pure, branch-light, RT-safe, no allocations.
 *
 * Author: zhonx
 */

#ifndef LUX_ENV_SHAPE_H
#define LUX_ENV_SHAPE_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tension scale: curve ∈ [-1,1] maps to exponent k ∈ [-6,6]. */
#define LUX_ENV_SHAPE_TENSION 6.0f

/* Map x ∈ [0,1] through the shaped curve.  curve is clamped to [-1,1]. */
static inline float lux_env_shape(float x, float curve)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    if (curve >  1.0f) curve =  1.0f;
    if (curve < -1.0f) curve = -1.0f;

    const float k = curve * LUX_ENV_SHAPE_TENSION;
    if (k > -1e-3f && k < 1e-3f)   /* effectively linear */
        return x;

    /* Normalised exponential: monotonic, passes through (0,0) and (1,1). */
    return (expf(k * x) - 1.0f) / (expf(k) - 1.0f);
}

#ifdef __cplusplus
}
#endif

#endif /* LUX_ENV_SHAPE_H */
