#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UITheme.h"
#include "ui/Sp3ctraControls.h"

#include <cmath>

/**
 * @file Sp3ctraLookAndFeel.h
 * @brief Custom LookAndFeel for Sp3ctra VST.
 *
 * Inherits from juce::LookAndFeel_V4.
 * Overrides drawButtonText so that ALL TextButton labels use the unified
 * Sp3ctraTheme::kFontBtn token instead of JUCE's default scaling.
 *
 * Every widget branch below reads the SAME interaction ladder as the
 * graphic-editor handles — ui/Sp3ctraControls.h:
 *
 *   Idle      the accent DESATURATED (Sp3ctraControls::restOf): a panel at
 *             rest is calm, nothing shouts for attention;
 *   Hover     full accent — "you can grab this";
 *   Edit      full accent + WHITE cursor / ring — being changed right now,
 *             by the mouse OR by a MIDI CC / automation, which keeps the
 *             control lit for Sp3ctraTheme::kCtlGlowMs (the remote heat,
 *             stamped by Sp3ctraBarSlider and MidiLearnAttachment).
 *
 * No branch here restates an alpha or a width: it resolves
 * Sp3ctraControls::inkOf() / fillAlpha() and paints. Restyle every control
 * of the interface from those tokens, not from this file.
 *
 * Usage — instantiate once in Sp3ctraAudioProcessorEditor and call:
 *   juce::LookAndFeel::setDefaultLookAndFeel(&laf);
 * Unregister in the destructor:
 *   juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
 */
