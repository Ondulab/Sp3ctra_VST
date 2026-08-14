/**
 * @file EqCurve.h
 * @brief Shared graphic-EQ curve evaluator — uniform Catmull-Rom spline
 *        through N band nodes, in dB.
 *
 * Same maths as lux_eq_curve_db (processing/lux_eq.h) generalised to an
 * arbitrary node count: C1-smooth, interpolates every node, end nodes
 * duplicated, overshoot clamped to ±clampDb. Single source of truth for every
 * ScoreEqComponent consumer (SCORE / VOICE / SAMPLER slots / MIDI SCORE), so
 * the drawn spline IS the applied gain — matching the LuxEq module's look.
 *
 * Pure header, no JUCE deps — usable from UI components and engine code alike.
 */
#pragma once

/** Gain curve in dB at position x ∈ [0, n-1] over the node gains g[0..n-1]. */
inline float eqCurveDbAt(const float* g, int n, float x, float clampDb) noexcept
{
    if (n <= 0) return 0.0f;
    if (n == 1) return g[0];
    const int last = n - 1;
    if (x <= 0.0f)         return g[0];
    if (x >= (float) last) return g[last];
    // Two nodes = ONE STRAIGHT LINE (the duplicated-endpoint spline below
    // would ease in/out into an S shape between them).
    if (n == 2) return g[0] + (g[1] - g[0]) * x;
    int k = (int) x;
    if (k > last - 1) k = last - 1;
    const float t  = x - (float) k;
    const float p0 = g[(k > 0) ? k - 1 : 0];
    const float p1 = g[k];
    const float p2 = g[k + 1];
    const float p3 = g[(k + 2 <= last) ? k + 2 : last];
    const float t2 = t * t, t3 = t2 * t;
    float db = 0.5f * ((2.0f * p1)
                     + (p2 - p0) * t
                     + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                     + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
    if (db >  clampDb) db =  clampDb;
    if (db < -clampDb) db = -clampDb;
    return db;
}
