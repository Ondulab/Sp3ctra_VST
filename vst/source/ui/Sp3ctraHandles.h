/**
 * @file Sp3ctraHandles.h
 * @brief THE painter of everything grabbable in a Sp3ctra graphic editor —
 *        nodes, secondary rings, chevrons, thumbs, grabbable lines, type
 *        chips and the drag readout. One file: restyle it here and every
 *        editor (ADSR, filter, EQ, LEVELS, CENTROID, ECHO, REVERB…) follows.
 *
 * Colour split of a module page:
 *   • DISPLAY (curves, fills, frames, captions, box labels, "--- SECTION ---")
 *     = the module's category colour (ModuleCatalog::moduleColour);
 *   • CONTROLS (handles, bar sliders, toggles, chips)
 *     = Sp3ctraTheme::kColHandle — one vivid hue on every page.
 *   A handle NEVER takes the module colour: "what you touch" must pop out of
 *   "what you look at".
 *
 * State grammar — one hue, four states, each visibly hotter than the last:
 *   Idle      hollow: dark core (kColHandleCore) + lime ring
 *   Selected  filled lime core + lime ring + thin outer ring (persistent
 *             selection: the handle the value boxes / MIDI CCs talk to)
 *   Hover     filled lime core + soft halo
 *   Drag      filled lime core + WHITE ring + halo (+ lime readout)
 *
 * Editors keep their own hit-testing and geometry; they only fold their
 * (dragging, hovered, selected) booleans into a State via stateOf() and call
 * the painter. Ghosted grips (e.g. a DJ knob in its dead zone) pass alpha.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "../UITheme.h"

namespace Sp3ctraHandles
{
    enum class State { Idle, Selected, Hover, Drag };

    inline juce::Colour colour() noexcept { return juce::Colour(Sp3ctraTheme::kColHandle); }
    inline juce::Colour hot()    noexcept { return juce::Colour(Sp3ctraTheme::kColHandleHot); }
    inline juce::Colour core()   noexcept { return juce::Colour(Sp3ctraTheme::kColHandleCore); }

    inline bool isHot(State s)    noexcept { return s == State::Hover || s == State::Drag; }
    inline bool isFilled(State s) noexcept { return s != State::Idle; }

    /** Fold the usual editor booleans into a state. */
    inline State stateOf(bool dragging, bool hovered, bool selected = false) noexcept
    {
        if (dragging) return State::Drag;
        if (hovered)  return State::Hover;
        return selected ? State::Selected : State::Idle;
    }

    // ── Geometry tokens ────────────────────────────────────────────────────
    constexpr float kNodeR    = 4.5f;   ///< standard node radius
    constexpr float kRingR    = 4.0f;   ///< secondary (ring-only) handle radius
    constexpr float kHaloPad  = 3.0f;   ///< halo / outer-ring clearance
    constexpr float kRingW    = 1.4f;   ///< idle / selected ring width
    constexpr float kRingHotW = 1.8f;   ///< drag ring width
    constexpr float kLineW    = 1.6f;   ///< grabbable line, idle
    constexpr float kLineHotW = 2.4f;   ///< grabbable line, hot

    /** Ring colour for a state (white while dragging), alpha-scaled. */
    inline juce::Colour ringColour(State s, float alpha = 1.0f) noexcept
    {
        return (s == State::Drag ? hot() : colour()).withMultipliedAlpha(alpha);
    }

    /** Fill colour for a state (dark core when idle), alpha-scaled. */
    inline juce::Colour fillColour(State s, float alpha = 1.0f) noexcept
    {
        return (isFilled(s) ? colour() : core()).withMultipliedAlpha(alpha);
    }

    // ── Primitives ─────────────────────────────────────────────────────────

    /** Soft halo under a hot handle. */
    inline void drawHalo(juce::Graphics& g, juce::Point<float> c, float r, float alpha = 1.0f)
    {
        const float R = r + kHaloPad;
        g.setColour(colour().withMultipliedAlpha(0.28f * alpha));
        g.fillEllipse(c.x - R, c.y - R, 2.0f * R, 2.0f * R);
    }

    /** Round node — ADSR nodes, EQ handles, filter edges, echo taps… */
    inline void drawNode(juce::Graphics& g, juce::Point<float> c, State s,
                         float r = kNodeR, float alpha = 1.0f)
    {
        const float rad = isHot(s) ? r + 1.0f : r;
        if (isHot(s))
            drawHalo(g, c, rad, alpha);
        if (s == State::Selected)
        {
            const float R = rad + kHaloPad;
            g.setColour(colour().withMultipliedAlpha(0.8f * alpha));
            g.drawEllipse(c.x - R, c.y - R, 2.0f * R, 2.0f * R, 1.0f);
        }
        g.setColour(fillColour(s, alpha));
        g.fillEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad);
        g.setColour(ringColour(s, alpha));
        g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad,
                      s == State::Drag ? kRingHotW : kRingW);
    }

    /** Secondary ring-only handle (curve bend, slope) — smaller, hollow
     *  when idle, filled when hot. */
    inline void drawRing(juce::Graphics& g, juce::Point<float> c, State s,
                         float r = kRingR, float alpha = 1.0f)
    {
        const float rad = isHot(s) ? r + 1.0f : r;
        if (isHot(s))
        {
            g.setColour(colour().withMultipliedAlpha(alpha));
            g.fillEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad);
        }
        g.setColour(ringColour(s, alpha).withMultipliedAlpha(isHot(s) ? 1.0f : 0.75f));
        g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad,
                      isHot(s) ? kRingHotW : 1.2f);
    }

    /** Rounded thumb (fader thumb, line grip). */
    inline void drawThumb(juce::Graphics& g, juce::Rectangle<float> r, State s,
                          float corner = 2.0f, float alpha = 1.0f)
    {
        if (isHot(s))
        {
            g.setColour(colour().withMultipliedAlpha(0.28f * alpha));
            g.fillRoundedRectangle(r.expanded(kHaloPad), corner + 1.0f);
        }
        g.setColour(fillColour(s, alpha));
        g.fillRoundedRectangle(r, corner);
        g.setColour(ringColour(s, alpha));
        g.drawRoundedRectangle(r, corner, s == State::Drag ? kRingHotW : 1.0f);
    }

    /** Grabbable line (the LEVELS floor, a threshold…) — lime, thicker and
     *  white-cored when hot. `dashed` keeps the editor's dashed reading. */
    inline void drawGrabLine(juce::Graphics& g, juce::Line<float> l, State s,
                             bool dashed = false, float alpha = 1.0f)
    {
        const float w = isHot(s) ? kLineHotW : kLineW;
        if (isHot(s))
        {
            g.setColour(colour().withMultipliedAlpha(0.28f * alpha));
            g.drawLine(l, w + 4.0f);
        }
        g.setColour((s == State::Drag ? hot() : colour())
                        .withMultipliedAlpha((isHot(s) ? 1.0f : 0.85f) * alpha));
        if (dashed)
        {
            const float dash[2] = { 5.0f, 3.0f };
            g.drawDashedLine(l, dash, 2, w);
        }
        else
            g.drawLine(l, w);
    }

    /** Chevron arrowhead at `tip`, pointing along the unit vector `dir`
     *  (the EQ second-setting grips: width / slope / tilt lever). */
    inline void drawChevron(juce::Graphics& g, juce::Point<float> tip,
                            juce::Point<float> dir, State s, float alpha = 1.0f)
    {
        juce::Path ch;
        ch.startNewSubPath(-2.5f, -4.0f);
        ch.lineTo         ( 2.5f,  0.0f);
        ch.lineTo         (-2.5f,  4.0f);
        ch.applyTransform(juce::AffineTransform::rotation(std::atan2(dir.y, dir.x))
                              .translated(tip.x, tip.y));
        if (isHot(s))
        {
            g.setColour(colour().withMultipliedAlpha(0.28f * alpha));
            g.strokePath(ch, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        }
        g.setColour(ringColour(s, alpha).withMultipliedAlpha(isHot(s) ? 1.0f : 0.9f));
        g.strokePath(ch, juce::PathStrokeType(isHot(s) ? 2.2f : 1.6f));
    }

    /** Link segment handle → grip (the second setting's span). */
    inline void drawLink(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b,
                         State s, float alpha = 1.0f)
    {
        g.setColour(colour().withMultipliedAlpha((isHot(s) ? 0.75f : 0.45f) * alpha));
        g.drawLine(a.x, a.y, b.x, b.y, 1.1f);
    }

    /** Selectable type chip (BELL / LP / HP…): ON = filled lime with dark
     *  text, OFF = lime outline + lime text (brighter on hover). */
    inline void drawChip(juce::Graphics& g, juce::Rectangle<float> r,
                         const juce::String& text, bool on, bool hot,
                         float corner = 3.0f)
    {
        if (on)
        {
            g.setColour(colour().withAlpha(hot ? 1.0f : 0.9f));
            g.fillRoundedRectangle(r, corner);
            g.setColour(juce::Colour(0xff14141c));
        }
        else
        {
            g.setColour(colour().withAlpha(hot ? 0.75f : 0.35f));
            g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
            g.setColour(colour().withAlpha(hot ? 1.0f : 0.7f));
        }
        g.drawText(text, r.toNearestInt(), juce::Justification::centred, false);
    }

    /** Live readout next to a dragged handle: dark pill + lime text, kept
     *  inside `clip` (the plot). Anchor = the handle centre; the pill sits
     *  above-right of it, flipping when it would leave the plot. */
    inline void drawReadout(juce::Graphics& g, const juce::String& text,
                            juce::Point<float> anchor, juce::Rectangle<float> clip)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), text) + 10;
        const int h = 16;
        float x = anchor.x + 10.0f;
        float y = anchor.y - (float) h - 6.0f;
        if (x + (float) w > clip.getRight()) x = anchor.x - 10.0f - (float) w;
        if (y < clip.getY())                 y = anchor.y + 8.0f;
        x = juce::jmax(clip.getX(), x);
        const juce::Rectangle<float> pill(x, y, (float) w, (float) h);
        g.setColour(core().withAlpha(0.92f));
        g.fillRoundedRectangle(pill, 3.0f);
        g.setColour(colour().withAlpha(0.6f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 3.0f, 1.0f);
        g.setColour(colour());
        g.drawText(text, pill.toNearestInt(), juce::Justification::centred, false);
    }
} // namespace Sp3ctraHandles
