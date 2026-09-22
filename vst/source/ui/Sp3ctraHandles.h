/**
 * @file Sp3ctraHandles.h
 * @brief THE painter of everything grabbable in a Sp3ctra graphic editor —
 *        nodes, secondary rings, chevrons, thumbs, grabbable lines, type
 *        chips and the drag readout. One file: restyle it here and every
 *        editor (ADSR, filter, EQ, LEVELS, CENTROID, ECHO, REVERB…) follows.
 *
 * This file owns the SHAPES. The interaction grammar behind them — the four
 * rungs, their colours, alphas and ring widths, and the heat of a remote
 * (MIDI / automation) edit — lives in ui/Sp3ctraControls.h and is shared
 * with every widget through Sp3ctraLookAndFeel. Nothing here invents a
 * colour: each primitive resolves Sp3ctraControls::inkOf() and strokes it.
 *
 * Colour split of a module page:
 *   • DISPLAY (curves, fills, frames, captions, box labels, "--- SECTION ---")
 *     = the module's category colour (ModuleCatalog::moduleColour);
 *   • CONTROLS (handles, bar sliders, toggles, chips)
 *     = Sp3ctraTheme::kColHandle — one vivid hue on every page.
 *   A handle NEVER takes the module colour: "what you touch" must pop out of
 *   "what you look at".
 *
 * State grammar — one hue, four rungs, each visibly hotter than the last:
 *   Idle      hollow: dark core + DESATURATED ring — a page at rest is calm
 *   Selected  filled core + ring + thin outer ring (persistent selection:
 *             the handle the value boxes / MIDI CCs talk to)
 *   Hover     filled core + full-accent ring + halo
 *   Edit      filled core + WHITE ring + halo (+ lime readout) — a mouse
 *             drag, and equally a MIDI CC or automation move, which keeps
 *             the handle lit for Sp3ctraTheme::kCtlGlowMs afterwards (heat)
 *
 * Editors keep their own hit-testing and geometry; they only fold their
 * (dragging, hovered, selected) booleans — and, when the handle is bound to
 * a parameter, its Sp3ctraControls::Bound heat — into a Look via stateOf()
 * and call the painter. Ghosted grips (e.g. a DJ knob in its dead zone)
 * pass alpha.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "../UITheme.h"
#include "Sp3ctraControls.h"

namespace Sp3ctraHandles
{
    // ── The shared grammar, re-exported under the painter's name ───────────
    using State = Sp3ctraControls::State;
    using Look  = Sp3ctraControls::Look;
    using Ink   = Sp3ctraControls::Ink;

    inline juce::Colour colour() noexcept { return Sp3ctraControls::active(); }
    inline juce::Colour rest()   noexcept { return Sp3ctraControls::rest(); }
    inline juce::Colour hot()    noexcept { return Sp3ctraControls::hot(); }
    inline juce::Colour core()   noexcept { return Sp3ctraControls::core(); }

    inline bool isHot(Look s)    noexcept { return s.hot(); }
    inline bool isFilled(Look s) noexcept { return s.filled(); }

    /** Fold the usual editor booleans (+ the parameter's remote-edit heat)
     *  into a Look. */
    inline Look stateOf(bool dragging, bool hovered, bool selected = false,
                        float heat = 0.0f) noexcept
    {
        return Sp3ctraControls::stateOf(dragging, hovered, selected, heat);
    }

    /** The ink of a handle — every primitive below starts here. (Named
     *  `ink` and not `inkOf`: Look lives in Sp3ctraControls, so an unqualified
     *  `inkOf` would find both through ADL.) */
    inline Ink ink(Look s, float alpha = Sp3ctraControls::kRoleHandle) noexcept
    {
        return Sp3ctraControls::inkOf(s, colour(), alpha);
    }

    // ── Geometry tokens ────────────────────────────────────────────────────
    constexpr float kNodeR    = 4.5f;   ///< standard node radius
    constexpr float kRingR    = 4.0f;   ///< secondary (ring-only) handle radius
    constexpr float kHaloPad  = 3.0f;   ///< halo / outer-ring clearance
    constexpr float kLineW    = 1.6f;   ///< grabbable line, idle
    constexpr float kLineHotW = 2.4f;   ///< grabbable line, hot

    /** Ring colour for a state (white while edited), alpha-scaled. */
    inline juce::Colour ringColour(Look s, float alpha = 1.0f) noexcept
    {
        return ink(s, alpha).line;
    }

    /** Fill colour for a state (dark core when idle), alpha-scaled. */
    inline juce::Colour fillColour(Look s, float alpha = 1.0f) noexcept
    {
        return ink(s, alpha).fill;
    }

    // ── Primitives ─────────────────────────────────────────────────────────

    /** Soft halo under a hot handle. */
    inline void drawHalo(juce::Graphics& g, juce::Point<float> c, float r,
                         juce::Colour halo)
    {
        if (halo.getFloatAlpha() <= 0.004f) return;
        const float R = r + kHaloPad;
        g.setColour(halo);
        g.fillEllipse(c.x - R, c.y - R, 2.0f * R, 2.0f * R);
    }

    /** Round node — ADSR nodes, EQ handles, filter edges, echo taps… */
    inline void drawNode(juce::Graphics& g, juce::Point<float> c, Look s,
                         float r = kNodeR, float alpha = 1.0f)
    {
        const auto ink = Sp3ctraHandles::ink(s, alpha);
        const float rad = s.hot() ? r + 1.0f : r;
        drawHalo(g, c, rad, ink.halo);
        if (s == State::Selected)
        {
            const float R = rad + kHaloPad;
            g.setColour(ink.line.withMultipliedAlpha(0.8f));
            g.drawEllipse(c.x - R, c.y - R, 2.0f * R, 2.0f * R, 1.0f);
        }
        g.setColour(ink.fill);
        g.fillEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad);
        g.setColour(ink.line);
        g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad, ink.lineW);
    }

    /** Secondary ring-only handle (curve bend, slope) — smaller, hollow
     *  when idle, filled when hot. */
    inline void drawRing(juce::Graphics& g, juce::Point<float> c, Look s,
                         float r = kRingR, float alpha = 1.0f)
    {
        const auto ink = Sp3ctraHandles::ink(s, alpha);
        const float rad = s.hot() ? r + 1.0f : r;
        drawHalo(g, c, rad, ink.halo);
        if (s.hot())
        {
            g.setColour(ink.fill);
            g.fillEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad);
        }
        g.setColour(ink.line);
        g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad, ink.lineW);
    }

    /** Rounded thumb (fader thumb, line grip). */
    inline void drawThumb(juce::Graphics& g, juce::Rectangle<float> r, Look s,
                          float corner = 2.0f, float alpha = 1.0f)
    {
        const auto ink = Sp3ctraHandles::ink(s, alpha);
        if (ink.hasHalo())
        {
            g.setColour(ink.halo);
            g.fillRoundedRectangle(r.expanded(kHaloPad), corner + 1.0f);
        }
        g.setColour(ink.fill);
        g.fillRoundedRectangle(r, corner);
        g.setColour(ink.line);
        g.drawRoundedRectangle(r, corner, juce::jmin(ink.lineW, 1.6f));
    }

    /** Grabbable line (the LEVELS floor, a threshold…) — thicker and
     *  white-cored while edited. `dashed` keeps the editor's dashed reading. */
    inline void drawGrabLine(juce::Graphics& g, juce::Line<float> l, Look s,
                             bool dashed = false, float alpha = 1.0f)
    {
        const auto ink = Sp3ctraHandles::ink(s, alpha);
        const float w = s.hot() ? kLineHotW : kLineW;
        if (ink.hasHalo())
        {
            g.setColour(ink.halo);
            g.drawLine(l, w + 4.0f);
        }
        g.setColour(ink.line);
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
                            juce::Point<float> dir, Look s, float alpha = 1.0f)
    {
        const auto ink = Sp3ctraHandles::ink(s, alpha);
        juce::Path ch;
        ch.startNewSubPath(-2.5f, -4.0f);
        ch.lineTo         ( 2.5f,  0.0f);
        ch.lineTo         (-2.5f,  4.0f);
        ch.applyTransform(juce::AffineTransform::rotation(std::atan2(dir.y, dir.x))
                              .translated(tip.x, tip.y));
        if (ink.hasHalo())
        {
            g.setColour(ink.halo);
            g.strokePath(ch, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        }
        g.setColour(ink.line);
        g.strokePath(ch, juce::PathStrokeType(s.hot() ? 2.2f : 1.6f));
    }

    /** Link segment handle → grip (the second setting's span). */
    inline void drawLink(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b,
                         Look s, float alpha = 1.0f)
    {
        g.setColour(ink(s, alpha).line.withMultipliedAlpha(s.hot() ? 0.75f : 0.55f));
        g.drawLine(a.x, a.y, b.x, b.y, 1.1f);
    }

    /** Selectable type chip (BELL / LP / HP…): ON = filled accent with dark
     *  text, OFF = outline + text on the ladder (desaturated at rest,
     *  full accent under the pointer). */
    inline void drawChip(juce::Graphics& g, juce::Rectangle<float> r,
                         const juce::String& text, bool on, bool hovered,
                         float corner = 3.0f)
    {
        const auto look = Sp3ctraControls::stateOf(false, hovered, on);
        const auto chipInk = ink(look);
        if (on)
        {
            g.setColour(colour().withAlpha(hovered ? 1.0f : Sp3ctraTheme::kCtlAlphaSel));
            g.fillRoundedRectangle(r, corner);
            g.setColour(juce::Colour(0xff14141c));
        }
        else
        {
            g.setColour(chipInk.line.withMultipliedAlpha(0.75f));
            g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
            g.setColour(chipInk.line);
        }
        g.drawText(text, r.toNearestInt(), juce::Justification::centred, false);
    }

    /** Size of the readout pill of `text` (drawReadoutPill) — for editors
     *  that place the pill themselves (a radial toile keeps it inside its
     *  rim). */
    inline juce::Rectangle<float> readoutSize(const juce::String& text)
    {
        const juce::Font f { juce::FontOptions(Sp3ctraTheme::kFontTiny) };
        return { 0.0f, 0.0f,
                 (float) (juce::GlyphArrangement::getStringWidthInt(f, text) + 10), 16.0f };
    }

    /** The readout itself — dark pill + lime text — at a caller-chosen
     *  place (readoutSize gives the extent). */
    inline void drawReadoutPill(juce::Graphics& g, const juce::String& text,
                                juce::Rectangle<float> pill)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.setColour(core().withAlpha(0.92f));
        g.fillRoundedRectangle(pill, 3.0f);
        g.setColour(colour().withAlpha(0.6f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 3.0f, 1.0f);
        g.setColour(colour());
        g.drawText(text, pill.toNearestInt(), juce::Justification::centred, false);
    }

    /** Live readout next to a handle being edited, kept inside `clip` (the
     *  plot). Anchor = the handle centre; the pill sits above-right of it,
     *  flipping when it would leave the plot. */
    inline void drawReadout(juce::Graphics& g, const juce::String& text,
                            juce::Point<float> anchor, juce::Rectangle<float> clip)
    {
        const auto sz = readoutSize(text);
        const float w = sz.getWidth(), h = sz.getHeight();
        float x = anchor.x + 10.0f;
        float y = anchor.y - h - 6.0f;
        if (x + w > clip.getRight()) x = anchor.x - 10.0f - w;
        if (y < clip.getY())         y = anchor.y + 8.0f;
        x = juce::jmax(clip.getX(), x);
        drawReadoutPill(g, text, { x, y, w, h });
    }
} // namespace Sp3ctraHandles
