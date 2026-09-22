/**
 * @file SetupHeader.h
 * @brief Shared painted header for the zone-3 SETUP faces (M5).
 *
 * Every per-block setup panel paints the same slim header strip:
 * the block name in its identity (accent) colour + a thin underline.
 * Keeping this in one place guarantees a consistent SETUP-face look.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../UITheme.h"

namespace SetupUI
{
    /** Height of the painted SETUP-face header strip. */
    constexpr int kHeaderH = 26;

    /** Paints the standard SETUP-face header (accent title + underline).
        @param ruleWidth  width of the underline — 0 (default) spans the whole
                          panel; a panel whose content stops short of the window
                          passes its content width so the rule matches it. */
    inline void paintHeader(juce::Graphics& g, const juce::Component& c,
                            const juce::String& title, juce::Colour accent,
                            int ruleWidth = 0)
    {
        const int fullW = c.getWidth() - 2 * Sp3ctraTheme::kHPad;
        const int ruleW = ruleWidth > 0 ? juce::jmin(ruleWidth, fullW) : fullW;

        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(title,
                   Sp3ctraTheme::kHPad, 0, fullW, kHeaderH - 4,
                   juce::Justification::centredLeft, true);

        g.setColour(accent.withAlpha(0.35f));
        g.fillRect(Sp3ctraTheme::kHPad, kHeaderH - 3, ruleW, 1);
    }
} // namespace SetupUI
