/**
 * @file Sp3ctraBarSlider.h
 * @brief THE horizontal value control of the Sp3ctra UI — one class to rule
 *        every horizontal slider (the DC BLOCK "Amount" rectangle look).
 *
 * Replaces the legacy blue thumb sliders (LinearHorizontal) AND the per-editor
 * initBox() recipes (LinearBar + colour boilerplate). Change THIS file to
 * restyle every horizontal slider in the interface.
 *
 * Behaviour: the UI-wide gesture contract lives in Sp3ctraGestureSlider —
 * quick click released = go to the pointed value, drag = follow, double-click
 * = reset to the default, long press = type the value in place,
 * right-click = MIDI learn (MidiLearnAttachment owns it). This file only
 * adds the bar's LOOK on top:
 *   • mouse wheel — never changes the value (the wheel scrolls the hosting
 *     page; see ScrollWheelGuard for plain sliders);
 *   • the value text overlay is read-only outside the long-press editor.
 *
 * Looks: the bar owns NO state recipe. It publishes one accent colour and
 * Sp3ctraLookAndFeel paints the interaction ladder from it (ui/Sp3ctraControls.h) —
 * desaturated at rest, full accent under the pointer, white value cursor while
 * it moves. A controller moving the bar's parameter lights it the same way:
 * MidiLearnAttachment stamps the remote-edit heat on this component (the
 * MidiTouch bus in ui/Sp3ctraControls.h) for Sp3ctraTheme::kCtlGlowMs. The
 * bar deliberately does NOT glow on every value change: rebinding a page to
 * another instance rewrites every box, and that is not an edit.
 *
 * Rotary knobs keep their own style. The vertical mixer faders (AudioMixPanel)
 * reuse this bar's visual language — slimmer and vertical — via
 * Sp3ctraLookAndFeel::drawLinearSlider's LinearVertical branch; behaviourally
 * they share the same Sp3ctraGestureSlider contract. This class itself is
 * horizontal-only by design.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestureSlider.h"

class Sp3ctraBarSlider : public Sp3ctraGestureSlider
{
public:
    /** THE control colour (Sp3ctraTheme::kColHandle) — a bar is something you
     *  touch, so it takes the handle hue like every node and toggle. Module
     *  editors keep this default; only identity strips (the mixer faders)
     *  re-tint through setAccent. */
    static constexpr juce::uint32 kDefaultAccent = Sp3ctraTheme::kColHandle;

    Sp3ctraBarSlider()
    {
        setSliderStyle(juce::Slider::LinearBar);
        // Read-only overlay: the value text is display-only — typing goes
        // through the double-click editor (Sp3ctraGestureSlider), never
        // through a single click that must stay the go-to-value gesture.
        setTextBoxStyle(juce::Slider::TextBoxAbove, true, 0, 0);
        // Wheel-inert from birth: inside the scrollable zone-3 pages a wheel
        // gesture must scroll the page, never nudge the bar under the
        // pointer. The one-shot ScrollWheelGuard walk only covers sliders
        // that exist when the editor is built — pages built later (the
        // VIDEO SCROLL "ALL" view) were slipping through.
        setScrollWheelEnabled(false);
        setAccent(juce::Colour(kDefaultAccent));
    }

    /** Re-tint the bar — for IDENTITY strips only (mixer levels per engine).
     *  Module-page boxes keep the default handle colour. */
    void setAccent(juce::Colour accentColour)
    {
        accent_ = accentColour;
        applyColours();
    }

    /** Disabled bars must READ disabled: the accent fill and outline all but
     *  vanish and the interior darkens (the value text already dims through
     *  the label's own enablement) — not just a slightly greyer number. */
    void enablementChanged() override
    {
        applyColours();
        repaint();
    }

    juce::Colour getAccent() const noexcept { return accent_; }

    //==========================================================================
    // Hover / press feedback — the LookAndFeel's LinearBar branch reads the
    // live mouse state at paint time; these just make sure a paint happens
    // on every transition (juce::Slider only repaints on value change).
    void mouseEnter(const juce::MouseEvent& e) override { Sp3ctraGestureSlider::mouseEnter(e); repaint(); }
    void mouseExit (const juce::MouseEvent& e) override { Sp3ctraGestureSlider::mouseExit(e);  repaint(); }

private:
    void applyColours()
    {
        const bool en = isEnabled();
        // The bar publishes its ACCENT, nothing else: every alpha of every
        // state (rest / hover / edit, enabled or not) is resolved by the
        // ladder in Sp3ctraLookAndFeel's LinearBar branch.
        setColour(juce::Slider::trackColourId,          accent_);
        setColour(juce::Slider::backgroundColourId,     juce::Colour(en ? Sp3ctraTheme::kColBarBg
                                                                        : Sp3ctraTheme::kColBarBgOff));
        setColour(juce::Slider::textBoxTextColourId,    juce::Colours::white.withAlpha(en ? 0.92f : 0.45f));
        setColour(juce::Slider::textBoxOutlineColourId, accent_);
    }

    juce::Colour accent_ { kDefaultAccent };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraBarSlider)
};
