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

    /** Paints the standard SETUP-face header (accent title + underline). */
    inline void paintHeader(juce::Graphics& g, const juce::Component& c,
                            const juce::String& title, juce::Colour accent)
    {
        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(title,
                   Sp3ctraTheme::kHPad, 0,
                   c.getWidth() - 2 * Sp3ctraTheme::kHPad, kHeaderH - 4,
                   juce::Justification::centredLeft, true);

        g.setColour(accent.withAlpha(0.35f));
        g.fillRect(Sp3ctraTheme::kHPad, kHeaderH - 3,
                   c.getWidth() - 2 * Sp3ctraTheme::kHPad, 1);
    }
} // namespace SetupUI
