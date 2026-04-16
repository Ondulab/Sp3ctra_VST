#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

//==============================================================================
/**
 * @brief LuxStral Synthesis Settings Tab
 *
 * Contains ONLY parameters that are NOT already exposed in the main
 * SYNTH → LuxStral or IMAGE → LuxStral interfaces:
 * - Engine Enable
 * - Musical Tuning
 * - Dynamics Processing (Soft Limit only)
 * - Performance (Worker Threads)
 *
 * Parameters already in the main UI (Envelope, Image Processing, Stereo,
 * StrokeForge) have been intentionally removed to avoid duplication.
 */
class LuxStralSettingsTab : public juce::Component,
                            public juce::ComboBox::Listener,
                            public juce::Slider::Listener
{
public:
    LuxStralSettingsTab(Sp3ctraAudioProcessor& processor);
    ~LuxStralSettingsTab() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;

    // Viewport for scrolling
    juce::Viewport viewport;
    juce::Component contentComponent;

    // Section: Engine Enable
    juce::Label enableLabel;
    juce::ToggleButton enableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;

    // Section: Musical Tuning
    juce::Label tuningRangeSectionLabel;
    juce::Label tuningLabel;
    juce::Slider tuningSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tuningAttachment;
    juce::Label rootNoteLabel;
    juce::ComboBox rootNoteComboBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rootNoteAttachment;
    juce::Label numOctavesLabel;
    juce::Slider numOctavesSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numOctavesAttachment;
    juce::Label freqRangeInfoLabel;
    juce::Label physiologicalFilterLabel;
    juce::ToggleButton physiologicalFilterToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> physiologicalFilterAttachment;
    juce::Label physiologicalDepthLabel;
    juce::Slider physiologicalDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> physiologicalDepthAttachment;

    // Section: Dynamics Processing (Soft Limit only)
    juce::Label dynamicsSectionLabel;
    juce::Label softLimitThresholdLabel;
    juce::Slider softLimitThresholdSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitThresholdAttachment;
    juce::Label softLimitKneeLabel;
    juce::Slider softLimitKneeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitKneeAttachment;

    // Section: StrokeForge Advanced Blob Detection
    juce::Label sfBlobSectionLabel;
    juce::Label contrastAdaptiveLabel;
    juce::ToggleButton contrastAdaptiveToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> contrastAdaptiveAttachment;
    juce::Label contrastSensLabel;
    juce::Slider contrastSensSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> contrastSensAttachment;

    // Section: Performance
    juce::Label performanceSectionLabel;
    juce::Label numWorkersLabel;
    juce::Slider numWorkersSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numWorkersAttachment;

    void layoutContentComponent();

    // Listener callbacks
    void comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged) override;
    void sliderValueChanged(juce::Slider* slider) override;

    // Helper functions
    float getRootNoteFrequency() const;
    int getMaxOctavesForRootNote() const;
    void updateOctavesSliderRange();
    void updateFrequencyRangeInfo();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxStralSettingsTab)
};
