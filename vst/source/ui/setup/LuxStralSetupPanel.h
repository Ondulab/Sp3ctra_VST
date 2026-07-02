/**
 * @file LuxStralSetupPanel.h
 * @brief SETUP face of the ♪ LUXSTRAL block (zone 3, M5).
 *
 * Migrated from the former gear-wheel LuxStralSettingsTab — same controls,
 * same APVTS parameter IDs:
 *   luxstralTuning / luxstralRootNote / luxstralNumOctaves /
 *   luxstralPhysiologicalFilter / luxstralPhysiologicalDepth /
 *   luxstralSoftLimitThreshold / luxstralSoftLimitKnee /
 *   sfBlobContrastAdaptive / sfBlobContrastSensitivity
 *
 * NOTE: the "Worker Threads" slider (luxstralNumWorkers) did NOT move here —
 * it lives in the gear-wheel SYSTEM tab (machine-level setting, cf. C9).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class LuxStralSetupPanel : public juce::Component,
                           public juce::ComboBox::Listener,
                           public juce::Slider::Listener
{
public:
    LuxStralSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~LuxStralSetupPanel() override;

    /** Natural content height (header + 3 sections, no internal viewport —
     *  the zone-3 viewport scrolls). Enable row removed (−47 px). */
    static constexpr int kPreferredH = 505;

    /** Bind the SETUP face to LuxStral A (0) or B (1) — fired by the rack on
     *  selection (M8). Soft Limit rebinds to the engine's own params; Tuning /
     *  Physiological / StrokeForge-blob settings are SHARED and labelled so. */
    void setEngineIndex(int idx);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int engineIndex_ = 0;    // 0 = LuxStral A (luxstral*), 1 = B (luxstralB*)

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

    // Listener callbacks
    void comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged) override;
    void sliderValueChanged(juce::Slider* slider) override;

    // Helper functions
    float getRootNoteFrequency() const;
    int getMaxOctavesForRootNote() const;
    void updateOctavesSliderRange();
    void updateFrequencyRangeInfo();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxStralSetupPanel)
};
