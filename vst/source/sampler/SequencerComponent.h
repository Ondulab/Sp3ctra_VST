#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesequencer/FrameSequencer.h"
#include "../framesampler/FrameSampler.h"
#include <functional>
#include <cmath>

class Sp3ctraAudioProcessor;

/**
 * @brief Step sequencer grid — 16 steps in 2 rows of 8, square cells.
 *
 * Above the grid a mini spectral timeline shows the selected slot's
 * content, active zone (start/end bars) and playhead.
 *
 * Interaction model:
 *   Left click   → increment bank (+1)
 *   Right click  → decrement bank (−1)
 *   Drag up/down → increment / decrement continuously
 */
class SequencerComponent : public juce::Component,
                           private juce::Timer
{
public:
    explicit SequencerComponent(Sp3ctraAudioProcessor& proc);
    ~SequencerComponent() override;

    void paint  (juce::Graphics& g) override;
    void resized() override;

    /** Wired by SamplerPageComponent — returns the currently selected slot index. */
    std::function<int()> getSelectedSlot;

    // Display constants
    static constexpr int kDisplayCols  = 8;
    static constexpr int kDisplayRows  = 2;
    static constexpr int kDisplaySteps = kDisplayCols * kDisplayRows; // 16

private:
    // ── StepCell ─────────────────────────────────────────────────────────────
    struct StepCell final : public juce::TextButton
    {
        /** Called with delta = +1 or -1. */
        std::function<void(int delta)> onStep;

        // Flat fill, no border/outline
        void paintButton(juce::Graphics& g, bool isHighlighted, bool isDown) override
        {
            auto bg = findColour(juce::TextButton::buttonColourId);
            if (isHighlighted) bg = bg.brighter(0.08f);
            if (isDown)        bg = bg.darker(0.12f);
            g.setColour(bg);
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            // No border
            g.setColour(findColour(juce::TextButton::textColourOffId));
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(getButtonText(), getLocalBounds(),
                       juce::Justification::centred, false);
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            dragStartY_   = e.position.y;
            dragAccSteps_ = 0;
            isDrag_       = false;
            if (e.mods.isRightButtonDown())
            {
                if (onStep) onStep(-1);
                return;
            }
            juce::TextButton::mouseDown(e);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown()) return;
            constexpr float kPxPerStep = 8.0f;
            const int steps = static_cast<int>(
                (dragStartY_ - e.position.y) / kPxPerStep);
            if (steps != dragAccSteps_)
            {
                const int diff = steps - dragAccSteps_;
                dragAccSteps_  = steps;
                isDrag_        = true;
                if (onStep) onStep(diff);
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown()) return;
            if (!isDrag_)
                if (onStep) onStep(+1);
            isDrag_       = false;
            dragAccSteps_ = 0;
            juce::TextButton::mouseUp(e);
        }

    private:
        float dragStartY_   = 0.f;
        int   dragAccSteps_ = 0;
        bool  isDrag_       = false;
    };

    // ── Helpers ──────────────────────────────────────────────────────────────
    void timerCallback() override;
    void updateButton(int i);
    void paintMiniTimeline(juce::Graphics& g) const;

    Sp3ctraAudioProcessor& processor;
    StepCell stepBtns[FrameSequencer::MAX_STEPS];
    int cachedNumSteps = -1;

    // Layout geometry — set in resized(), consumed in paint()
    int cachedCellH_       = 0;
    int cachedTimelineH_   = 60;
    int cachedCellsStartY_ = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerComponent)
};