class Sp3ctraLookAndFeel : public juce::LookAndFeel_V4
{
public:
    Sp3ctraLookAndFeel()
    {
        // Toggle accent = THE control colour (Sp3ctraTheme::kColHandle): a
        // toggle is something you touch, so it takes the handle hue on every
        // page, never the module colour (drawToggleButton reads tickColourId
        // with parent inheritance — a host may still override it locally).
        setColour(juce::ToggleButton::tickColourId, juce::Colour(Sp3ctraTheme::kColHandle));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // The ladder, read for a widget — the ONLY place a branch below asks
    // "what state is this control in?". Sliders publish an accent through
    // their colour ids; the ladder owns every alpha, so a colour id set with
    // a baked-in alpha (legacy call sites) reads exactly the same.
    // ─────────────────────────────────────────────────────────────────────────

    /** Live reading of a slider: mouse state + remote-edit heat (MIDI CC,
     *  automation — stamped by Sp3ctraBarSlider / MidiLearnAttachment). */
    static Sp3ctraControls::Look lookOfSlider(juce::Slider& s)
    {
        const bool en   = s.isEnabled();
        const bool drag = en && s.isMouseButtonDown(true);
        const bool over = en && ! drag && s.isMouseOverOrDragging(true);
        return Sp3ctraControls::stateOf(drag, over, false, Sp3ctraControls::heatOf(s));
    }

    /** The chrome hue of a slider (outline / cursor / thumb ring). */
    static juce::Colour accentOf(juce::Slider& s)
    {
        const auto c = s.findColour(juce::Slider::textBoxOutlineColourId);
        return c.isTransparent() ? Sp3ctraControls::active() : c.withAlpha(1.0f);
    }

    /** The "how much" surface of a bar / fader, on the ladder. */
    static juce::Colour fillOf(juce::Slider& s, Sp3ctraControls::Look lk, bool enabled)
    {
        const auto c = s.findColour(juce::Slider::trackColourId);
        const auto base = c.isTransparent() ? Sp3ctraControls::active() : c.withAlpha(1.0f);
        const bool resting = (lk.state == Sp3ctraControls::State::Idle && lk.heat <= 0.0f);
        return (resting ? Sp3ctraControls::restOf(base) : base)
                   .withAlpha(Sp3ctraControls::fillAlpha(lk, enabled));
    }

    /** Live reading of a button / combo box. */
    static Sp3ctraControls::Look lookOfWidget(const juce::Component& c, bool down,
                                              bool over, bool selected = false)
    {
        return Sp3ctraControls::lookOf(c, down, over, selected);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Bar-slider value label — the LinearBar text box covers the whole bar
    // and, by default, draws ITS OWN outline (Label::outlineColourId) on top
    // of the one drawLinearSlider paints. Sp3ctra paints the bar chrome in
    // ONE place (the LinearBar branch below, with hover/drag states), so the
    // label is text-only: transparent background, no outline.
    // ─────────────────────────────────────────────────────────────────────────
    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox(slider);
        if (slider.isBar())
        {
            l->setColour(juce::Label::outlineColourId,    juce::Colours::transparentBlack);
            l->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        }
        return l;
    }
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

        // Standard buttons — the panel's own background, and a border on the
        // interaction ladder: plain chrome at rest (a button is not a value),
        // the control colour under the pointer, white while it is pressed or
        // while a MIDI CC is firing it (Sp3ctraControls::heatOf).
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto baseColour = backgroundColour
            .withMultipliedBrightness(isButtonDown ? 0.7f : isMouseOverButton ? 1.1f : 1.0f);
        g.setColour(baseColour);
        g.fillRoundedRectangle(bounds, 3.0f);

        const auto look = lookOfWidget(button, isButtonDown, isMouseOverButton);
        const auto ink  = Sp3ctraControls::inkOf(look, Sp3ctraControls::active(),
                                                 Sp3ctraControls::kRoleOutline, button.isEnabled());
        const float lit = juce::jmax(look.edit(),
                                     look.state == Sp3ctraControls::State::Hover ? 1.0f : 0.0f);
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder)
                        .interpolatedWith(ink.line.withAlpha(1.0f), lit)
                        .withAlpha(button.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle(bounds, 3.0f, lit > 0.0f ? ink.lineW : 1.0f);
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
    // ToggleButton — sliding switch in the DC-block bar language: dark
    // interior, translucent accent fill when ON, 1 px accent outline, and a
    // solid-accent square knob (the power-LED cue). The accent is the nearest
    // ancestor's ToggleButton::tickColourId — the handle colour by default
    // (a toggle is a control), so toggles tint like the bar sliders.
    // ─────────────────────────────────────────────────────────────────────────
    /** Track geometry of the sliding switch below — shared with the pages so a
     *  toggle column is never laid out narrower than its own knob. */
    static constexpr float kSwitchH     = 20.0f;   ///< track height cap
    static constexpr float kSwitchRatio = 1.9f;    ///< track width = height x this

    /** Width drawToggleButton() needs at this height: lay a bare toggle out at
     *  least this wide, or the ON knob (right end of the track) is clipped. */
    static int toggleSwitchWidth(int height) noexcept
    {
        return (int) std::ceil(juce::jmin(kSwitchH, (float) height) * kSwitchRatio);
    }

    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override
    {
        const bool on      = button.getToggleState();
        const bool enabled = button.isEnabled();

        const auto accent = button.findColour(juce::ToggleButton::tickColourId, true)
                                  .withAlpha(1.0f);
        // The rung: hovered / pressed / remotely edited (a CC that flipped it
        // keeps it lit for kCtlGlowMs, exactly like a handle).
        const auto look = lookOfWidget(button, shouldDrawButtonAsDown,
                                       shouldDrawButtonAsHighlighted);
        const auto ink  = Sp3ctraControls::inkOf(look, accent,
                                                 Sp3ctraControls::kRoleOutline, enabled);

        // Switch geometry — vertically centred, left-aligned within the bounds.
        const float h = juce::jmin(kSwitchH, (float)button.getHeight());
        const float w = h * kSwitchRatio;
        const float y = ((float)button.getHeight() - h) * 0.5f;
        const auto  track = juce::Rectangle<float>(0.0f, y, w, h);
        constexpr float r = 2.0f;   // same corner family as the bars

        // Track — dark interior; accent fill when ON (the bar's "value" look,
        // on the same ladder: calm at rest, brighter under the pointer).
        g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
        g.fillRoundedRectangle(track, r);
        if (on)
        {
            g.setColour(Sp3ctraControls::valueInk(look, accent, enabled)
                            .withAlpha(Sp3ctraControls::fillAlpha(look, enabled)));
            g.fillRoundedRectangle(track, r);
        }
        g.setColour(ink.line.withMultipliedAlpha(on ? 1.0f : 0.75f));
        g.drawRoundedRectangle(track.reduced(0.5f), r, 1.0f);
        if (ink.hasHalo())
        {
            g.setColour(ink.halo.withMultipliedAlpha(0.5f));
            g.fillRoundedRectangle(track.expanded(2.0f), r + 1.0f);
        }

        // Sliding knob — the grabbable object: the value ink when ON, a muted
        // grey when OFF; both follow the ladder.
        constexpr float pad = 2.5f;
        const float knobS = h - 2.0f * pad;
        const float knobX = on ? (track.getRight() - pad - knobS)
                               : (track.getX() + pad);
        g.setColour(on ? Sp3ctraControls::valueInk(look, accent, enabled)
                       : juce::Colour(0xff4a4e58).withMultipliedAlpha(
                             enabled ? (look.hot() ? 1.0f : 0.8f) : 0.5f));
        g.fillRoundedRectangle(knobX, y + pad, knobS, knobS, r);

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
    // Linear Slider — two custom branches:
    //   • LinearHorizontal: explicit filled/unfilled colours so the "value"
    //     portion (left of thumb) is always brighter than the unfilled part;
    //   • LinearVertical (AudioMixPanel faders): Sp3ctraBarSlider's DC-block
    //     bar language turned vertical and slimmer — dark interior, translucent
    //     accent fill rising from the bottom, 1 px accent outline. Reads the
    //     same slider colour ids Sp3ctraBarSlider::setAccent sets, so a mixer
    //     strip tints its fader exactly like an editor tints its bars.
    // ─────────────────────────────────────────────────────────────────────────
    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        if (style == juce::Slider::LinearVertical)
        {
            // 12 px by default (thinner than the horizontal bars); a strip may
            // narrow it further through the "faderBarW" property (AUDIO MIX
            // send columns). The bar stays centred in the slider width.
            const auto& props = slider.getProperties();
            const float kBarW = props.contains("faderBarW")
                              ? (float) (double) props["faderBarW"] : 12.0f;
            const float barW = juce::jmin(kBarW, (float) width);
            const juce::Rectangle<float> bar((float) x + ((float) width - barW) * 0.5f,
                                             (float) y, barW, (float) height);
            const auto look = lookOfSlider(slider);
            const bool en   = slider.isEnabled();
            const auto ink  = Sp3ctraControls::inkOf(look, accentOf(slider),
                                                     Sp3ctraControls::kRoleOutline, en);

            g.setColour(slider.findColour(juce::Slider::backgroundColourId));
            g.fillRect(bar);

            // Fill bottom → value (sliderPos is the value's y; top = max).
            const float top = juce::jlimit((float) y, (float) (y + height), sliderPos);
            g.setColour(fillOf(slider, look, en));
            g.fillRect(juce::Rectangle<float>(bar.getX() + 0.5f, top,
                                              barW - 1.0f, bar.getBottom() - top));

            // Value edge — the crisp cursor of what is moving (mouse or MIDI).
            // A small mark, so it takes the full handle weight, not the
            // outline one.
            if (en && look.hot())
            {
                g.setColour(Sp3ctraControls::inkOf(look, accentOf(slider),
                                                   Sp3ctraControls::kRoleHandle, en).line);
                g.fillRect(juce::Rectangle<float>(bar.getX() + 0.5f,
                                                  juce::jlimit(bar.getY(), bar.getBottom() - 2.0f, top),
                                                  barW - 1.0f, 2.0f));
            }

            g.setColour(ink.line);
            g.drawRect(bar, 1.0f);

            // Graduations (AUDIO MIX faders set the "faderTicks" property):
            // a tick every 10% of the range beside the bar, longer/brighter
            // at 0 / 50 / 100 — the majors also print their value (through
            // the slider's own text conversion, so a percent fader reads
            // "50", not "0.5"). getPositionOfValue keeps them aligned with
            // the exact value↔pixel law the slider itself uses.
            if (slider.getProperties().contains("faderTicks"))
            {
                for (int t = 0; t <= 10; ++t)
                {
                    const double val = slider.proportionOfLengthToValue(t / 10.0);
                    const float  ty  = slider.getPositionOfValue(val);
                    const bool major = (t == 0 || t == 5 || t == 10);
                    const float len  = major ? 5.0f : 3.0f;
                    g.setColour(ink.line.withAlpha(major ? 0.6f : 0.3f));
                    g.fillRect(juce::Rectangle<float>(bar.getX() - len - 1.0f,
                                                      ty - 0.5f, len, 1.0f));
                    if (major)
                    {
                        // Right-justified against the tick, whatever room the
                        // strip width leaves left of the bar.
                        const int tx = juce::jmax(0, x - 2);
                        const int tw = (int) (bar.getX() - len - 3.0f) - tx;
                        g.setFont(juce::FontOptions(8.0f));
                        g.setColour(ink.line.withAlpha(0.55f));
                        g.drawText(slider.getTextFromValue(val),
                                   juce::Rectangle<int>(tx,
                                                        juce::jlimit(y, y + height - 9,
                                                                     (int) (ty - 4.5f)),
                                                        tw, 9),
                                   juce::Justification::centredRight, false);
                    }
                }
            }
            return;
        }

        if (style == juce::Slider::LinearBar)
        {
            // Sp3ctraBarSlider's DC-block bar. The bar owns NO state recipe of
            // its own: it publishes an accent (the handle colour by default,
            // an engine tint on the mixer strips) and the ladder decides the
            // rest — desaturated at rest, full accent under the pointer,
            // white value cursor while it is being moved by the mouse or by a
            // MIDI controller (Sp3ctraControls::heatOf, stamped by the bar's
            // MidiLearnAttachment).
            const juce::Rectangle<float> r((float) x, (float) y, (float) width, (float) height);
            const bool en   = slider.isEnabled();
            const auto look = lookOfSlider(slider);
            const auto ink  = Sp3ctraControls::inkOf(look, accentOf(slider),
                                                     Sp3ctraControls::kRoleOutline, en);

            g.setColour(slider.findColour(juce::Slider::backgroundColourId));
            g.fillRect(r);

            const float pos = juce::jlimit(r.getX(), r.getRight(), sliderPos);
            g.setColour(fillOf(slider, look, en));
            g.fillRect(juce::Rectangle<float>(r.getX() + 1.0f, r.getY() + 1.0f,
                                              juce::jmax(0.0f, pos - r.getX() - 1.0f),
                                              r.getHeight() - 2.0f));

            // Value edge — a crisp cursor at the end of the fill: the thing
            // that is moving. White while it is actually being edited, and at
            // full handle weight (a small mark, not a long outline).
            if (en && look.hot())
            {
                g.setColour(Sp3ctraControls::inkOf(look, accentOf(slider),
                                                   Sp3ctraControls::kRoleHandle, en).line);
                g.fillRect(juce::Rectangle<float>(juce::jlimit(r.getX(), r.getRight() - 2.0f, pos - 1.0f),
                                                  r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f));
            }
            g.setColour(ink.line);
            g.drawRect(r, 1.0f);
            return;
        }

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
        const auto  look    = lookOfSlider(slider);
        const auto  ink     = Sp3ctraControls::inkOf(look, Sp3ctraControls::active(),
                                                  Sp3ctraControls::kRoleOutline, en);

        // Unfilled portion (right of thumb) — very dark
        g.setColour(juce::Colour(0xff1a1f2a));
        g.fillRoundedRectangle(trackX, trackY - trackH * 0.5f, trackW, trackH, trackH * 0.5f);

        // Filled portion (left of thumb) — the control colour on the ladder
        const float filledW = thumbX - trackX;
        if (filledW > 0.f)
        {
            g.setColour(Sp3ctraControls::valueInk(look, Sp3ctraControls::active(), en)
                            .withMultipliedAlpha(0.85f));
            g.fillRoundedRectangle(trackX, trackY - trackH * 0.5f, filledW, trackH, trackH * 0.5f);
        }

        // Thumb — the grabbable object: same ink as a canvas node.
        constexpr float thumbR = 7.0f;
        if (ink.hasHalo())
        {
            g.setColour(ink.halo);
            g.fillEllipse(thumbX - thumbR - 3.f, trackY - thumbR - 3.f,
                          (thumbR + 3.f) * 2.f, (thumbR + 3.f) * 2.f);
        }
        g.setColour(Sp3ctraControls::valueInk(look, Sp3ctraControls::active(), en));
        g.fillEllipse(thumbX - thumbR, trackY - thumbR, thumbR * 2.f, thumbR * 2.f);
        g.setColour(ink.line);
        g.drawEllipse(thumbX - thumbR, trackY - thumbR, thumbR * 2.f, thumbR * 2.f, ink.lineW);
    }

    /** The vertical bar faders have no round thumb, so they don't need the V4
        thumb clearance — keep a hair of breathing room above/below the bar
        (Sp3ctraTheme::kVFaderInset, which the mixer's VU / overlay layout
        shares so they hug the bar's real extent). Horizontal sliders keep the
        indent their circular thumb requires. */
    int getSliderThumbRadius(juce::Slider& slider) override
    {
        return slider.isVertical() ? Sp3ctraTheme::kVFaderInset
                                   : LookAndFeel_V4::getSliderThumbRadius(slider);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Rotary Slider — dark knob body with a handle-colour value arc + pointer
    // (Sp3ctraTheme::kColHandle: a knob is something you touch). Track
    // #1a1f2a. Used by the audio-parameter knob grids.
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
        const bool  en   = slider.isEnabled();
        const auto  look = lookOfSlider(slider);
        const auto  ink  = Sp3ctraControls::inkOf(look, Sp3ctraControls::active(),
                                                  Sp3ctraControls::kRoleOutline, en);

        const juce::PathStrokeType arcStroke(
            arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        // Unfilled arc (full sweep) — very dark
        juce::Path bgArc;
        bgArc.addCentredArc(cx, cy, radius, radius, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff1a1f2a));
        g.strokePath(bgArc, arcStroke);

        // Filled arc (start → value) — the control colour on the ladder.
        // Centred parameters (pan) set "rotaryFromCentre": their arc grows
        // from 12 o'clock towards the value instead of sweeping from the min.
        const float arcFrom = slider.getProperties().contains("rotaryFromCentre")
                            ? (rotaryStartAngle + rotaryEndAngle) * 0.5f
                            : rotaryStartAngle;
        if (en && std::abs(angle - arcFrom) > 0.008f)
        {
            juce::Path valArc;
            valArc.addCentredArc(cx, cy, radius, radius, 0.0f,
                                 juce::jmin(arcFrom, angle),
                                 juce::jmax(arcFrom, angle), true);
            g.setColour(Sp3ctraControls::valueInk(look, Sp3ctraControls::active(), en)
                            .withMultipliedAlpha(0.85f));
            g.strokePath(valArc, arcStroke);
        }

        // Knob body
        const float knobR = radius - 5.0f;
        if (ink.hasHalo())
        {
            g.setColour(ink.halo.withMultipliedAlpha(0.6f));
            g.fillEllipse(cx - knobR - 3.f, cy - knobR - 3.f,
                          (knobR + 3.f) * 2.f, (knobR + 3.f) * 2.f);
        }
        g.setColour(juce::Colour(0xff22272f));
        g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.f, knobR * 2.f);
        g.setColour(look.hot() ? ink.line.withMultipliedAlpha(0.7f)
                               : juce::Colour(0xff33373f));
        g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.f, knobR * 2.f, 1.0f);

