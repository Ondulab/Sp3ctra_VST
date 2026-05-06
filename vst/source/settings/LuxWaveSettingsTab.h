#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief LuxWave Settings Tab
 *
 * Contains only settings that are NOT exposed in the main Synth UI:
 * - Engine Enable/Disable
 * - MIDI Channel
 * - Octave Offset
 *
 * All synthesis parameters (ADSR, Filter, LFO, Scan Mode) are controlled
 * from the main SYNTH => LuxWave page in PluginEditor.
 */
class LuxWaveSettingsTab : public juce::Component
{
public:
    LuxWaveSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxWaveSettingsTab() override;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxWaveSettingsTab)
};
