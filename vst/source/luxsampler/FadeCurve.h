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
 * @param type  Curve shape to apply.
 * @param power Curve intensity [0.1..10.0]:
 *              - 1.0  = neutral (LINEAR-equivalent for EXP/LOG)
 *              - >1.0 = sharper transition
 *              - <1.0 = gentler transition
 * @return      Shaped output [0..1].
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

    switch (type)
    {
        case FadeCurveType::LINEAR:
            return t;

        case FadeCurveType::EXPONENTIAL:
            // power > 1 → slow ramp then fast rise (concave / "ease in")
            // power < 1 → fast ramp then slow rise (convex / "ease out")
            return std::pow(t, power);

        case FadeCurveType::LOGARITHMIC:
            // Inverse of exponential:
            // power > 1 → fast start, gentle saturation ("ease out")
            // power < 1 → slow start, fast finish ("ease in")
            return std::pow(t, 1.0f / std::max(0.1f, power));

        case FadeCurveType::SCURVE:
        {
            // Hermite smoothstep: 3t² − 2t³  →  S-shaped [0..1]→[0..1]
            const float s = t * t * (3.0f - 2.0f * t);
            // Power shapes the S: >1 = sharper knee, <1 = flatter
            return (power > 0.999f && power < 1.001f)
                       ? s
                       : std::pow(s, power);
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
