#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesampler/FrameSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Edit panel for the currently selected FrameSampler slot.
 *
 * Controls:
 *   REC / PLAY-STOP / CLEAR   — state-aware action buttons
 *   Start / End               — normalised playback position sliders [0..1]
 *   Speed                     — playback speed multiplier [0.1..8.0×]
 *   Loop mode                 — four radio-style buttons (NONE/LOOP/INV/PING)
 *   Priority                  — toggle for late-read priority flag
 *
 * Slider values are written directly to FrameSampler per-slot play params (Non-RT).
 * Slider values are refreshed from FrameSampler on slot switch (setSelectedSlot).
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

    /** Pull slider / priority values from FrameSampler and update UI silently. */
    void refreshSliderValues();

    /** Update loop-mode button highlight to reflect current LoopMode. */
    void refreshLoopButtons();

    /** Apply LoopMode m to the selected slot and refresh button highlights. */
    void applyLoopMode(LoopMode m);

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot = 0;
    bool blinkOn      = false;

    // ── Action buttons ────────────────────────────────────────────────────────
    juce::TextButton recBtn   { "REC" };
    juce::TextButton playBtn  { "PLAY" };
    juce::TextButton clearBtn { "CLEAR" };

    // ── Labels ────────────────────────────────────────────────────────────────
    juce::Label startLabel  { {}, "Start" };
    juce::Label endLabel    { {}, "End" };
    juce::Label speedLabel  { {}, "Speed" };
    juce::Label loopLabel   { {}, "Loop" };

    // ── Sliders ───────────────────────────────────────────────────────────────
    juce::Slider startSlider; // 0.0–1.0 normalised
    juce::Slider endSlider;   // 0.0–1.0 normalised
    juce::Slider speedSlider; // 0.1–8.0 speed multiplier

    // ── Loop mode (4 radio-style buttons: NONE / LOOP / INVERSE / PINGPONG) ──
    juce::TextButton loopBtns[4];

    // ── Priority ──────────────────────────────────────────────────────────────
    juce::ToggleButton priorityToggle { "Priority (late read)" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotEditorComponent)
};