        // Pointer line — from inner radius outward, rotated to the value angle
        juce::Path pointer;
        pointer.startNewSubPath(0.0f, -knobR * 0.35f);
        pointer.lineTo(0.0f, -knobR * 0.92f);
        g.setColour(Sp3ctraControls::valueInk(look, Sp3ctraControls::active(), en));
        g.strokePath(pointer,
                     juce::PathStrokeType(2.5f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded),
                     juce::AffineTransform::rotation(angle).translated(cx, cy));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ComboBox — a control, so it speaks the bar language: dark interior,
    // handle-colour outline (brighter on hover / press) and a handle-colour
    // arrow. Same outline alphas as the bar sliders (35 % idle, 70 % hot).
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
        const bool en   = box.isEnabled();
        const auto look = lookOfWidget(box, isButtonDown, en && box.isMouseOver(true));
        const auto ink  = Sp3ctraControls::inkOf(look, Sp3ctraControls::active(),
                                                 Sp3ctraControls::kRoleOutline, en);

        // Background — the bar interior
        g.setColour(juce::Colour(en ? Sp3ctraTheme::kColBarBg : Sp3ctraTheme::kColBarBgOff)
                        .brighter(isButtonDown ? 0.12f : 0.0f));
        g.fillRoundedRectangle(boxR, radius);

