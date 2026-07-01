#pragma once

/*
 * FadeCurve.h
 *
 * Fade curve shaping utility for LuxSampler crossfades.
 * Provides configurable curve types and power control applied to all
 * fade parameters (Attack, Decay, TrebleCut, BassCut).
 *
 * RT-safe: no allocation, no I/O, bounded math only.
 */

#include <cmath>
#include <algorithm>

// ============================================================================
// FadeCurveType — selectable curve shapes
// ============================================================================
enum class FadeCurveType : int
{
    LINEAR      = 0, // f(t) = t                   — constant-rate transition
    EXPONENTIAL = 1, // f(t) = t^power              — concave (slow start, fast end)
    LOGARITHMIC = 2, // f(t) = t^(1/power)          — convex  (fast start, slow end)
    SCURVE      = 3  // f(t) = smoothstep(t)^power  — S-shaped (slow-fast-slow)
};

constexpr int kNumFadeCurveTypes = 4;

/**
 * @brief Apply curve shaping to a normalised fade position.
 *
 * @param t     Input position [0..1] — 0 = no effect, 1 = full effect.
 * @param type  Curve shape to apply. Selecting a non-LINEAR type is ALWAYS
 *              visibly curved (the type selector has an immediate effect),
 *              independent of @p power.
 * @param power Curve INTENSITY [0.1..10.0]:
 *              - small (→0.1) = gentle, close to LINEAR
 *              - 1.0          = default, a clear moderate curve (EXP ≈ t², LOG ≈ √t)
 *              - large (→10)  = extreme knee
 *              LINEAR ignores @p power entirely.
 * @return      Shaped output [0..1].
 *
 * Rationale: the exponent is mapped as (1 + power) so that EXP/LOG never
 * collapse onto LINEAR at the default power of 1.0 — previously power==1.0 was a
 * neutral point where LIN/EXP/LOG were identical and the type selector looked
 * broken. "No curve" is now expressed by choosing LINEAR, not by a power value.
 *
 * All curve types are monotonically increasing and pass through (0,0) and (1,1).
 * This function is RT-safe: no allocation, no blocking, bounded computation.
 */
inline float applyFadeCurve(float t,
                            FadeCurveType type,
                            float power) noexcept
{
    // Clamp to valid range
    t = std::max(0.0f, std::min(1.0f, t));
    const float p   = std::max(0.1f, power);
    const float exp = 1.0f + p; // 1.1 (gentle) .. 11 (extreme); default(1.0)→2

    switch (type)
    {
        case FadeCurveType::LINEAR:
            return t;

        case FadeCurveType::EXPONENTIAL:
            // Concave "ease-in": slow start then fast rise. exp∈[1.1..11].
            return std::pow(t, exp);

        case FadeCurveType::LOGARITHMIC:
            // Convex "ease-out": fast start then gentle saturation (mirror of EXP).
            return std::pow(t, 1.0f / exp);

        case FadeCurveType::SCURVE:
        {
            // Hermite smoothstep: 3t² − 2t³  →  S-shaped [0..1]→[0..1].
            const float s = t * t * (3.0f - 2.0f * t);
            // Power sharpens the knee; s is already non-linear at any power.
            return std::pow(s, p);
        }
    }

    return t; // fallback — unreachable
}

/**
 * @brief Label strings for each curve type (for UI display).
 *
 * Indexed by static_cast<int>(FadeCurveType).
 */
inline const char* fadeCurveTypeName(FadeCurveType type) noexcept
{
    switch (type)
    {
        case FadeCurveType::LINEAR:      return "LIN";
        case FadeCurveType::EXPONENTIAL: return "EXP";
        case FadeCurveType::LOGARITHMIC: return "LOG";
        case FadeCurveType::SCURVE:      return "S";
    }
    return "?";
}
