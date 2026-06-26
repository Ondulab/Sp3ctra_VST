/**
 * @file AudioPanelWidgets.h
 * @brief Shared layout tokens + paint helpers for the badged "audio panel" look.
 *
 * These were previously private to EngineAudioPanels.cpp.  They are promoted to
 * a shared header so the LUXSTRAL image-pipeline page (LuxStralTabComponent) can
 * render the SAME section frames, badges and rotary-knob grids as the audio
 * panels — keeping a single visual language across the whole module.
 *
 *   AudioPanelLayout — pure layout constants (knob grid, toggle strip, sections).
 *   AudioPanelUI     — stateless paint/place helpers (badge, section bg, knobs).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include "../UITheme.h"
#include "EnvelopeEditorComponent.h"

//==============================================================================
/**
 * Layout tokens for the rotary-knob grids of the audio panels.
 * Sections are stacked full-width; continuous params are knobs laid out in a
 * fixed-column grid, on/off params live in a toggle strip under the badge.
 */
namespace AudioPanelLayout
{
    constexpr int kKnobCols    = 4;                       ///< knobs per grid row
    constexpr int kKnobArea    = 44;                      ///< rotary draw square
    constexpr int kKnobValH    = 14;                      ///< value text-box height
    constexpr int kKnobLblH    = 13;                      ///< name label height
    constexpr int kKnobCellH   = kKnobArea + kKnobValH + kKnobLblH; ///< 71
    constexpr int kKnobGapX    = 6;
    constexpr int kKnobGapY    = 6;
    constexpr int kKnobRowStep = kKnobCellH + kKnobGapY;  ///< 77

    constexpr int kToggleStripH = Sp3ctraTheme::kControlH; ///< 22
    constexpr int kToggleGap    = 6;                       ///< below toggle strip
    constexpr int kToggleW      = 160;                     ///< single toggle width
    constexpr int kSecGapV      = 10;                      ///< between stacked sections
    constexpr int kBottomPad    = 8;

    // Envelope-editor blocks (audio ADSR rendered as draggable curve, not knobs).
    constexpr int kEnvCaptionH  = 13;                      ///< caption strip above an editor
    constexpr int kEnvGap       = 10;                      ///< below the editor row
    constexpr int kEnvH         = EnvelopeEditorComponent::kPreferredH; ///< 124

    /// Grid rows needed to host n knobs.
    constexpr int rows(int n)  { return (n + kKnobCols - 1) / kKnobCols; }
    /// Pixel height of an n-knob grid (no trailing row gap).
    constexpr int gridH(int n) { return rows(n) * kKnobRowStep - kKnobGapY; }
    /// Full height of one section: badge + gap + optional toggle strip + grid.
    constexpr int sectionH(int nKnobs, bool hasToggles)
    {
        return Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
             + (hasToggles ? kToggleStripH + kToggleGap : 0)
             + gridH(nKnobs);
    }
}

//==============================================================================
/**
 * Stateless paint / placement helpers — the shared "audio panel" visual language.
 * All geometry comes in as explicit ints so callers stay in control of layout.
 */
namespace AudioPanelUI
{
    /** Rotary knob: accent-arc cadran + value text-box below (Sp3ctraLookAndFeel). */
    inline void initKnob(juce::Slider& s, const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, AudioPanelLayout::kKnobValH);
        s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        if (suffix) s.setTextValueSuffix(suffix);
    }

    /** Cell rectangle for knob #idx in a grid starting at (gx, gy) of width gw. */
    inline juce::Rectangle<int> knobCell(int gx, int gw, int gy, int idx)
    {
        using namespace AudioPanelLayout;
        const int cellW = (gw - (kKnobCols - 1) * kKnobGapX) / kKnobCols;
        const int col = idx % kKnobCols;
        const int row = idx / kKnobCols;
        return { gx + col * (cellW + kKnobGapX), gy + row * kKnobRowStep, cellW, kKnobCellH };
    }

    /** Place a knob slider (rotary + value box) into its grid cell. */
    inline void placeKnob(juce::Slider& s, int gx, int gw, int gy, int idx)
    {
        using namespace AudioPanelLayout;
        const auto c = knobCell(gx, gw, gy, idx);
        s.setBounds(c.getX(), c.getY(), c.getWidth(), kKnobArea + kKnobValH);
    }

    /** Draw the name label under knob #idx (the cell's bottom strip). */
    inline void drawKnobLabel(juce::Graphics& g, int gx, int gw, int gy, int idx,
                              const char* text, juce::uint32 colour = 0xffb8c4d0)
    {
        using namespace AudioPanelLayout;
        const auto c = knobCell(gx, gw, gy, idx);
        g.setColour(juce::Colour(colour));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText(text,
                   juce::Rectangle<int>(c.getX(), c.getY() + kKnobArea + kKnobValH,
                                        c.getWidth(), kKnobLblH),
                   juce::Justification::centred, true);
    }

    /** Subtle rounded backdrop grouping one stacked section. */
    inline void drawSectionBg(juce::Graphics& g, int x, int y, int w, int h)
    {
        const auto r = juce::Rectangle<int>(x, y, w, h).toFloat();
        g.setColour(juce::Colour(0xff131320));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawRoundedRectangle(r, 4.f, 1.f);
    }

    /** Coloured section badge at (x, y). */
    inline void drawBadge(juce::Graphics& g, int x, int y, int w,
                          juce::uint32 bg, juce::uint32 fg, const char* text)
    {
        g.setColour(juce::Colour(bg));
        g.fillRoundedRectangle(juce::Rectangle<int>(x, y, w, Sp3ctraTheme::kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(fg));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(text, juce::Rectangle<int>(x + 8, y, w - 16, Sp3ctraTheme::kSectionH),
                   juce::Justification::centredLeft, true);
    }

    /** Small accented caption above an envelope editor. */
    inline void drawEnvCaption(juce::Graphics& g, int x, int y, int w,
                               juce::uint32 fg, const char* text)
    {
        g.setColour(juce::Colour(fg).withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.drawText(text, x + 2, y, w - 4, AudioPanelLayout::kEnvCaptionH,
                   juce::Justification::centredLeft, true);
    }
}
