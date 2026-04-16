#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

/**
 * @brief LuxSynth Additive Synthesis Settings Tab
 *
 * Contains parameters for the LuxSynth/LuxWave merged engine:
 * - Volume ADSR Envelope
 * - Filter ADSR Envelope
 * - Spectral Parameters (gamma, oscillators)
 * - LFO (vibrato)
 * - Engine Enable/Disable
 */
class LuxSynthSettingsTab : public juce::Component,
                             public juce::Slider::Listener
{
public:
    LuxSynthSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxSynthSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

    juce::Viewport viewport;
    juce::Component contentComponent;

    // Section: Engine Enable
    juce::Label enableSectionLabel;
    juce::Label enableLabel;
    juce::ToggleButton enableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;
    juce::Label midiChannelLabel;
    juce::ComboBox midiChannelCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiChannelAttachment;
    juce::Label octaveOffsetLabel;
    juce::ComboBox octaveOffsetCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveOffsetAttachment;

    // Section: Volume ADSR
    juce::Label volAdsrSectionLabel;
    juce::Label attackLabel;
    juce::Slider attackSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment;
    juce::Label decayLabel;
    juce::Slider decaySlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> decayAttachment;
    juce::Label sustainLabel;
    juce::Slider sustainSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sustainAttachment;
    juce::Label releaseLabel;
    juce::Slider releaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;

    // Section: Filter ADSR
    juce::Label fltAdsrSectionLabel;
    juce::Label fltAttackLabel;
    juce::Slider fltAttackSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltAttackAttachment;
    juce::Label fltDecayLabel;
    juce::Slider fltDecaySlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltDecayAttachment;
    juce::Label fltSustainLabel;
    juce::Slider fltSustainSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltSustainAttachment;
    juce::Label fltReleaseLabel;
    juce::Slider fltReleaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltReleaseAttachment;
    juce::Label fltCutoffLabel;
    juce::Slider fltCutoffSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltCutoffAttachment;
    juce::Label fltDepthLabel;
    juce::Slider fltDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fltDepthAttachment;

    // Section: Spectral
    juce::Label spectralSectionLabel;
    juce::Label gammaLabel;
    juce::Slider gammaSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gammaAttachment;
    juce::Label numOscLabel;
    juce::Slider numOscSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numOscAttachment;

    // Section: LFO
    juce::Label lfoSectionLabel;
    juce::Label lfoRateLabel;
    juce::Slider lfoRateSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfoRateAttachment;
    juce::Label lfoDepthLabel;
    juce::Slider lfoDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lfoDepthAttachment;

    void layoutContentComponent();
    void sliderValueChanged(juce::Slider* slider) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthSettingsTab)
};
