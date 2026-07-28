#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UITheme.h"

/**
 * @file Sp3ctraLookAndFeel.h
 * @brief Custom LookAndFeel for Sp3ctra VST.
 *
 * Inherits from juce::LookAndFeel_V4.
 * Overrides drawButtonText so that ALL TextButton labels use the unified
 * Sp3ctraTheme::kFontBtn token instead of JUCE's default scaling.
 *
 * Usage — instantiate once in Sp3ctraAudioProcessorEditor and call:
 *   juce::LookAndFeel::setDefaultLookAndFeel(&laf);
 * Unregister in the destructor:
 *   juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
 */
class Sp3ctraLookAndFeel : public juce::LookAndFeel_V4
{
public:
    Sp3ctraLookAndFeel() = default;
    ~Sp3ctraLookAndFeel() override = default;

    // ─────────────────────────────────────────────────────────────────────────
    // TextButton background — tab-aware: skip background for tab buttons
    // so the parent paint() can render proper tab shapes behind them.
    // Mark buttons with: btn.getProperties().set("isTab", true);
    // ─────────────────────────────────────────────────────────────────────────
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isMouseOverButton,
                              bool isButtonDown) override
    {
        // Tab buttons: parent component paints the tab shape, we only add
        // a subtle hover/press overlay on top.
        if (button.getProperties().contains("isTab"))
        {
            if (isButtonDown)
            {
                g.setColour(juce::Colours::white.withAlpha(0.06f));
                g.fillRect(button.getLocalBounds());
            }
            else if (isMouseOverButton)
            {
                g.setColour(juce::Colours::white.withAlpha(0.03f));
                g.fillRect(button.getLocalBounds());
            }
            // Otherwise: fully transparent — parent's tab shape shows through
            return;
        }

        // Standard buttons — default V4 rendering
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto baseColour = backgroundColour
            .withMultipliedBrightness(isButtonDown ? 0.7f : isMouseOverButton ? 1.1f : 1.0f);
        g.setColour(baseColour);
        g.fillRoundedRectangle(bounds, 3.0f);
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TextButton text — uniform font size, tab-aware font token
    // ─────────────────────────────────────────────────────────────────────────
    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool /*isMouseOverButton*/,
                        bool /*isButtonDown*/) override
    {
        // Brace-init to avoid the "most vexing parse" with juce::Font ctor
        juce::Font font { juce::FontOptions(Sp3ctraTheme::kFontBtn) };
        g.setFont(font);

        g.setColour(button.findColour(
                        button.getToggleState()
                            ? juce::TextButton::textColourOnId
                            : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));

        const int yIndent    = juce::jmin(4, button.proportionOfHeight(0.3f));
        const int cornerSize = juce::jmin(button.getHeight(), button.getWidth()) / 2;
        const int fontH      = static_cast<int>(font.getHeight());
        const int leftInd    = juce::jmin(fontH, 2 + cornerSize /
                                    (button.isConnectedOnLeft()  ? 4 : 2));
        const int rightInd   = juce::jmin(fontH, 2 + cornerSize /
                                    (button.isConnectedOnRight() ? 4 : 2));

        const auto textArea = button.getLocalBounds()
                                    .reduced(leftInd, yIndent)
                                    .withTrimmedRight(rightInd - leftInd);

        g.drawFittedText(button.getButtonText(), textArea,
                         juce::Justification::centred, 2);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ToggleButton — iOS-style sliding switch (track + knob) + label on the right.
    // Replaces JUCE's default tick-box for every boolean/enable toggle.
    // ─────────────────────────────────────────────────────────────────────────
    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool /*shouldDrawButtonAsDown*/) override
    {
        const bool on      = button.getToggleState();
        const bool enabled = button.isEnabled();

        // Switch geometry — vertically centred, left-aligned within the bounds.
        const float h = juce::jmin(20.0f, (float)button.getHeight());
        const float w = h * 1.9f;
        const float y = ((float)button.getHeight() - h) * 0.5f;
        const auto  track  = juce::Rectangle<float>(0.0f, y, w, h);
        const float radius = h * 0.5f;

        // Track
        juce::Colour trackCol = on ? juce::Colour(0xff4fa3e0) : juce::Colour(0xff33373f);
        if (! enabled) trackCol = trackCol.withMultipliedAlpha(0.5f);
        g.setColour(trackCol);
        g.fillRoundedRectangle(track, radius);
        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(track, radius);
        }

        // Sliding knob
        constexpr float pad = 2.0f;
        const float knobD = h - 2.0f * pad;
        const float knobX = on ? (track.getRight() - pad - knobD)
                               : (track.getX() + pad);
        g.setColour(juce::Colour(enabled ? 0xffeaf3fb : 0xff888888));
        g.fillEllipse(knobX, y + pad, knobD, knobD);

        // Label
        const auto text = button.getButtonText();
        if (text.isNotEmpty())
        {
            g.setColour(button.findColour(juce::ToggleButton::textColourId)
                            .withMultipliedAlpha(enabled ? 1.0f : 0.5f));
            g.setFont(juce::Font { juce::FontOptions(Sp3ctraTheme::kFontBtn) });
            const int tx = static_cast<int>(w + 8.0f);
            g.drawText(text, tx, 0, button.getWidth() - tx, button.getHeight(),
                       juce::Justification::centredLeft, true);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Linear Slider — explicit filled/unfilled colours so the "value" portion
    // (left of thumb) is always brighter than the unfilled portion (right).
    // ─────────────────────────────────────────────────────────────────────────
    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                              0, 0, style, slider);
            return;
        }

        const float trackY  = (float)y + (float)height * 0.5f;
        const float trackH  = 4.0f;
        const float trackX  = (float)x;
        const float trackW  = (float)width;
        const float thumbX  = sliderPos;
        const bool  en      = slider.isEnabled();

        // Unfilled portion (right of thumb) — very dark
        g.setColour(juce::Colour(0xff1a1f2a));
        g.fillRoundedRectangle(trackX, trackY - trackH * 0.5f, trackW, trackH, trackH * 0.5f);

        // Filled portion (left of thumb) — accent blue (muted when disabled)
        const float filledW = thumbX - trackX;
        if (filledW > 0.f)
        {
            g.setColour(juce::Colour(en ? 0xff4fa3e0 : 0xff363f4d));
            g.fillRoundedRectangle(trackX, trackY - trackH * 0.5f, filledW, trackH, trackH * 0.5f);
        }

        // Thumb — circle (muted when disabled)
        constexpr float thumbR = 7.0f;
        g.setColour(juce::Colour(en ? 0xffa0c4e8 : 0xff555a62));
        g.fillEllipse(thumbX - thumbR, trackY - thumbR, thumbR * 2.f, thumbR * 2.f);

        if (slider.isMouseOverOrDragging())
        {
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.fillEllipse(thumbX - thumbR - 2.f, trackY - thumbR - 2.f,
                          (thumbR + 2.f) * 2.f, (thumbR + 2.f) * 2.f);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Rotary Slider — dark knob body with an accent-blue value arc + pointer.
    // Visual language matches drawLinearSlider: track #1a1f2a, accent #4fa3e0,
    // pointer #a0c4e8. Used by the audio-parameter knob grids.
    // ─────────────────────────────────────────────────────────────────────────
    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        constexpr float arcW = 3.5f;

        const juce::PathStrokeType arcStroke(
            arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        // Unfilled arc (full sweep) — very dark
        juce::Path bgArc;
        bgArc.addCentredArc(cx, cy, radius, radius, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff1a1f2a));
        g.strokePath(bgArc, arcStroke);

        // Filled arc (start → value) — accent blue
        if (slider.isEnabled() && angle > rotaryStartAngle)
        {
            juce::Path valArc;
            valArc.addCentredArc(cx, cy, radius, radius, 0.0f,
                                 rotaryStartAngle, angle, true);
            g.setColour(juce::Colour(0xff4fa3e0));
            g.strokePath(valArc, arcStroke);
        }

        // Knob body
        const float knobR = radius - 5.0f;
        g.setColour(juce::Colour(0xff22272f));
        g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.f, knobR * 2.f);
        g.setColour(juce::Colour(0xff33373f));
        g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.f, knobR * 2.f, 1.0f);

        // Hover halo
        if (slider.isMouseOverOrDragging())
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.f, knobR * 2.f);
        }

        // Pointer line — from inner radius outward, rotated to the value angle
        juce::Path pointer;
        pointer.startNewSubPath(0.0f, -knobR * 0.35f);
        pointer.lineTo(0.0f, -knobR * 0.92f);
        g.setColour(juce::Colour(slider.isEnabled() ? 0xffa0c4e8 : 0xff555a62));
        g.strokePath(pointer,
                     juce::PathStrokeType(2.5f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded),
                     juce::AffineTransform::rotation(angle).translated(cx, cy));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ComboBox — dark background + small white filled-triangle arrow
    // ─────────────────────────────────────────────────────────────────────────

    /** Uniform font for combo box selected text. */
    juce::Font getComboBoxFont(juce::ComboBox&) override
    {
        return juce::Font { juce::FontOptions(Sp3ctraTheme::kFontSettings) };
    }

    /**
     * Draws the combo box background, border, and a compact white
     * downward-pointing triangle arrow — replaces JUCE's wide Unicode chevron.
     *
     * @param buttonX/buttonW   Bounds of the arrow-button area (right side).
     */
    void drawComboBox(juce::Graphics& g,
                      int width, int height,
                      bool isButtonDown,
                      int buttonX, int /*buttonY*/, int buttonW, int /*buttonH*/,
                      juce::ComboBox& box) override
    {
        const juce::Rectangle<float> boxR(0.f, 0.f, (float)width, (float)height);
        constexpr float radius = 3.0f;

        // Background
        g.setColour(juce::Colour(Sp3ctraTheme::kColBtnBg)
                        .brighter(isButtonDown ? 0.12f : 0.0f));
        g.fillRoundedRectangle(boxR, radius);

        // Border
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.drawRoundedRectangle(boxR.reduced(0.5f), radius, 1.0f);

        // Small filled downward triangle, centred in the arrow-button zone
        if (box.isEnabled())
        {
            constexpr float aw = 6.0f; // arrow base width
            constexpr float ah = 4.0f; // arrow height
            const float ax = (float)buttonX + (float)buttonW * 0.5f - aw * 0.5f;
            const float ay = (float)height  * 0.5f - ah * 0.5f;

            juce::Path tri;
            tri.addTriangle(ax,          ay,
                            ax + aw,     ay,
                            ax + aw * 0.5f, ay + ah);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.fillPath(tri);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PopupMenu — dark theme + kFontSmall (11 px, non-bold)
    // ─────────────────────────────────────────────────────────────────────────

    /** All menu item text uses the same light weight as auxiliary labels. */
    juce::Font getPopupMenuFont() override
    {
        return juce::Font { juce::FontOptions(Sp3ctraTheme::kFontSmall) };
    }

    /** The default ideal width knows nothing about the tick gutter drawn by
        drawPopupMenuItem below — short labels ("8") ended up ellipsized when
        ticked. Reserve the gutter here. */
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                   int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize(
            text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
        if (! isSeparator)
            idealWidth += static_cast<int>(Sp3ctraTheme::kFontSmall) + 6;
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        // fillAll first: covers the OS-level square window corners so they
        // never show as white, even if the rounded rect leaves pixel gaps.
        g.fillAll(juce::Colour(Sp3ctraTheme::kColBg));
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.drawRoundedRectangle(0.5f, 0.5f, (float)width - 1.f, (float)height - 1.f, 2.f, 1.f);
    }

    void drawPopupMenuSectionHeader(juce::Graphics& g,
                                    const juce::Rectangle<int>& area,
                                    const juce::String& sectionName) override
    {
        g.setColour(juce::Colour(Sp3ctraTheme::kColPanelBg));
        g.fillRect(area);
        g.setFont(juce::Font { juce::FontOptions(Sp3ctraTheme::kFontSmall) }.boldened());
        g.setColour(juce::Colour(0xff66cc88)); // accent green
        g.drawText(sectionName, area.reduced(8, 0), juce::Justification::centredLeft, true);
    }

    void drawPopupMenuItem(juce::Graphics& g,
                           const juce::Rectangle<int>& area,
                           bool isSeparator,
                           bool isActive,
                           bool isHighlighted,
                           bool isTicked,
                           bool hasSubMenu,
                           const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* /*icon*/,
                           const juce::Colour* /*textColour*/) override
    {
        if (isSeparator)
        {
            const auto r = area.reduced(5, 0).withHeight(1).withY(area.getCentreY());
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
            g.fillRect(r);
            return;
        }

        const auto bgCol = isHighlighted
            ? juce::Colour(0xff2a3a2a)   // subtle green tint on hover
            : juce::Colour(Sp3ctraTheme::kColBg);
        g.setColour(bgCol);
        g.fillRect(area);

        const auto textCol = isActive
            ? juce::Colour(Sp3ctraTheme::kColText)
            : juce::Colour(Sp3ctraTheme::kColTextMuted);
        g.setColour(textCol);
        g.setFont(juce::Font { juce::FontOptions(Sp3ctraTheme::kFontSmall) });

        // Tick mark
        if (isTicked)
        {
            const float tickSz = Sp3ctraTheme::kFontSmall;
            g.setColour(juce::Colour(0xff66cc88));
            g.drawText(juce::CharPointer_UTF8("\xe2\x9c\x93"), // ✓
                       area.withWidth(static_cast<int>(tickSz) + 4),
                       juce::Justification::centred, false);
        }

        const int leftPad  = isTicked ? static_cast<int>(Sp3ctraTheme::kFontSmall) + 6 : 8;
        const int rightPad = hasSubMenu ? 20 : (shortcutKeyText.isNotEmpty() ? 80 : 8);
        g.setColour(textCol);
        g.drawText(text, area.withTrimmedLeft(leftPad).withTrimmedRight(rightPad),
                   juce::Justification::centredLeft, true);

        if (shortcutKeyText.isNotEmpty())
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColTextMuted));
            g.drawText(shortcutKeyText, area.reduced(0, 0).withTrimmedLeft(area.getWidth() - 75),
                       juce::Justification::centredRight, true);
        }

        if (hasSubMenu)
        {
            const float arrowH = (float)area.getHeight() * 0.4f;
            juce::Path arrow;
            arrow.addTriangle(0.f, 0.f, arrowH * 0.5f, arrowH * 0.5f, 0.f, arrowH);
            g.setColour(textCol);
            g.fillPath(arrow, juce::AffineTransform::translation(
                           (float)(area.getRight() - 12),
                           (float)area.getCentreY() - arrowH * 0.5f));
        }
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraLookAndFeel)
};
