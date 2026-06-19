/**
 * @file PaletteRailComponent.h
 * @brief Far-left module palette rail (M4 four-zone shell) — static stub.
 *
 * 36 px vertical rail with the three block categories (SRC / FX / OUT).
 * Clicking does nothing yet: drag & drop block insertion arrives with M6.
 * Kept visually consistent with the Sp3ctraTheme dark tokens.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"

class PaletteRailComponent : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    static constexpr int kRailW = 36;

    PaletteRailComponent()
    {
        setTooltip("Module palette - drag & drop coming with M6");
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff14141c));

        struct Cat { const char* name; juce::uint32 col; };
        static constexpr Cat cats[] = {
            { "SRC", 0xff68788f },   // sources  — neutral grey-blue
            { "FX",  0xffe06bb8 },   // effects  — pink (Pitch identity)
            { "OUT", 0xff4fa3e0 },   // outputs  — blue (engine identity)
        };

        const int chipW = getWidth() - 8;
        int y = 10;

        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());

        for (const auto& cat : cats)
        {
            const juce::String name(cat.name);
            const int letters = name.length();
            const int chipH   = 14 * letters + 10;
            const juce::Colour col(cat.col);

            const juce::Rectangle<int> chip(4, y, chipW, chipH);
            g.setColour(col.withAlpha(0.10f));
            g.fillRoundedRectangle(chip.toFloat(), 4.f);
            g.setColour(col.withAlpha(0.35f));
            g.drawRoundedRectangle(chip.toFloat(), 4.f, 1.f);

            // Letters stacked vertically
            g.setColour(col.brighter(0.2f));
            for (int i = 0; i < letters; ++i)
                g.drawText(name.substring(i, i + 1),
                           chip.getX(), chip.getY() + 5 + i * 14, chip.getWidth(), 14,
                           juce::Justification::centred, false);

            y += chipH + 10;
        }

        // Right border
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.fillRect(getWidth() - 1, 0, 1, getHeight());
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PaletteRailComponent)
};
