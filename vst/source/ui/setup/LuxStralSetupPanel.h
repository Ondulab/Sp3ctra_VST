/**
 * @file LuxStralSetupPanel.h
 * @brief SETUP face of the ♪ LUXSTRAL block (zone 3, M5).
 *
 * Migrated from the former gear-wheel LuxStralSettingsTab — same controls,
 * same APVTS parameter IDs:
 *   luxstralTuning / luxstralRootNote / luxstralNumOctaves /
 *   luxstralPhysiologicalFilter / luxstralPhysiologicalDepth /
 *   luxstralSoftLimitThreshold / luxstralSoftLimitKnee /
 *   luxstralRangeDb (decode window — moved here from the OUT page 2026-08-05) /
 *   sfBlobContrastAdaptive / sfBlobContrastSensitivity
 *
 * NOTE: the "Worker Threads" slider (luxstralNumWorkers) did NOT move here —
 * it lives in the gear-wheel SYSTEM tab (machine-level setting, cf. C9).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"
#include "../Sp3ctraBarSlider.h"

class LuxStralSetupPanel : public juce::Component,
                           public juce::ComboBox::Listener,
                           public juce::Slider::Listener
{
public:
    LuxStralSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~LuxStralSetupPanel() override;

    /** Natural content height (header + 3 sections, no internal viewport —
     *  the zone-3 viewport scrolls). The TIMBRE section (sample wavetable)
     *  lives on the engine PLAY page — it is a performance control set. */
    static constexpr int kPreferredH = 545;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;

    // Section: Musical Tuning
    juce::Label tuningRangeSectionLabel;
    juce::Label tuningLabel;
    Sp3ctraBarSlider tuningSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tuningAttachment;
    juce::Label rootNoteLabel;
    juce::ComboBox rootNoteComboBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rootNoteAttachment;
    juce::Label numOctavesLabel;
    Sp3ctraBarSlider numOctavesSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> numOctavesAttachment;
    juce::Label freqRangeInfoLabel;
    juce::Label physiologicalFilterLabel;
    juce::ToggleButton physiologicalFilterToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> physiologicalFilterAttachment;
    juce::Label physiologicalDepthLabel;
    Sp3ctraBarSlider physiologicalDepthSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> physiologicalDepthAttachment;

    // Section: Dynamics Processing (Soft Limit + decode window)
    juce::Label dynamicsSectionLabel;
    juce::Label softLimitThresholdLabel;
    Sp3ctraBarSlider softLimitThresholdSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitThresholdAttachment;
    juce::Label softLimitKneeLabel;
    Sp3ctraBarSlider softLimitKneeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> softLimitKneeAttachment;
    juce::Label rangeDbLabel;
    Sp3ctraBarSlider rangeDbSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> rangeDbAttachment;

    // Section: StrokeForge Advanced Blob Detection
    juce::Label sfBlobSectionLabel;
    juce::Label contrastAdaptiveLabel;
    juce::ToggleButton contrastAdaptiveToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> contrastAdaptiveAttachment;
    juce::Label contrastSensLabel;
    Sp3ctraBarSlider contrastSensSlider;
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
