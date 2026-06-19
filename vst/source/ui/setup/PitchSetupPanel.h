/**
 * @file PitchSetupPanel.h
 * @brief SETUP face of the PITCH block (zone 3, M5).
 *
 * Migrated 1:1 from the former gear-wheel LuxPitchSettingsTab —
 * same controls, same APVTS parameter IDs:
 *   luxpitchMidiChannel / luxpitchOctaveOffset /
 *   luxpitchReferenceNote / luxpitchPolyphony
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class PitchSetupPanel : public juce::Component
{
public:
    PitchSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~PitchSetupPanel() override;

    /** Natural content height (header + 4 rows). */
    static constexpr int kPreferredH = 190;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;

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
