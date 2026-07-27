#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

//==============================================================================
/**
 * @brief System Settings Tab (machine-level settings, cf. plan C9)
 *
 * Everything that is NOT a per-block musical setting:
 *   - Log Level (Error/Warning/Info/Debug)        [from GeneralSettingsTab]
 *   - LuxStral Worker Threads                     [from LuxStralSettingsTab]
 *   - Detached video window default width/height  [from VideoScrollSettingsTab]
 *
 * Per-block settings now live in the zone-3 SETUP faces (M5);
 * the waterfall display settings live in the zone-4 toolbar.
 */
class SystemSettingsTab : public juce::Component
{
public:
    explicit SystemSettingsTab(Sp3ctraAudioProcessor& processor);
    ~SystemSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

    // Log Level
    juce::Label logLevelLabel;
    juce::ComboBox logLevelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> logLevelAttachment;

    // LuxStral worker threads
    juce::Label numWorkersLabel;
    juce::Slider numWorkersSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numWorkersAttachment;

    // MIDI — follow the module a controller edits (auto-navigate)
    juce::Label        midiSectionLabel;
    juce::Label        midiFollowLabel;
    juce::ToggleButton midiFollowToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> midiFollowAttachment;

    // Detached video window default size
    juce::Label  videoWindowSectionLabel;
    juce::Label  windowWLabel;
    juce::Slider windowWSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> windowWAttachment;
    juce::Label  windowHLabel;
    juce::Slider windowHSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> windowHAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SystemSettingsTab)
};
