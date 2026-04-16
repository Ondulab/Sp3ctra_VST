#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief LuxSynth Settings Tab
 *
 * Contains only settings that are NOT exposed in the main Synth UI:
 * - Engine Enable/Disable
 * - MIDI Channel
 * - Octave Offset
 *
 * All synthesis parameters (ADSR, Filter, Spectral, LFO) are controlled
 * from the main SYNTH => LuxSynth page in PluginEditor.
 */
class LuxSynthSettingsTab : public juce::Component
{
public:
    LuxSynthSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxSynthSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

    // Section: Engine Enable
    juce::Label enableLabel;
    juce::ToggleButton enableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;

    juce::Label midiChannelLabel;
    juce::ComboBox midiChannelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiChannelAttachment;

    juce::Label octaveOffsetLabel;
    juce::ComboBox octaveOffsetCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveOffsetAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthSettingsTab)
};
