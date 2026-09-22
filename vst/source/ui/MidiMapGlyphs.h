/**
 * @file MidiMapGlyphs.h
 * @brief The two glyphs of a MIDI mapping row, shared by the MIDI MAP panel
 *        and the MIDI CURVE window so they read the same everywhere:
 *
 *   F  — the per-mapping MIDI-FOLLOW chip (lit = a controller move
 *        navigates to the module page; off = a busy control stays quiet).
 *   ⚙  — the gear that opens the mapping's transfer-law window; lit in
 *        the mapping amber when the law is not neutral.
 *
 * The amber is the "mapped" badge colour (MidiLearnAttachment's dot): the
 * one hue that already MEANS "this control has a MIDI mapping".
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"

namespace MidiMapGlyphs
{
    constexpr juce::uint32 kAccent = 0xffe0a24a;   ///< the mapped-badge amber

    /** The F chip: filled amber with a dark letter when ON, a thin outline
     *  with a dim letter when OFF (the type-chip idiom of the EQ header). */
    inline void drawFollowChip(juce::Graphics& g, juce::Rectangle<float> r,
                               bool on, bool over)
    {
        const juce::Colour amber(kAccent);
        if (on)
        {
            g.setColour(amber.withAlpha(over ? 1.0f : 0.88f));
            g.fillRoundedRectangle(r, 3.0f);
            g.setColour(juce::Colour(0xff14141c));
        }
        else
        {
            g.setColour(juce::Colours::white.withAlpha(over ? 0.6f : 0.30f));
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(over ? 0.8f : 0.38f));
        }
        g.setFont(juce::Font(juce::FontOptions(juce::jmin(11.0f, r.getHeight() - 3.0f))).boldened());
        g.drawText("F", r.toNearestInt(), juce::Justification::centred, false);
    }

    /** Eight-tooth gear, hub punched out, centred on `c` with outer radius
     *  `R`. `colour` carries the state (amber lit / dim white). */
    inline void drawGear(juce::Graphics& g, juce::Point<float> c, float R,
                         juce::Colour colour)
    {
        constexpr int   teeth = 8;
        const float     rIn   = R * 0.70f;
        const float     hub   = R * 0.30f;
        const float     step  = juce::MathConstants<float>::twoPi / (float) teeth;
        const float     w     = step * 0.24f;   // half tooth width (angle)

        juce::Path p;
        for (int i = 0; i < teeth; ++i)
        {
            const float a = step * (float) i;
            auto pt = [&](float ang, float rad)
            { return juce::Point<float>(c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad); };
            const auto p0 = pt(a - w,        rIn);
            const auto p1 = pt(a - w * 0.6f, R);
            const auto p2 = pt(a + w * 0.6f, R);
            const auto p3 = pt(a + w,        rIn);
            if (i == 0) p.startNewSubPath(p0); else p.lineTo(p0);
            p.lineTo(p1); p.lineTo(p2); p.lineTo(p3);
            // arc of the body to the next tooth
            const float a2 = a + step - w;
            p.addCentredArc(c.x, c.y, rIn, rIn, 0.0f,
                            a + w + juce::MathConstants<float>::halfPi,
                            a2    + juce::MathConstants<float>::halfPi, false);
        }
        p.closeSubPath();
        p.addEllipse(c.x - hub, c.y - hub, 2.0f * hub, 2.0f * hub);
        p.setUsingNonZeroWinding(false);   // the hub punches a hole
        g.setColour(colour);
        g.fillPath(p);
    }
}
