/**
 * @file SamplerSetupPanel.h
 * @brief SETUP face of the SAMPLER block (zone 3, M5).
 *
 * The whole former gear-wheel LuxSamplerSettingsTab component, moved and
 * re-parented. Play params are PER-ENGINE (fsEngineParam: engine A keeps the
 * legacy "luxSampler*" ids, engine B owns "luxSamplerB*"); setSamplerIndex()
 * rebinds every engine-scoped attachment:
 *   - MIDI Channel (1-16)                (luxSampler[B]MidiChannel)
 *   - Octave Offset (-2..+2)             (luxSampler[B]OctaveOffset)
 *   - Max Duration (1..60 s)             (luxSampler[B]MaxDuration)
 *   - REC / PLAY / SAVE MIDI bindings with MIDI-learn
 *       (luxSampler[B]Rec|Play|SaveBindType / ...BindNum)
 * Shared (session-level, not per-engine):
 *   - Image export toggle + format       (luxSamplerExportImages/-Format)
 *   - Output directory browse / clear    (processor get/setSamplerOutputDir)
 *   - 12-slot status grid (state, duration, clear), refreshed at 10 Hz
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class SamplerSetupPanel final : public juce::Component,
                                private juce::Timer
{
public:
    SamplerSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~SamplerSetupPanel() override;

    /** Natural content height (header + 9 control rows + 12-slot grid).
     *  (Enable row removed — power lives in the rack LED + zone-3 header.) */
    static constexpr int kPreferredH = 658;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Bind this panel to sampler engine 0 (A) or 1 (B) — rebinds every
     *  engine-scoped attachment (MIDI channel, octave, duration, bindings)
     *  to that engine's APVTS bank and cancels any pending MIDI-learn. */
    void setSamplerIndex(int i);

private:
    // Timer — refreshes slot display at 10 Hz
    void timerCallback() override;
    void updateSlotDisplays();

    // ─────────────────────────────────────────────────────────────────────────
    Sp3ctraAudioProcessor&              audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour                        accent;
    int                                 samplerIndex_ = 0;  // 0 = engine A, 1 = engine B

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
                        int actionId);   // 0 = REC, 1 = PLAY, 2 = SAVE

    /** (Re)create every engine-scoped attachment against the bank of
     *  samplerIndex_ (fsEngineParam) and cancel any pending MIDI-learn. */
    void rebindEngineParams();

    int  pendingLearnTarget = -1; // engine * 3 + action, mirrors the processor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerSetupPanel)
};
