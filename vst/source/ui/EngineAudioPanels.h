/**
 * @file EngineAudioPanels.h
 * @brief Audio-parameter panels for the ♪ engine blocks (M4 four-zone shell).
 *
 * These components host the EXACT controls that previously lived directly in
 * PluginEditor under the SYNTH tab's AUDIOSTRAL / AUDIOSYNTH / AUDIOWAVE
 * sub-pages (same APVTS parameter IDs, same attachments, same painted
 * labels).  Packaging them as components lets ZONE 3 stack each one under
 * its matching image-pipeline page:
 *
 *   ♪ LUXSTRAL block → LuxStralTabComponent + AudioStralPanel
 *   ♪ LUXSYNTH block → LuxSynthTabComponent + AudioSynthPanel
 *   ♪ LUXWAVE  block → AudioWavePanel (alone)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"

//==============================================================================
/** AUDIOSTRAL — LuxStral audio params (left) + StrokeForge controls (right). */
class AudioStralPanel : public juce::Component
{
public:
    explicit AudioStralPanel(Sp3ctraAudioProcessor& p);

    /** Natural content height (8 control rows + section badge). */
    static constexpr int kPreferredH = Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + 8 * Sp3ctraTheme::kRowStep + 8;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::ToggleButton deviceOnToggle, stereoEnableToggle;
    juce::Slider luxstralVolumeSlider, attackSlider, releaseSlider,
                 stereoTempSlider, sumExpSlider, noiseGateSlider;

    juce::ToggleButton sfEnabledToggle, sfFocusOnlyToggle;
    juce::Slider sfMorphWidthSlider, sfFocusSigmaSlider, sfSpectralThreshSlider;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<BtnAttach> deviceOnAttachment, stereoEnableAttachment,
                               sfEnabledAttachment, sfFocusOnlyAttachment;
    std::unique_ptr<SldAttach> luxstralVolumeAttachment, attackAttachment,
                               releaseAttachment, stereoTempAttachment,
                               sumExpAttachment, noiseGateAttachment,
                               sfMorphWidthAttachment, sfFocusSigmaAttachment,
                               sfSpectralThreshAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioStralPanel)
};

//==============================================================================
/** AUDIOSYNTH — LuxSynth volume ADSR + spectral (left), filter ADSR + LFO (right). */
class AudioSynthPanel : public juce::Component
{
public:
    explicit AudioSynthPanel(Sp3ctraAudioProcessor& p);

    static constexpr int kPreferredH = Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + 8 * Sp3ctraTheme::kRowStep + 8;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::ToggleButton lxEnableToggle;
    juce::Slider luxsynthVolumeSlider;
    juce::Slider lxAttackSlider, lxDecaySlider, lxSustainSlider, lxReleaseSlider;
    juce::Slider lxFltAttackSlider, lxFltDecaySlider, lxFltSustainSlider, lxFltReleaseSlider;
    juce::Slider lxFltCutoffSlider, lxFltDepthSlider;
    juce::Slider lxNumOscSlider;
    juce::Slider lxLfoRateSlider, lxLfoDepthSlider;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<BtnAttach> lxEnableAttachment;
    std::unique_ptr<SldAttach> luxsynthVolumeAttachment,
                               lxAttackAttach, lxDecayAttach, lxSustainAttach, lxReleaseAttach,
                               lxFltAttackAttach, lxFltDecayAttach, lxFltSustainAttach,
                               lxFltReleaseAttach, lxFltCutoffAttach, lxFltDepthAttach,
                               lxNumOscAttach, lxLfoRateAttach, lxLfoDepthAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSynthPanel)
};

//==============================================================================
/** AUDIOWAVE — LuxWave wavetable ADSR + scan (left), filter + LFO (right). */
class AudioWavePanel : public juce::Component
{
public:
    explicit AudioWavePanel(Sp3ctraAudioProcessor& p);

    static constexpr int kPreferredH = Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + 8 * Sp3ctraTheme::kRowStep + 8;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::ToggleButton lwEnableToggle;
    juce::Slider luxwaveVolumeSlider, lwAmplitudeSlider;
    juce::Slider lwAttackSlider, lwDecaySlider, lwSustainSlider, lwReleaseSlider;
    juce::Slider lwFltCutoffSlider, lwFltDepthSlider;
    juce::Slider lwLfoRateSlider, lwLfoDepthSlider;
    juce::ComboBox lwScanModeCombo;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CmbAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<BtnAttach> lwEnableAttachment;
    std::unique_ptr<SldAttach> luxwaveVolumeAttachment, lwAmplitudeAttach,
                               lwAttackAttach, lwDecayAttach, lwSustainAttach, lwReleaseAttach,
                               lwFltCutoffAttach, lwFltDepthAttach,
                               lwLfoRateAttach, lwLfoDepthAttach;
    std::unique_ptr<CmbAttach> lwScanModeAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioWavePanel)
};
