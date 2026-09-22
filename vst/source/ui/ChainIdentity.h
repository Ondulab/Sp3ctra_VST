/**
 * @file ChainIdentity.h
 * @brief How a CHAIN is named and coloured everywhere it appears — the rack
 *        header, the PLAY/SETUP bar badge and the MIDI parameter labels
 *        (ParamIdentity.h) all draw the same numbered pastille + name.
 *
 * The NUMBER (1-based rack index) is the identity: the header colour cycles
 * every three chains and the name is a free user label defaulting to
 * "CHAIN". One recipe here; no component restates the pastille or the cycle.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ChainIdentity
{
    /** Header colour of chain `chainIdx` (0-based) — amber / green / grey. */
    inline juce::Colour colour(int chainIdx) noexcept
    {
        switch (juce::jmax(0, chainIdx) % 3)
        {
            case 0:  return juce::Colour(0xffe0b84a);   // amber
            case 1:  return juce::Colour(0xff4ae0a0);   // green
            default: return juce::Colour(0xffc0c4cc);   // grey
        }
    }

    /** The label shown next to the number: the user name, or "CHAIN". */
    inline juce::String label(const juce::String& userName)
    {
        return userName.isNotEmpty() ? userName : juce::String("CHAIN");
    }

    /** Pastille diameter of the rack header / face bar. Compact rows
     *  (MIDI MAP) shrink it — the number font follows (drawPastille). */
    constexpr int   kPastilleD   = 16;
    /** Ink of the number inside the pastille — the rack gutter dark. */
    constexpr juce::uint32 kColInk = 0xff121218;

    /** The numbered pastille: a filled disc in the chain colour, the 1-based
     *  number punched in dark. `alpha` fades the whole badge (heat-driven
     *  readouts). */
    inline void drawPastille(juce::Graphics& g, juce::Rectangle<float> dot,
                             int number, juce::Colour chainColour,
                             float alpha = 1.0f)
    {
        g.setColour(chainColour.withMultipliedAlpha(alpha));
        g.fillEllipse(dot);

        // Centre the number's INK on the disc, not its font box: drawText
        // centres the ascent+descent line box, which a lining digit (no
        // descender, side bearings) fills unevenly, and its integer-snapped
        // text rectangle adds up to half a pixel of drift against the
        // anti-aliased disc. The glyph outline gives the exact ink extent.
        // 11 px at the 16 px reference, scaled with the disc.
        juce::GlyphArrangement ga;
        ga.addLineOfText(juce::Font(juce::FontOptions(
                             juce::jmax(8.0f, dot.getHeight() * (11.0f / 16.0f)))).boldened(),
                         juce::String(number), 0.0f, 0.0f);
        juce::Path ink;
        ga.createPath(ink);
        const auto ib = ink.getBounds();
        ga.moveRangeOfGlyphs(0, -1, dot.getCentreX() - ib.getCentreX(),
                                    dot.getCentreY() - ib.getCentreY());
        g.setColour(juce::Colour(kColInk).withAlpha(alpha));
        ga.draw(g);
    }
} // namespace ChainIdentity
