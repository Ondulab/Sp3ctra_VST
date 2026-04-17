#pragma once

#include <juce_core/juce_core.h>

/**
 * @file VideoScrollMode.h
 * @brief Video orientation modes — 4 simple rotation angles.
 *
 * The internal scroll buffer always advances row-by-row (vertical).
 * The orientation angle is applied as an AffineTransform in paint(),
 * rotating the rendered waterfall to the desired screen direction.
 *
 *   Deg0   — vertical scroll, new data arrives at bottom  (↑ scroll up)
 *   Deg90  — horizontal scroll, new data at right         (← scroll left = L->R)
 *   Deg180 — vertical scroll, new data at top             (↓ scroll down)
 *   Deg270 — horizontal scroll, new data at left          (→ scroll right = R->L)
 */
enum class VideoScrollMode : int
{
    Deg0   = 0,   ///< 0°   — new data at bottom, waterfall scrolls up
    Deg90  = 1,   ///< 90°  — new data at right,  waterfall scrolls left  (L->R)
    Deg180 = 2,   ///< 180° — new data at top,    waterfall scrolls down
    Deg270 = 3,   ///< 270° — new data at left,   waterfall scrolls right (R->L)
    COUNT  = 4
};

/** Rotation angle in radians for each mode (used by AffineTransform in paint()). */
inline float videoScrollModeAngle(VideoScrollMode m)
{
    return static_cast<int>(m) * juce::MathConstants<float>::halfPi;
}

/** Short label shown in the toolbar overlay. */
inline const char* videoScrollModeLabel(VideoScrollMode m)
{
    switch (m)
    {
        case VideoScrollMode::Deg0:   return "0 deg";
        case VideoScrollMode::Deg90:  return "90 deg";
        case VideoScrollMode::Deg180: return "180 deg";
        case VideoScrollMode::Deg270: return "270 deg";
        default:                      return "???";
    }
}
