#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

/**
 * @file VideoScrollMode.h
 * @brief Shared VIDEO SCROLL constants + helpers (param bounds, output labels).
 *
 * Historically hosted the 4-way orientation enum (0/90/180/270°). Since
 * 2026-08-28 the orientation is the continuous "rotation" param — degrees,
 * clockwise on screen: 0 = new lines at the bottom / scroll up, 90 = at the
 * left, 180 = scroll down, 270 = at the right (the former mode × 90°). See
 * docs/PLAN_VIDEO_SCROLL_CHAIN_PAGES_ZOOM.md.
 */

/** Bounds shared by the APVTS param ranges (PluginProcessor) and the renderer
 *  clamps (VideoScrollRenderCore). Zoom is the width of the generation band
 *  relative to the visible span (D5/D7). */
namespace VideoScrollLimits
{
    constexpr float kZoomMin     = 0.05f;
    constexpr float kZoomMax     = 4.0f;
    constexpr float kRotationMax = 360.0f;   // degrees, wraps

    /** Attenuation law (2026-09-04) — travel of the curve's free middle point
     *  along the distance axis.
     *
     *  0.25 / 0.75 is not a taste bound, it is the MONOTONICITY bound. The law
     *  is a quadratic Bézier through that point, whose x-control lands at
     *  2·midX − 0.5; outside [0.25, 0.75] that control leaves the segment, the
     *  curve doubles back in x, and "the level at distance a" stops being a
     *  function at all (verified numerically: the endpoints then unpin).
     *  It also keeps the handle clear of the far-edge one — two handles that
     *  overlap stop saying anything. */
    constexpr float kFadeMidXMin = 0.25f;
    constexpr float kFadeMidXMax = 0.75f;

    /** Time packing ("pack", bipolar). The screen→buffer map is
     *  gg = ad + c·ad²/span; its derivative 1 + 2·c·ad/span stays positive
     *  over ad ∈ [0, span] only while c > −0.5, so decompression stops just
     *  short of that — past it the map folds and history would run backwards. */
    constexpr float kPackMax = 2.5f;     // v = +1 — hardest packing
    constexpr float kPackMin = -0.49f;   // v = −1 — widest spread, still monotonic
}

/** Bipolar packing → the quadratic coefficient of the screen→buffer map.
 *  ONE conversion for the renderer's warp and the grid cell that draws it. */
inline float videoScrollPackCoeff(float v) noexcept
{
    v = juce::jlimit(-1.0f, 1.0f, v);
    return v * (v >= 0.0f ? VideoScrollLimits::kPackMax : -VideoScrollLimits::kPackMin);
}

/**
 * @brief The attenuation law — the level still kept at normalised distance
 *        `a` (0 at the birth line, 1 at the far edge of the window).
 *
 * A quadratic Bézier from (0, 1) to (1, 1 − fade) that passes EXACTLY through
 * the free middle point (midX, midY) at its half-way parameter. While
 * `midFree` is false that point rides the straight segment, so the law is a
 * plain linear attenuation and `fade` alone dials it: this is what makes
 * "lower the right end" behave before the curve is ever bent.
 *
 * THE single source of the law: VideoScrollRenderCore::buildWarp dims with it
 * and the grid's Attenuation cell draws it, so the curve under the pointer is
 * the curve the output obeys.
 */
inline float videoScrollAttenLevel(float a, float fade,
                                   float midX, float midY, bool midFree) noexcept
{
    a    = juce::jlimit(0.0f, 1.0f, a);
    fade = juce::jlimit(0.0f, 1.0f, fade);
    if (! midFree)                       // the untouched law is a straight line
        return 1.0f - fade * a;

    midX = juce::jlimit(VideoScrollLimits::kFadeMidXMin,
                        VideoScrollLimits::kFadeMidXMax, midX);
    midY = juce::jlimit(0.0f, 1.0f, midY);

    // Control point that puts the curve through (midX, midY) at t = 0.5.
    const float y2 = 1.0f - fade;
    const float cx = 2.0f * midX - 0.5f;
    const float cy = 2.0f * midY - 0.5f * (1.0f + y2);

    // Solve Bx(t) = a for t: t²(1 − 2cx) + 2cx·t − a = 0.
    const float k = 1.0f - 2.0f * cx;
    float t;
    if (std::abs(k) < 1.0e-4f)
        t = (std::abs(cx) < 1.0e-6f) ? a : juce::jlimit(0.0f, 1.0f, a / (2.0f * cx));
    else
    {
        const float disc = juce::jmax(0.0f, cx * cx + k * a);
        t = juce::jlimit(0.0f, 1.0f, (std::sqrt(disc) - cx) / k);
    }

    const float u = 1.0f - t;
    return juce::jlimit(0.0f, 1.0f, u * u * 1.0f + 2.0f * t * u * cy + t * t * y2);
}

/** Display labels of the patched VIDEO SCROLL outputs — the host chain's
 *  outward label ("CHAIN n", or the user chain name when set), suffixed a/b/…
 *  when one chain hosts several probes. `slotsChains` = {slot, chainIdx} in
 *  RACK order (Sp3ctraAudioProcessor::activeVideoSlots()); `chainNames` =
 *  per-chain user labels indexed by chainIdx, "" = unnamed
 *  (Sp3ctraAudioProcessor::chainNames()). ONE helper for the VIDEO MIX strip,
 *  the zone-3 chain tabs and the ALL view, so the three always name an output
 *  the same way. */
inline juce::StringArray videoScrollOutputLabels(
    const std::vector<std::pair<int, int>>& slotsChains,
    const juce::StringArray& chainNames = {})
{
    std::map<int, int> perChain, seen;
    for (const auto& sc : slotsChains)
        ++perChain[sc.second];

    juce::StringArray out;
    for (const auto& sc : slotsChains)
    {
        const int c = sc.second;
        juce::String label = (c < chainNames.size() && chainNames[c].isNotEmpty())
                                 ? chainNames[c]
                                 : "CHAIN " + juce::String(c + 1);
        if (perChain[c] > 1)
            label += juce::String::charToString((juce::juce_wchar) ('a' + seen[c]++));
        out.add(label);
    }
    return out;
}
