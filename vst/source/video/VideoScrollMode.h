#pragma once

#include <juce_core/juce_core.h>
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
}

/** Display labels of the patched VIDEO SCROLL outputs — "CHAIN n", suffixed
 *  a/b/… when one chain hosts several probes. `slotsChains` = {slot, chainIdx}
 *  in RACK order (Sp3ctraAudioProcessor::activeVideoSlots()). ONE helper for
 *  the VIDEO MIX strip, the zone-3 chain tabs and the ALL view, so the three
 *  always name an output the same way. */
inline juce::StringArray videoScrollOutputLabels(const std::vector<std::pair<int, int>>& slotsChains)
{
    std::map<int, int> perChain, seen;
    for (const auto& sc : slotsChains)
        ++perChain[sc.second];

    juce::StringArray out;
    for (const auto& sc : slotsChains)
    {
        juce::String label = "CHAIN " + juce::String(sc.second + 1);
        if (perChain[sc.second] > 1)
            label += juce::String::charToString((juce::juce_wchar) ('a' + seen[sc.second]++));
        out.add(label);
    }
    return out;
}
