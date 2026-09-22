/**
 * @file Sp3ctraGestureSlider.h
 * @brief THE pointer-gesture contract shared by every value control of the
 *        Sp3ctra UI — horizontal bars, mixer faders, rotary knobs.
 *
 * One base class so every adjustable control answers the same gestures
 * (the same pair a hand-painted canvas handle answers — ui/Sp3ctraGestures.h):
 *
 *   • quick click, released — GO TO the pointed value. Linear styles only
 *     (bars and faders position absolutely; a rotary knob never jumps on a
 *     click). The jump happens on RELEASE, not on press: a press may still
 *     become a hold (type) or a double-click (reset), neither of which may
 *     be corrupted by a premature jump;
 *   • drag                  — live absolute positioning for bars/faders, the
 *     usual relative gesture for rotary knobs (base juce::Slider behaviour);
 *   • double-click          — RESET to the control's default: the
 *     double-click return value (a SliderAttachment stores the parameter's
 *     declared default there), else the skew-aware physical centre. The
 *     pair's first click may already have jumped the value on its release —
 *     the reset simply lands on top of it;
 *   • long press            — TYPE the value once the pointer stays still
 *     for kLongPressMs: opens the value text box as an editor in place (or a
 *     floating entry bubble over textless controls — the mini master, the
 *     textless level bars). Return commits through the slider's own text
 *     conversion (dB, "L37"/"C"/"R100"…), Esc cancels. The rest of that
 *     gesture is swallowed: the release neither jumps nor drags;
 *   • right-click           — reserved: never drags, never edits — the
 *     MidiLearnAttachment popup owns the gesture everywhere;
 *   • the wheel is untouched here (Sp3ctraBarSlider disables it at birth,
 *     plain sliders go through ScrollWheelGuard).
 *
 * Display-only scrubbers call setEditGesturesEnabled(false): click/drag keep
 * seeking, but a stray hold or double-click can no longer edit or reset.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "Sp3ctraGestures.h"

class Sp3ctraGestureSlider : public juce::Slider,
                             private juce::Timer
{
public:
    Sp3ctraGestureSlider() = default;
    ~Sp3ctraGestureSlider() override { stopTimer(); }

    /** Hold-to-type delay — one number for the whole UI (ui/Sp3ctraGestures.h). */
    static constexpr int kLongPressMs = Sp3ctraGestures::kLongPressMs;

    /** Pure display scrubbers (media/video position bars) keep click/drag
     *  seeking but turn the destructive extras off: no double-click reset, no
     *  long-press editor. */
    void setEditGesturesEnabled(bool shouldEdit) noexcept { editGesturesEnabled_ = shouldEdit; }

    /** The double-click, callable by hand: the declared default (parameter
     *  default through a SliderAttachment, or setDoubleClickReturnValue),
     *  else the skew-aware physical centre. */
    void resetToDefault()
    {
        const double lo = getMinimum(), hi = getMaximum();
        if (! (hi > lo) || ! isEnabled())
            return;
        const double target = isDoubleClickReturnEnabled()
                                  ? getDoubleClickReturnValue()
                                  : proportionOfLengthToValue(0.5);
        setValue(target, juce::sendNotificationSync);
    }

    /** The long press, callable by hand (a canvas handle whose values live
     *  in this box opens it from its own hold): the value text box as an
     *  in-place editor, or the entry bubble when the control has no text. */
    void openValueEditor()
    {
        if (! isEnabled()) return;
        if (auto* l = valueLabel())
        {
            // Works on read-only boxes too: the slider's onTextChange wiring
            // commits through getValueFromText when the editor closes.
            l->showEditor();
            return;
        }
        // Textless control: floating entry bubble (a CallOutBox is never
        // clipped by a 20 px wide fader).
        Sp3ctraGestures::openEntry(*this, { Sp3ctraGestures::fieldOf({}, *this) });
    }

    //==========================================================================
    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging_ = false;
        longPressFired_ = false;
        pressed_ = false;
        baseSession_ = false;
        repaint();
        if (e.mods.isPopupMenu() || ! isEnabled())
            return;
        if (e.getNumberOfClicks() != 1)
            return;                        // double-click → mouseDoubleClick

        pressed_ = true;
        if (editGesturesEnabled_ && getMaximum() > getMinimum())
            startTimer(kLongPressMs);

        // Rotary knobs keep their native press-anchored relative drag (no
        // value jump on press). Linear styles defer everything: a quick
        // release jumps, a real drag opens the session in mouseDrag.
        if (isRotary())
        {
            baseSession_ = true;
            juce::Slider::mouseDown(e);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! pressed_ || e.mods.isPopupMenu() || longPressFired_)
            return;                        // a long-press editor ate the gesture
        if (! dragging_)
        {
            if (! e.mouseWasDraggedSinceMouseDown())
                return;
            dragging_ = true;
            stopTimer();                   // moving hand ≠ holding hand
            if (! baseSession_)
            {
                baseSession_ = true;
                juce::Slider::mouseDown(e);   // opens the drag session (gesture begin)
            }
        }
        juce::Slider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        stopTimer();
        const bool wasPressed = pressed_;
        pressed_ = false;

        if (baseSession_)
        {
            juce::Slider::mouseUp(e);      // closes the host gesture
            baseSession_ = false;
        }
        else if (wasPressed && ! longPressFired_ && ! dragging_
                 && isLinearStyle() && isEnabled()
                 && e.getNumberOfClicks() == 1)
        {
            // Quick click released without drag: go to the pointed value —
            // done as a micro drag session so hosts still see a proper
            // begin / set / end automation gesture.
            juce::Slider::mouseDown(e);
            juce::Slider::mouseUp(e);
        }

        dragging_ = false;
        longPressFired_ = false;
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        stopTimer();
        pressed_ = false;
        if (e.mods.isPopupMenu() || ! isEnabled() || ! editGesturesEnabled_)
            return;
        resetToDefault();
    }

private:
    //==========================================================================
    bool isLinearStyle() const noexcept
    {
        const auto s = getSliderStyle();
        return s == LinearBar || s == LinearBarVertical
            || s == LinearHorizontal || s == LinearVertical;
    }

    void timerCallback() override
    {
        stopTimer();
        if (! pressed_ || dragging_ || ! isEnabled())
            return;
        // Long press = type the value. The release that follows must neither
        // jump (linear click) nor drag: the flag swallows the rest.
        longPressFired_ = true;
        openValueEditor();
    }

    /** The slider's value text box, when it has one (knob/fader box below,
     *  bar overlay label). */
    juce::Label* valueLabel() const
    {
        for (auto* c : getChildren())
            if (auto* l = dynamic_cast<juce::Label*>(c))
                return l;
        return nullptr;
    }

    //==========================================================================
    bool   editGesturesEnabled_ = true;
    bool   pressed_        = false;  ///< left press in flight (single click)
    bool   dragging_       = false;  ///< movement crossed the drag threshold
    bool   baseSession_    = false;  ///< base Slider gesture opened (needs mouseUp)
    bool   longPressFired_ = false;  ///< hold editor consumed this gesture

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraGestureSlider)
};
