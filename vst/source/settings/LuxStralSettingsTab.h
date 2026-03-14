#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"

//==============================================================================
/**
 * @brief LuxStral Synthesis Settings Tab
 *
 * Contains all parameters for the additive synthesis engine:
 * - Musical Tuning
 * - Envelope Parameters
 * - Image Processing
 * - Stereo Processing
 * - Dynamics Processing
 * - Performance
 * - StrokeForge (sine→square waveform morphing)
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
    Sp3ctraAudioProcessor& audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;

    // Viewport for scrolling
    juce::Viewport viewport;
    juce::Component contentComponent;

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

    // Section: Envelope Parameters
    juce::Label envelopeSectionLabel;
    juce::Label attackLabel;
    juce::Slider attackSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment;
    juce::Label releaseLabel;
    juce::Slider releaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;

    // Section: Image Processing
    juce::Label imageProcSectionLabel;
    juce::Label gammaEnableLabel;
    juce::ToggleButton gammaEnableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gammaEnableAttachment;
    juce::Label gammaValueLabel;
    juce::Slider gammaValueSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gammaValueAttachment;
    juce::Label contrastMinLabel;
    juce::Slider contrastMinSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> contrastMinAttachment;

    // Section: Stereo Processing
    juce::Label stereoSectionLabel;
    juce::Label stereoEnableLabel;
    juce::ToggleButton stereoEnableToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> stereoEnableAttachment;
    juce::Label stereoTempAmpLabel;
    juce::Slider stereoTempAmpSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stereoTempAmpAttachment;

    // Section: Dynamics Processing
    juce::Label dynamicsSectionLabel;
    juce::Label volumeWeightingLabel;
    juce::Slider volumeWeightingSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeWeightingAttachment;
    juce::Label softLimitThresholdLabel;
    juce::Slider softLimitThresholdSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitThresholdAttachment;
    juce::Label softLimitKneeLabel;
    juce::Slider softLimitKneeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitKneeAttachment;

    // Section: Performance
    juce::Label performanceSectionLabel;
    juce::Label numWorkersLabel;
    juce::Slider numWorkersSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numWorkersAttachment;

    // ── StrokeForge — Sine → Square waveform morphing ────────────────────
    // The waveform morph is controlled by stroke width:
    //   narrow stroke → g_waveform_morph≈0 → pure sine
    //   wide   stroke → g_waveform_morph≈1 → pure square (bandlimited)
    // sfMorphWidthScale = blob width (in notes) that saturates morph to 1.0
    juce::Label sfSectionLabel;

    juce::Label       sfEnabledLabel;
    juce::ToggleButton sfEnabledToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sfEnabledAttachment;

    juce::Label  sfBlobThresholdLabel;
    juce::Slider sfBlobThresholdSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfBlobThresholdAttachment;

    juce::Label  sfMinWidthLabel;
    juce::Slider sfMinWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMinWidthAttachment;

    juce::Label  sfMergeGapLabel;
    juce::Slider sfMergeGapSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMergeGapAttachment;

    juce::Label  sfMorphWidthLabel;
    juce::Slider sfMorphWidthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sfMorphWidthAttachment;

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
