/**
 * @file LuxWaveSetupPanel.h
 * @brief SETUP face of the ♪ LUXWAVE block (zone 3, M5).
 *
 * Migrated 1:1 from the former gear-wheel LuxWaveSettingsTab —
 * same controls, same APVTS parameter IDs:
 *   luxwaveMidiChannel / luxwaveOctaveOffset
 *   (enable moved to the rack LED + zone-3 header power switch)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class LuxWaveSetupPanel : public juce::Component
{
public:
    LuxWaveSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~LuxWaveSetupPanel() override;

    /** Natural content height (header + 2 rows). */
    static constexpr int kPreferredH = 108;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;

    juce::Label midiChannelLabel;
    juce::ComboBox midiChannelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiChannelAttachment;

    juce::Label octaveOffsetLabel;
    juce::ComboBox octaveOffsetCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveOffsetAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxWaveSetupPanel)
};
