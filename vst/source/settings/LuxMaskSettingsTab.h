#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief LuxMask settings tab — MIDI channel, octave offset, reference note,
 *        polyphony.
 *
 * Infrastructure parameters (not gameplay controls).
 * Mirrors the pattern of LuxPitchSettingsTab.
 */
class LuxMaskSettingsTab : public juce::Component
{
public:
    LuxMaskSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxMaskSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxMaskSettingsTab)
};
