#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesampler/FrameSampler.h"
#include "SlotTimelineComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Edit panel for the currently selected FrameSampler slot.
 *
 * Layout — two vertical columns inside a full-width zone:
 *   Left  (~63 %):
 *     REC / PLAY-STOP / CLEAR   — state-aware action buttons
 *     Timeline                  — brightness waveform; drag handles set Start/End
 *                                 (Start/End sliders removed — edited directly on timeline)
 *   Right (~37 %):
 *     Speed    — playback speed multiplier [0.01..32.0×]; skewed so 1.0× is at centre
 *     Loop     — four radio-style buttons (NONE / LOOP / INV / PING)
 *     Resume   — toggle: resume from last stopped position
 *
 * Control values are written directly to FrameSampler per-slot play params (Non-RT).
 * Values are refreshed from FrameSampler on slot switch (setSelectedSlot).
 * Button states are refreshed at ~5 Hz via internal Timer.
 */
class SlotEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit SlotEditorComponent(Sp3ctraAudioProcessor& proc);
    ~SlotEditorComponent() override;

    /** Switch to editing a different slot (0–11). Refreshes all controls. */
    void setSelectedSlot(int idx);
    int  getSelectedSlot() const noexcept { return selectedSlot; }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Pull slider / resume values from FrameSampler and update UI silently. */
    void refreshSliderValues();

    /** Update loop-mode button highlight to reflect current LoopMode. */
    void refreshLoopButtons();

    /** Apply LoopMode m to the selected slot and refresh button highlights. */
    void applyLoopMode(LoopMode m);

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot = 0;
    bool blinkOn      = false;

    // ── Timeline visualizer ───────────────────────────────────────────────────
    SlotTimelineComponent timeline;

    // ── Action buttons ────────────────────────────────────────────────────────
    juce::TextButton recBtn   { "REC" };
    juce::TextButton playBtn  { "PLAY" };
    juce::TextButton clearBtn { "CLEAR" };

    // ── Labels ────────────────────────────────────────────────────────────────
    juce::Label  brightnessLabel { {}, "IMG" };
    juce::Slider brightnessSlider;
    juce::Label speedLabel { {}, "Speed" };
    juce::Label loopLabel  { {}, "Loop" };

    // ── Sliders ───────────────────────────────────────────────────────────────
    juce::Slider speedSlider; // 0.01–32.0×; skewed so 1.0× sits at centre position

    // ── Loop mode (4 radio-style buttons: NONE / LOOP / INVERSE / PINGPONG) ──
    juce::TextButton loopBtns[4];

    // ── Resume mode toggle ────────────────────────────────────────────────────
    juce::ToggleButton resumeToggle { "Resume from last position" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotEditorComponent)
};
