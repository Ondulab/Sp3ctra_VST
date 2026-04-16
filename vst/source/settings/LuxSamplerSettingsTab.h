#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief LuxSampler Settings Tab
 *
 * UI for configuring and monitoring the LuxSampler subsystem.
 * Refreshes slot states at 10 Hz via juce::Timer.
 *
 * Controls:
 *   - Enable toggle
 *   - MIDI Channel (1-16)
 *   - Octave Offset (-2..+2)
 *   - Max Duration (1..10 s)
 *   - 12-slot status grid (state, duration, clear)
 *   - Save / Load / Clear All buttons
 */
class LuxSamplerSettingsTab final : public juce::Component,
                                      private juce::Timer
{
public:
    explicit LuxSamplerSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxSamplerSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Timer — refreshes slot display at 10 Hz
    void timerCallback() override;
    void updateSlotDisplays();

    // ─────────────────────────────────────────────────────────────────────────
    Sp3ctraAudioProcessor&              audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;

    // Enable
    juce::Label        enableLabel;
    juce::ToggleButton enableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;

    // MIDI Channel
    juce::Label    midiChannelLabel;
    juce::ComboBox midiChannelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiChannelAttachment;

    // Octave Offset
    juce::Label    octaveOffsetLabel;
    juce::ComboBox octaveOffsetCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveOffsetAttachment;

    // Max Duration
    juce::Label  maxDurationLabel;
    juce::Slider maxDurationSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxDurationAttachment;

    // Per-slot displays (12 rows)
    static constexpr int NUM_SLOTS = 12;
    juce::Label slotIndexLabel [NUM_SLOTS];
    juce::Label slotStateLabel [NUM_SLOTS];
    juce::Label slotDurLabel   [NUM_SLOTS];
    juce::TextButton slotClearBtn[NUM_SLOTS];


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSamplerSettingsTab)
};
