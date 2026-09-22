/**
 * @file VideoMixFocus.h
 * @brief THE projector law of the VIDEO MIX radar — shared by the toile
 *        (ui/VideoMixRadar.h) and the compositor (VideoMixerComponent's
 *        render thread) so the polygon the user drags is exactly what is
 *        rendered.
 *
 * The radar puts every patched VIDEO SCROLL output on a SPOKE — rack order,
 * clockwise from the top. Each spoke carries that output's own mix level
 * (videoMix{slot}_level, unchanged, 0 = centre … 1 = rim). On top of those
 * base levels ONE global handle, the PROJECTOR (videoMixFocusX / Y, a unit
 * disc, X right / Y up), is the one-hand performance gesture: pulled toward
 * a spoke it progressively dims the OTHER outputs without touching their
 * base levels; at the centre it is neutral, so an XY pad springing back to
 * rest changes nothing. It can only MASK, never invent — an output's
 * effective level is level × weight with weight ≤ 1:
 *
 *   weight_i = 1 − m · (1 − max(0, cos(angle between the focus and spoke i)))
 *   m        = |focus| clamped to 1
 *
 * Full throw toward spoke A → A untouched, the perpendicular spokes and the
 * opposite one at 0 (a solo). Two OPPOSITE outputs alone = both spokes up,
 * projector at the centre — the mix a single-handle star can never reach.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace VideoMixFocus
{
    /** The two global APVTS ids of the projector (−1 … 1, default 0). */
    inline constexpr const char* kXId = "videoMixFocusX";
    inline constexpr const char* kYId = "videoMixFocusY";

    /** Dead zone around the centre: below this radius the projector is
     *  exactly neutral (a resting XY pad never leaves a residual tilt). */
    inline constexpr float kDeadZone = 0.04f;

    /** Direction of spoke `i` of `n` — angle CLOCKWISE from straight up
     *  (radians). Index = rack order of the patched outputs. */
    inline float spokeAngle(int i, int n) noexcept
    {
        if (n <= 0) return 0.0f;
        return juce::MathConstants<float>::twoPi * (float) i / (float) n;
    }

    /** Unit vector of spoke `i` in a (right, up) frame. */
    inline juce::Point<float> spokeDir(int i, int n) noexcept
    {
        const float a = spokeAngle(i, n);
        return { std::sin(a), std::cos(a) };
    }

    /** The focus (fx right, fy up) snapped to the unit disc + dead zone —
     *  the vector every reader of the parameters must agree on. */
    inline juce::Point<float> focusVector(float fx, float fy) noexcept
    {
        const float m = std::sqrt(fx * fx + fy * fy);
        if (m < kDeadZone) return { 0.0f, 0.0f };
        if (m > 1.0f)      return { fx / m, fy / m };
        return { fx, fy };
    }

    /** Projector weight (0 … 1) of spoke `i` of `n` for the focus (fx, fy). */
    inline float weight(int i, int n, float fx, float fy) noexcept
    {
        const auto  f = focusVector(fx, fy);
        const float m = std::sqrt(f.x * f.x + f.y * f.y);
        if (m <= 0.0f) return 1.0f;
        const auto  d    = spokeDir(i, n);
        const float c    = (f.x * d.x + f.y * d.y) / m;   // cos(focus, spoke)
        const float keep = juce::jmax(0.0f, c);
        return juce::jlimit(0.0f, 1.0f, 1.0f - m * (1.0f - keep));
    }

    /** The projector's law for the SOUND — the VIDEO MIX → AUDIO link
     *  (video/VideoMixFollow.h): the weight the projector ALONE puts on the
     *  audio sends of the chain behind spoke `i` of `n`. Not a mask like the
     *  picture's law above but a CROSSFADE — the centre is the even mix, a
     *  spoke is a solo, and the projector blends between them:
     *
     *    keep_i   = max(0, 1 − |angle(focus, spoke i)| / (2π / n))
     *    weight_i = (1 − m) · ½ + m · keep_i
     *
     *  Centre → every chain at 50 %. Full throw onto spoke A → A at 100 %,
     *  every other chain at 0 (keep reaches 0 exactly at the neighbouring
     *  spokes, whatever n). Between two spokes → a straight crossfade of
     *  those two; back toward the centre → everyone returns to 50 %. The
     *  handles play no part: the sound follows the one-hand gesture only. */
    inline float audioWeight(int i, int n, float fx, float fy) noexcept
    {
        const auto  f = focusVector(fx, fy);
        const float m = std::sqrt(f.x * f.x + f.y * f.y);
        if (m <= 0.0f || n <= 0) return 0.5f;
        const float pitch = juce::MathConstants<float>::twoPi / (float) n;
        float diff = std::atan2(f.x, f.y) - spokeAngle(i, n);            // clockwise from up
        diff = std::remainder(diff, juce::MathConstants<float>::twoPi);   // → [−π, π]
        const float keep = juce::jmax(0.0f, 1.0f - std::abs(diff) / pitch);
        return juce::jlimit(0.0f, 1.0f, 0.5f * (1.0f - m) + m * keep);
    }
} // namespace VideoMixFocus
