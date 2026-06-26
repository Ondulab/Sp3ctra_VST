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
 *   ♪ LUXSYNTH block → LuxSynthTabComponent + AudioSynthPanel
 *   ♪ LUXWAVE  block → AudioWavePanel (alone)
 *
 * (LUXSTRAL's audio params live inside LuxStralTabComponent — that module is a
 *  single 2-column page, so it has no separate audio panel here.)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "EnvelopeEditorComponent.h"
#include "AudioPanelWidgets.h"   // AudioPanelLayout + AudioPanelUI (shared visual language)
#include <memory>

//==============================================================================
// NOTE: AUDIOSTRAL was absorbed into LuxStralTabComponent (the LUXSTRAL module is
// now a single 2-column page owning its own A/R envelope + Stereo/Sum/Gate knobs).

//==============================================================================
/** AUDIOSYNTH — LuxSynth volume ADSR + spectral (left), filter ADSR + LFO (right). */
class AudioSynthPanel : public juce::Component
{
public:
    explicit AudioSynthPanel(Sp3ctraAudioProcessor& p);

    /** Natural content height: badge + (VOLUME|FILTER ADSR editors) + 6-knob grid.
     *  (Enable strip removed — power lives in the rack LED + zone-3 header.) */
    static constexpr int kPreferredH = Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + AudioPanelLayout::kEnvCaptionH + AudioPanelLayout::kEnvH
                                     + AudioPanelLayout::kEnvGap
                                     + AudioPanelLayout::gridH(6)
                                     + AudioPanelLayout::kBottomPad;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::Slider luxsynthVolumeSlider;
    juce::Slider lxFltCutoffSlider, lxFltDepthSlider;
    juce::Slider lxNumOscSlider;
    juce::Slider lxLfoRateSlider, lxLfoDepthSlider;

    // Volume ADSR + Filter ADSR rendered as draggable envelope editors.
    std::unique_ptr<EnvelopeEditorComponent> volEnv, fltEnv;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SldAttach> luxsynthVolumeAttachment,
                               lxFltCutoffAttach, lxFltDepthAttach,
                               lxNumOscAttach, lxLfoRateAttach, lxLfoDepthAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSynthPanel)
};

//==============================================================================
/** AUDIOWAVE — LuxWave wavetable ADSR + scan (left), filter + LFO (right). */
class AudioWavePanel : public juce::Component
{
public:
    explicit AudioWavePanel(Sp3ctraAudioProcessor& p);

    /** Natural content height: badge + enable/scan strip + VOLUME ADSR editor + 6-knob grid. */
    static constexpr int kPreferredH = Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
                                     + AudioPanelLayout::kToggleStripH + AudioPanelLayout::kToggleGap
                                     + AudioPanelLayout::kEnvCaptionH + AudioPanelLayout::kEnvH
                                     + AudioPanelLayout::kEnvGap
                                     + AudioPanelLayout::gridH(6)
                                     + AudioPanelLayout::kBottomPad;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::Slider luxwaveVolumeSlider, lwAmplitudeSlider;
    juce::Slider lwFltCutoffSlider, lwFltDepthSlider;
    juce::Slider lwLfoRateSlider, lwLfoDepthSlider;
    juce::ComboBox lwScanModeCombo;

    // Wavetable amplitude ADSR rendered as a draggable envelope editor.
    std::unique_ptr<EnvelopeEditorComponent> volEnv;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CmbAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SldAttach> luxwaveVolumeAttachment, lwAmplitudeAttach,
                               lwFltCutoffAttach, lwFltDepthAttach,
                               lwLfoRateAttach, lwLfoDepthAttach;
    std::unique_ptr<CmbAttach> lwScanModeAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioWavePanel)
};
