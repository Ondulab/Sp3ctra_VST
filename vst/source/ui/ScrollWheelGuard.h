/**
 * @file ScrollWheelGuard.h
 * @brief Recursively disable mouse-wheel value changes on Sliders.
 *
 * By default a juce::Slider grabs the scroll wheel whenever the cursor is over
 * it and nudges its value.  Inside a scrollable panel this fights the user: a
 * wheel gesture meant to scroll the panel silently moves whatever knob/slider
 * happens to sit under the pointer.  Disabling per-slider scroll lets the wheel
 * event bubble up to the parent viewport so the panel scrolls instead.
 *
 * juce::ComboBox already defaults to scroll-disabled in this JUCE version, so
 * only Sliders need treatment here.
 *
 * Call once after a container has built its full child tree (typically at the
 * end of an editor/window constructor).  The walk is cheap and idempotent.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Sp3ctraUI
{
    /** Disable wheel-driven value changes on every Slider in the subtree of @p root. */
    inline void disableSliderScrollWheel(juce::Component& root)
    {
        if (auto* slider = dynamic_cast<juce::Slider*>(&root))
            slider->setScrollWheelEnabled(false);

        for (auto* child : root.getChildren())
            disableSliderScrollWheel(*child);
    }
}
