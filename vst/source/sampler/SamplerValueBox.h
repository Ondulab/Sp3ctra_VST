/**
 * @file SamplerValueBox.h
 * @brief Compact editable value chip for one LuxSampler per-slot play param.
 *
 * One box = one SamplerMidiTargets Kind, edited in NORMALISED 0..1 space so the
 * mouse gesture and a mapped MIDI CC land on exactly the same read/apply path:
 *
 *   • continuous  — left-drag vertically (mini-knob; Shift = fine), the value
 *     text follows live;
 *   • discrete    — left-click opens a popup at the cursor (LIN/EXP/LOG/S…),
 *     tick on the current entry;
 *   • right-click — NOT handled here: the owner attaches a MidiLearnAttachment
 *     to the box, which catches the popup click and shows the MIDI-Learn menu
 *     (plus the mapped badge dot, drawn top-right OVER this component).
 *
 * The box never caches the value: paint() and the gestures go through the
 * owner-supplied readNorm/applyNorm closures, so it always mirrors the engine
 * (image-handle drags, MIDI, slot switches) — the owner just calls repaint()
 * (see SlotEditorComponent::refreshParamBoxes).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"

class SamplerValueBox : public juce::Component
{
public:
    /** @p captionIn may be empty (discrete type boxes show just the value). */
    SamplerValueBox(juce::String captionIn, juce::Colour accentIn, bool discreteIn)
        : caption_(std::move(captionIn)), accent_(accentIn), discrete_(discreteIn)
    {
        setMouseCursor(discrete_ ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::UpDownResizeCursor);
    }

    std::function<float()>             readNorm;    // engine → normalised 0..1
    std::function<void(float)>         applyNorm;   // normalised 0..1 → engine
    std::function<juce::String(float)> format;      // normalised → display text

    /** Discrete boxes: popup entries (index/(N-1) = normalised value). */
    void setChoices(juce::StringArray c) { choices_ = std::move(c); }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour(0xff222230));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(dragging_ ? accent_.withAlpha(0.9f) : juce::Colour(0xff33373f));
        g.drawRoundedRectangle(b, 3.0f, 1.0f);

        const float n = readNorm ? readNorm() : 0.0f;
        const auto  r = getLocalBounds().reduced(5, 0);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        if (caption_.isNotEmpty())
        {
            g.setColour(accent_.withAlpha(0.55f));
            g.drawText(caption_, r, juce::Justification::centredLeft);
        }
        g.setColour(accent_.withAlpha(0.9f));
        g.drawText(format ? format(n) : juce::String(n, 2), r,
                   caption_.isNotEmpty() ? juce::Justification::centredRight
                                         : juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || discrete_ || ! readNorm)
            return;                       // right-click → MidiLearnAttachment
        dragStartNorm_ = readNorm();
        dragging_      = true;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging_ || ! applyNorm)
            return;
        // Full range over ~150 px of vertical travel; Shift = 10× finer.
        const float px    = e.mods.isShiftDown() ? 1500.0f : 150.0f;
        const float delta = (float) -e.getDistanceFromDragStartY() / px;
        applyNorm(juce::jlimit(0.0f, 1.0f, dragStartNorm_ + delta));
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging_)
        {
            dragging_ = false;
            repaint();
            return;
        }
        // Discrete: plain left-click opens the choice menu at the cursor.
        if (discrete_ && ! e.mods.isPopupMenu()
            && getLocalBounds().contains(e.getPosition()) && readNorm)
        {
            juce::PopupMenu m;
            const int cur = (int) std::lround(readNorm() * (float) (choices_.size() - 1));
            for (int i = 0; i < choices_.size(); ++i)
                m.addItem(i + 1, choices_[i], true, i == cur);

            juce::Component::SafePointer<SamplerValueBox> safe(this);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                                      .withMousePosition(),
                [safe](int result)
                {
                    if (safe == nullptr || result <= 0 || ! safe->applyNorm) return;
                    safe->applyNorm((float) (result - 1)
                                    / (float) juce::jmax(1, safe->choices_.size() - 1));
                    safe->repaint();
                });
        }
    }

private:
    juce::String      caption_;
    juce::Colour      accent_;
    bool              discrete_;
    juce::StringArray choices_;
    float             dragStartNorm_ = 0.0f;
    bool              dragging_      = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerValueBox)
};
