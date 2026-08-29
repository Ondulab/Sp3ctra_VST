/**
 * @file ModuleEditorChrome.h
 * @brief The DISPLAY chrome every module editor and module page shares —
 *        graphic frame, frame caption + readout, box labels, the
 *        "--- NAME ---" section caption, and the box-row metrics. One file:
 *        the skeleton of every FX page changes here.
 *
 * Standard module editor (DcBlock / LEVELS / ECHO / EQ / ADSR / FILTER …):
 *
 *   ┌ frame (kColFrameBg, accent 25 % outline, r = kFrameR) ───────────┐
 *   │ CAPTION                                              readout      │
 *   │    plot = plotOf(frame)  — the graph + its handles                │
 *   │                            (handles: Sp3ctraHandles, lime)        │
 *   └───────────────────────────────────────────────────────────────────┘
 *     kRowGap
 *     label      label      label        kLabelH  (kFontTiny, accent 60 %)
 *     [ box  ]   [ box  ]   [ box  ]     kBoxH    (Sp3ctraBarSlider), kBoxGap
 *
 * Rules (editor-display guideline #2): the frame holds ONLY the picture and
 * its handles; every numeric box lives in its own row BELOW the frame. A
 * page stacks its editors with kEditorGap, appends any extra control rows
 * (combos, toggles — same label-above idiom), and closes with a section
 * caption "--- NAME ---" (drawSectionCaption).
 *
 * Colour split: everything here is painted in the MODULE colour (display);
 * the controls themselves (bars, nodes, chips, toggles, combos) take
 * Sp3ctraTheme::kColHandle — see Sp3ctraHandles.h.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <initializer_list>
#include "../UITheme.h"

namespace ModuleChrome
{
    // ── Metrics ────────────────────────────────────────────────────────────
    constexpr int   kBoxH            = 18;   ///< numeric box (Sp3ctraBarSlider) height
    constexpr int   kLabelH          = 12;   ///< label strip above a box row
    constexpr int   kRowGap          = 6;    ///< frame bottom → label strip
    constexpr int   kBoxGap          = 8;    ///< between the boxes of a row
    constexpr int   kBoxRowH         = kLabelH + kBoxH;      ///< 30 — one label+box row
    constexpr int   kBelowFrameH     = kRowGap + kBoxRowH;   ///< 36 — a frame's box row, gap included
    constexpr int   kEditorGap       = 4;    ///< between stacked editors on a page
    constexpr int   kPagePad         = 8;    ///< page side padding
    constexpr int   kPageTop         = 4;    ///< page top padding (first editor y)
    constexpr int   kSectionCaptionH = 22;   ///< "--- NAME ---" band (4 + 12 text + 6)
    constexpr int   kFrameInset      = 6;    ///< frame → graph rect
    constexpr float kFrameR          = 4.0f;
    constexpr float kPlotInsetX      = 8.0f; ///< graph rect → plot (sides / bottom)
    constexpr float kPlotInsetY      = 7.0f;
    constexpr int   kCaptionH        = 14;   ///< caption strip inside the frame top
    /// graph rect → plot TOP: clears the caption strip (frame.y + 2 + kCaptionH
    /// + 2) so a curve at full scale never runs into the title.
    constexpr float kPlotInsetTop    = 12.0f;

    /** Height a page needs for `editorsH` of stacked editors + the closing
     *  section caption (+ optional extra control rows, kBoxRowH each). */
    constexpr int pageHeight(int editorsH, int extraRows = 0) noexcept
    {
        return kPageTop + editorsH + kSectionCaptionH
             + extraRows * (kBoxRowH + kRowGap) + kPagePad;
    }

    /** The graph rect of a frame (what the editors call graphRect_). */
    inline juce::Rectangle<float> graphOf(juce::Rectangle<float> frame) noexcept
    {
        return frame.reduced((float) kFrameInset);
    }

    /** The plot of a frame (curve + handles) — inset on every side, and
     *  starting BELOW the caption strip. */
    inline juce::Rectangle<float> plotOf(juce::Rectangle<float> frame) noexcept
    {
        return graphOf(frame).reduced(kPlotInsetX, kPlotInsetY)
                             .withTrimmedTop(kPlotInsetTop - kPlotInsetY);
    }

    // ── Painters (module colour = `accent`) ────────────────────────────────

    /** The graphic window. */
    inline void drawFrame(juce::Graphics& g, juce::Rectangle<float> frame,
                          juce::Colour accent)
    {
        g.setColour(juce::Colour(Sp3ctraTheme::kColFrameBg));
        g.fillRoundedRectangle(frame.reduced(0.5f), kFrameR);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(frame.reduced(0.5f), kFrameR, 1.0f);
    }

    /** Caption top-left inside the frame ("GAIN CURVE", "IN - OUT", "FILTER"). */
    inline void drawCaption(juce::Graphics& g, juce::Rectangle<float> frame,
                            juce::Colour accent, const juce::String& text)
    {
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.setColour(accent.withAlpha(0.75f));
        g.drawText(text, (int) frame.getX() + 8, (int) frame.getY() + 2,
                   (int) frame.getWidth() - 16, kCaptionH,
                   juce::Justification::centredLeft, false);
    }

    /** Readout / hint top-right inside the frame (live value of the selected
     *  handle, "click to add …"). Muted text: accent at `alpha`. */
    inline void drawReadout(juce::Graphics& g, juce::Rectangle<float> frame,
                            juce::Colour accent, const juce::String& text,
                            float alpha = 0.6f)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.setColour(accent.withAlpha(alpha));
        const int w = juce::jmin(260, (int) frame.getWidth() - 16);
        g.drawText(text, (int) frame.getRight() - 8 - w, (int) frame.getY() + 2,
                   w, kCaptionH, juce::Justification::centredRight, false);
    }

    /** Label centred above a box (or any control laid out by layoutBoxRow). */
    inline void drawBoxLabel(juce::Graphics& g, const juce::Component& box,
                             juce::Colour accent, const juce::String& text)
    {
        const auto bb = box.getBounds();
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.setColour(accent.withAlpha(0.6f));
        g.drawText(text, bb.getX(), bb.getY() - kLabelH, bb.getWidth(), kLabelH,
                   juce::Justification::centred, false);
    }

    /** Lay one row (kBoxRowH tall = label strip + boxes) out over N controls,
     *  kBoxGap apart. Works for bars, combos and toggles alike. */
    inline void layoutBoxRow(juce::Rectangle<int> row,
                             std::initializer_list<juce::Component*> boxes)
    {
        row.removeFromTop(kLabelH);
        const int n  = (int) boxes.size();
        const int bw = (row.getWidth() - (n - 1) * kBoxGap) / juce::jmax(1, n);
        int i = 0;
        for (auto* b : boxes)
        {
            if (b != nullptr)
                b->setBounds(row.getX() + i * (bw + kBoxGap), row.getY(), bw, kBoxH);
            ++i;
        }
    }

    /** "--- NAME ---" band closing a page section. `y` = top of the band
     *  (the bottom of the last editor), `width` = page width. */
    inline void drawSectionCaption(juce::Graphics& g, int y, int width,
                                   juce::Colour accent, const juce::String& name)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("--- " + name + " ---", kPagePad, y + 4, width - 2 * kPagePad, 12,
                   juce::Justification::centred, false);
    }
} // namespace ModuleChrome
