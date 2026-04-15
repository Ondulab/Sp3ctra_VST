/**
 * @file IconPaths.h
 * @brief Shared vector icon paths for the Sp3ctra UI.
 *
 * All path coordinates are normalised to a [0,1] square.
 * Use Icons::fillPath() to render them at any size.
 *
 * Categories:
 *   - Transport: play, pause (hold), stop
 *   - Source identification: signal (RAW), loop (Sampler), wave (Live), layers (Mix)
 *   - Indicator: eye (active visualizer source)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Icons
{

// ─────────────────────────────────────────────────────────────────────────────
// Transport icons
// ─────────────────────────────────────────────────────────────────────────────

/** Filled right-pointing triangle (play). */
inline juce::Path play()
{
    juce::Path p;
    p.addTriangle(0.12f, 0.04f, 0.12f, 0.96f, 0.92f, 0.50f);
    return p;
}

/** Two vertical bars (pause / hold). */
inline juce::Path pause()
{
    juce::Path p;
    p.addRectangle(0.15f, 0.10f, 0.28f, 0.80f);
    p.addRectangle(0.57f, 0.10f, 0.28f, 0.80f);
    return p;
}

/** Filled square (stop). */
inline juce::Path stop()
{
    juce::Path p;
    p.addRectangle(0.10f, 0.10f, 0.80f, 0.80f);
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Source identification icons
// ─────────────────────────────────────────────────────────────────────────────

/** RAW / signal: zigzag lightning bolt. */
inline juce::Path signal()
{
    juce::Path p;
    p.startNewSubPath(0.55f, 0.05f);
    p.lineTo(0.30f, 0.45f);
    p.lineTo(0.55f, 0.45f);
    p.lineTo(0.40f, 0.95f);
    p.lineTo(0.70f, 0.48f);
    p.lineTo(0.48f, 0.48f);
    p.lineTo(0.65f, 0.05f);
    p.closeSubPath();
    return p;
}

/** Sampler / loop: circular arrow. */
inline juce::Path loop()
{
    juce::Path p;
    // Arc (open circle)
    p.addArc(0.15f, 0.15f, 0.70f, 0.70f,
             -0.3f, juce::MathConstants<float>::twoPi - 0.9f, true);
    // Arrow head at end of arc
    const float ax = 0.50f + 0.35f * std::cos(-0.3f);
    const float ay = 0.50f + 0.35f * std::sin(-0.3f);
    p.startNewSubPath(ax - 0.12f, ay - 0.06f);
    p.lineTo(ax, ay);
    p.lineTo(ax - 0.04f, ay + 0.14f);
    return p;
}

/** Live / wave: three wavy horizontal lines. */
inline juce::Path wave()
{
    juce::Path p;
    for (int i = 0; i < 3; ++i)
    {
        const float y = 0.25f + i * 0.22f;
        p.startNewSubPath(0.10f, y);
        p.cubicTo(0.30f, y - 0.10f, 0.45f, y + 0.10f, 0.60f, y);
        p.cubicTo(0.72f, y - 0.08f, 0.80f, y + 0.06f, 0.90f, y);
    }
    return p;
}

/** Mix / layers: two overlapping rectangles. */
inline juce::Path layers()
{
    juce::Path p;
    // Back rectangle (offset up-left)
    p.addRoundedRectangle(0.10f, 0.10f, 0.55f, 0.45f, 0.04f);
    // Front rectangle (offset down-right)
    p.addRoundedRectangle(0.35f, 0.38f, 0.55f, 0.45f, 0.04f);
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Indicator icons
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Eye icon — indicates the currently visualised source.
 * Outer: almond/eye shape (two arcs).
 * Inner: filled circle (iris/pupil).
 */
inline juce::Path eye()
{
    juce::Path p;
    // Outer eye shape — two quadratic arcs forming an almond
    p.startNewSubPath(0.08f, 0.50f);
    p.cubicTo(0.25f, 0.15f, 0.75f, 0.15f, 0.92f, 0.50f);
    p.cubicTo(0.75f, 0.85f, 0.25f, 0.85f, 0.08f, 0.50f);
    p.closeSubPath();

    // Iris — filled circle
    p.addEllipse(0.35f, 0.30f, 0.30f, 0.40f);

    // Pupil — smaller filled circle (will be same colour, creates layered look)
    p.addEllipse(0.42f, 0.38f, 0.16f, 0.24f);

    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────

/** Scale a normalised [0,1] path to fit inside @p area and fill it. */
inline void fillPath(juce::Graphics& g, const juce::Path& src,
                     juce::Rectangle<float> area, juce::Colour col)
{
    juce::Path scaled = src;
    scaled.applyTransform(src.getTransformToScaleToFit(area, true));
    g.setColour(col);
    g.fillPath(scaled);
}

/** Scale a normalised [0,1] path to fit inside @p area and stroke it. */
inline void strokePath(juce::Graphics& g, const juce::Path& src,
                       juce::Rectangle<float> area, juce::Colour col,
                       float thickness = 1.5f)
{
    juce::Path scaled = src;
    scaled.applyTransform(src.getTransformToScaleToFit(area, true));
    g.setColour(col);
    g.strokePath(scaled, juce::PathStrokeType(thickness));
}

} // namespace Icons
