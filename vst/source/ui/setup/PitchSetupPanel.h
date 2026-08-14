/**
 * @file PitchSetupPanel.h
 * @brief SETUP face of the PITCH block (zone 3, M5).
 *
 * Migrated 1:1 from the former gear-wheel LuxPitchSettingsTab — same controls,
 * now bound to the selected instance's PER-INSTANCE bank (luxpitch{slot}_*):
 * setSlot(slot) rebinds every attachment (same pattern as VideoScrollPage).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"
#include "../Sp3ctraBarSlider.h"

class PitchSetupPanel : public juce::Component
{
public:
    PitchSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~PitchSetupPanel() override;

    /** Bind every control to the PITCH bank of `slot` (0..7) — the selected
     *  instance's parameters. */
    void setSlot(int slot);
    int  slot() const noexcept { return slot_; }

    /** Natural content height (header + 7 rows). */
    static constexpr int kPreferredH = 295;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ = 0;   // pool slot of the bound instance

    // Step Mode (LuxStral / Free)
    juce::Label    couplingLabel;
    juce::ComboBox couplingCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> couplingAttachment;

    // Free pixels per semitone
    juce::Label  freeStepLabel;
    Sp3ctraBarSlider freeStepSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freeStepAttachment;

    // Pitch Bend Range
    juce::Label  pbRangeLabel;
    Sp3ctraBarSlider pbRangeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pbRangeAttachment;

    // MIDI Channel (1-16)
    juce::Label    midiChannelLabel;
    juce::ComboBox midiChannelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiChannelAttachment;

    // Octave Offset (-2..+2)
    juce::Label    octaveOffsetLabel;
    juce::ComboBox octaveOffsetCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveOffsetAttachment;

    // Reference Note (C1..B6, default A3)
    juce::Label    refNoteLabel;
    juce::ComboBox refNoteCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> refNoteAttachment;

    // Polyphony toggle
    juce::Label        polyphonyLabel;
    juce::ToggleButton polyphonyToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> polyphonyAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchSetupPanel)
};
