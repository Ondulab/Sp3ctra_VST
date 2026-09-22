/**
 * @file ModulePowerButton.h
 * @brief THE module power switch — one look everywhere (zone-3 face row,
 *        SP3CTRA page transport). A real toggle Button so an APVTS
 *        ButtonAttachment keeps it in sync with the rack LED + automation.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ============================================================================
// ModulePowerButton — power switch for the selected module, sitting at the
// right end of the zone-3 PLAY|SETUP header row. A real toggle Button (so an
// APVTS ButtonAttachment keeps it in sync with the rack LED + host automation).
// Draws the universal power glyph, lit in the block's accent colour when on.
// ============================================================================
class ModulePowerButton : public juce::Button
{
public:
    ModulePowerButton() : juce::Button("modulePower")
    {
        setClickingTogglesState(true);
        setTooltip("Enable / disable this module");
    }

    void setAccent(juce::Colour c) { if (accent != c) { accent = c; repaint(); } }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b   = getLocalBounds().toFloat().reduced(2.f);
        const bool on  = getToggleState();

        const juce::Colour bg(0xff222836);
        g.setColour(isButtonDown ? bg.brighter(0.30f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 3.f);
        g.setColour(on ? accent.withAlpha(0.90f) : juce::Colour(0xff3a4250));
        g.drawRoundedRectangle(b, 3.f, on ? 1.4f : 1.f);

        // Power glyph: open ring (gap at top) + vertical stem through the gap.
        const float cx = b.getCentreX();
        const float cy = b.getCentreY() + 0.5f;
        const float r  = juce::jmin(b.getWidth(), b.getHeight()) * 0.26f;
        g.setColour(on ? accent.brighter(0.20f) : juce::Colour(0xff6b7280));

        juce::Path ring;
        ring.addCentredArc(cx, cy, r, r, 0.f,
                           juce::MathConstants<float>::pi * 0.30f,
                           juce::MathConstants<float>::pi * 1.70f, true);
        g.strokePath(ring, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.drawLine(cx, cy - r - 1.5f, cx, cy - 0.5f, 1.6f);
    }

private:
    juce::Colour accent { juce::Colour(0xff4fa3e0) };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulePowerButton)
};