        // Border — a bar's outline, on the ladder (desaturated at rest, full
        // accent under the pointer, white while a CC is changing the choice)
        g.setColour(ink.line);
        g.drawRoundedRectangle(boxR.reduced(0.5f), radius, 1.0f);

        // Small filled downward triangle, centred in the arrow-button zone
        if (en)
        {
            constexpr float aw = 6.0f; // arrow base width
            constexpr float ah = 4.0f; // arrow height
            const float ax = (float)buttonX + (float)buttonW * 0.5f - aw * 0.5f;
            const float ay = (float)height  * 0.5f - ah * 0.5f;

            juce::Path tri;
            tri.addTriangle(ax,          ay,
                            ax + aw,     ay,
                            ax + aw * 0.5f, ay + ah);
            g.setColour(Sp3ctraControls::valueInk(look, Sp3ctraControls::active(), en));
            g.fillPath(tri);
        }
    }

    /** V4's default reserves 30 px of label width for its wide chevron; the
     *  compact triangle above only needs ~16, and on narrow combos those dead
     *  pixels ellipsized short labels ("Ch 16" → "Ch …" on the MIDI MIX
     *  channel). The arrow zone follows automatically: ComboBox::paint hands
     *  drawComboBox everything right of the label as the button area. */
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(1, 1, box.getWidth() - 18, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
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
