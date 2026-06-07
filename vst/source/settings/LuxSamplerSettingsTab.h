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
 *   - Max Duration (1..60 s)
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

    // Image export on Save Session
    juce::Label        exportImagesLabel;
    juce::ToggleButton exportImagesToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> exportImagesAttachment;

    juce::Label    exportFormatLabel;
    juce::ComboBox exportFormatCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> exportFormatAttachment;

    // Output directory (shared by SAVE SESSION and image export). When set,
    // bypasses the file chooser and writes directly into this directory.
    juce::Label      outputDirLabel;
    juce::Label      outputDirValueLabel;
    juce::TextButton outputDirBrowseBtn;
    juce::TextButton outputDirClearBtn;
    std::unique_ptr<juce::FileChooser> outputDirChooser;


    // Per-slot displays (12 rows)

    static constexpr int NUM_SLOTS = 12;
    juce::Label slotIndexLabel [NUM_SLOTS];
    juce::Label slotStateLabel [NUM_SLOTS];
    juce::Label slotDurLabel   [NUM_SLOTS];
    juce::TextButton slotClearBtn[NUM_SLOTS];

    // ─────────────────────────────────────────────────────────────────────────
    // Action button MIDI bindings (REC / PLAY / SAVE).
    //
    // Each row binds one transport button to an incoming MIDI event so the user
    // can trigger the corresponding action from a keyboard or controller. The
    // selected slot follows the SlotEditorComponent selection (mirrored on the
    // processor side via setSamplerSelectedSlot).
    //
    // Row layout:  [LABEL] [Type ▼ Off|Note|CC] [Number ◀▶ 0..127] [LEARN]
    //
    // The LEARN button puts the audio thread into capture mode (atomic int on
    // the processor). On the next incoming MIDI Note/CC matching the configured
    // channel, the captured (type, number) pair is applied to the APVTS params.
    // ─────────────────────────────────────────────────────────────────────────
    struct ActionBindingRow
    {
        juce::Label       title;
        juce::Label       typeLabel;
        juce::ComboBox    typeBox;
        juce::Label       numberLabel;
        juce::Slider      numberSlider;
        juce::TextButton  learnBtn { "Learn" };

        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   numberAtt;
    };

    ActionBindingRow recBinding;
    ActionBindingRow playBinding;
    ActionBindingRow saveBinding;

    void initBindingRow(ActionBindingRow& row,
                        const juce::String& title,
                        const juce::String& typeParamId,
                        const juce::String& numParamId,
                        int learnTargetId);

    int  pendingLearnTarget = -1; // mirrors processor.getSamplerMidiLearnTarget()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSamplerSettingsTab)
};

