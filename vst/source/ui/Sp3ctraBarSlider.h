/**
 * @file Sp3ctraBarSlider.h
 * @brief THE horizontal value control of the Sp3ctra UI — one class to rule
 *        every horizontal slider (the DC BLOCK "Amount" rectangle look).
 *
 * Replaces the legacy blue thumb sliders (LinearHorizontal) AND the per-editor
 * initBox() recipes (LinearBar + colour boilerplate). Change THIS file to
 * restyle or re-behave every horizontal slider in the interface.
 *
 * Behaviour contract:
 *   • left click / drag — sets the value where the pointer is (the bar fills
 *     to the click point, then follows the drag);
 *   • double-click     — cycles min → centre → max → min, judged from the
 *     value BEFORE the click pair (the first click's jump is ignored as the
 *     cycle reference — see preClickValue_). The centre is the slider's
 *     double-click return value when one is set and it is not an
 *     endpoint (the parameter's pertinent default: 1.0× for speeds, 440 Hz
 *     for tunings…), otherwise the skew-aware physical centre of the range.
 *     There is NO text editing on double-click (the text overlay is
 *     read-only) — use drag for precision, MIDI learn for automation;
 *   • right-click      — reserved: never drags nor edits the value, so the
 *     MidiLearnAttachment popup owns the gesture everywhere;
 *   • mouse wheel      — unchanged (pages opt out via ScrollWheelGuard).
 *
 * Rotary knobs keep their own style. The vertical mixer faders (AudioMixPanel)
 * reuse this bar's visual language — slimmer and vertical — via
 * Sp3ctraLookAndFeel::drawLinearSlider's LinearVertical branch; behaviourally
 * they stay plain sliders. This class itself is horizontal-only by design.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

class Sp3ctraBarSlider : public juce::Slider
{
public:
    /** The UI's blue — same accent as Sp3ctraLookAndFeel's linear/rotary sliders. */
    static constexpr juce::uint32 kDefaultAccent = 0xff4fa3e0;

    Sp3ctraBarSlider()
    {
        setSliderStyle(juce::Slider::LinearBar);
        // Read-only overlay: the value text is display-only (no double-click
        // editor) — double-click is the min/centre/max cycle instead.
        setTextBoxStyle(juce::Slider::TextBoxAbove, true, 0, 0);
        setAccent(juce::Colour(kDefaultAccent));
    }

    /** Re-tint the bar (chain-module editors pass their module accent). */
    void setAccent(juce::Colour accentColour)
    {
        accent_ = accentColour;
        setColour(juce::Slider::trackColourId,          accent_.withAlpha(0.22f));
        setColour(juce::Slider::backgroundColourId,     juce::Colour(0xff181820));
        setColour(juce::Slider::textBoxTextColourId,    juce::Colours::white.withAlpha(0.92f));
        setColour(juce::Slider::textBoxOutlineColourId, accent_.withAlpha(0.3f));
    }

    juce::Colour getAccent() const noexcept { return accent_; }

    /** Pure display bars (e.g. the video position scrubber) turn the
     *  double-click cycle off — a double-click then does nothing. */
    void setCycleEnabled(bool shouldCycle) noexcept { cycleEnabled_ = shouldCycle; }

    //==========================================================================
    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click is the MIDI-learn gesture (MidiLearnAttachment listens
        // on this component) — it must never start a value drag.
        dragging_ = false;
        if (e.mods.isPopupMenu() || ! isEnabled())
            return;

        if (e.getNumberOfClicks() == 1)
        {
            // Click-to-set: the base mouseDown ends in mouseDrag, which snaps
            // the value to the pointer. Keep the pre-click value around: it is
            // the double-click cycle's reference (the jump must not corrupt it).
            preClickValue_ = getValue();
            dragging_ = true;
            juce::Slider::mouseDown(e);
        }
        // Second click of a double-click: no jump — mouseDoubleClick cycles
        // from preClickValue_.
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (! dragging_)
        {
            if (! e.mouseWasDraggedSinceMouseDown()) return;
            dragging_ = true;
            juce::Slider::mouseDown(e);   // opens the drag session (gesture begin)
        }
        juce::Slider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging_) juce::Slider::mouseUp(e);
        dragging_ = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || ! isEnabled() || ! cycleEnabled_) return;

        const double lo = getMinimum(), hi = getMaximum();
        if (! (hi > lo)) return;

        // Pertinent centre: the declared default when it is a real mid value,
        // else the skew-aware physical centre (1.0× on the skewed speed bar).
        double centre = proportionOfLengthToValue(0.5);
        if (isDoubleClickReturnEnabled())
        {
            const double d = getDoubleClickReturnValue();
            if (d > lo && d < hi) centre = d;
        }

        const double pLo = 0.0, pHi = 1.0;
        double pC = juce::jlimit(0.0, 1.0, valueToProportionOfLength(centre));
        constexpr double eps = 0.02;
        if (pC - pLo < 2.0 * eps || pHi - pC < 2.0 * eps)   // default sits on an
        {                                                   // endpoint: use the
            centre = proportionOfLengthToValue(0.5);        // physical centre
            pC     = 0.5;
        }

        // Cycle from the value BEFORE the click pair — the first click of the
        // double-click already snapped the value to the pointer (mouseDown).
        const double p = juce::jlimit(0.0, 1.0,
                                      valueToProportionOfLength(preClickValue_));
        double target;
        if      (p - pLo < eps)           target = centre;  // min    → centre
        else if (std::abs(p - pC) < eps)  target = hi;      // centre → max
        else if (pHi - p < eps)           target = lo;      // max    → min
        else                              target = centre;  // free   → centre
        setValue(target, juce::sendNotificationSync);
    }

private:
    juce::Colour accent_ { kDefaultAccent };
    bool   cycleEnabled_  = true;
    bool   dragging_      = false;
    double preClickValue_ = 0.0;   // value before the last first-click (cycle ref)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraBarSlider)
};
