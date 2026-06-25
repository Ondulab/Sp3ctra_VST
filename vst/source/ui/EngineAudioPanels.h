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
#include "EnvelopeEditorComponent.h"
#include <memory>

//==============================================================================
/**
 * Layout tokens for the rotary-knob grids of the audio panels.
 * Sections are stacked full-width; continuous params are knobs laid out in a
 * fixed-column grid, on/off params live in a toggle strip under the badge.
 * Used by both kPreferredH (here) and the paint/resized math (.cpp).
 */
namespace AudioPanelLayout
{
    constexpr int kKnobCols    = 4;                       ///< knobs per grid row
    constexpr int kKnobArea    = 44;                      ///< rotary draw square
    constexpr int kKnobValH    = 14;                      ///< value text-box height
    constexpr int kKnobLblH    = 13;                      ///< name label height
    constexpr int kKnobCellH   = kKnobArea + kKnobValH + kKnobLblH; ///< 71
    constexpr int kKnobGapX    = 6;
    constexpr int kKnobGapY    = 6;
    constexpr int kKnobRowStep = kKnobCellH + kKnobGapY;  ///< 77

    constexpr int kToggleStripH = Sp3ctraTheme::kControlH; ///< 22
    constexpr int kToggleGap    = 6;                       ///< below toggle strip
    constexpr int kToggleW      = 160;                     ///< single toggle width
    constexpr int kSecGapV      = 10;                      ///< between stacked sections
    constexpr int kBottomPad    = 8;

    // Envelope-editor blocks (audio ADSR rendered as draggable curve, not knobs).
    constexpr int kEnvCaptionH  = 13;                      ///< caption strip above an editor
    constexpr int kEnvGap       = 10;                      ///< below the editor row
    constexpr int kEnvH         = EnvelopeEditorComponent::kPreferredH; ///< 124

    /// Grid rows needed to host n knobs.
    constexpr int rows(int n)  { return (n + kKnobCols - 1) / kKnobCols; }
    /// Pixel height of an n-knob grid (no trailing row gap).
    constexpr int gridH(int n) { return rows(n) * kKnobRowStep - kKnobGapY; }
    /// Full height of one section: badge + gap + optional toggle strip + grid.
    constexpr int sectionH(int nKnobs, bool hasToggles)
    {
        return Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
             + (hasToggles ? kToggleStripH + kToggleGap : 0)
             + gridH(nKnobs);
    }
}

//==============================================================================
/** AUDIOSTRAL — LuxStral audio params + StrokeForge controls (stacked). */
class AudioStralPanel : public juce::Component
{
public:
    explicit AudioStralPanel(Sp3ctraAudioProcessor& p);

    /** Natural content height: AUDIOSTRAL (toggles + AR editor + 4 knobs) over STROKEFORGE (2 toggles + 3 knobs). */
    static constexpr int kPreferredH =
          Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap
        + AudioPanelLayout::kToggleStripH + AudioPanelLayout::kToggleGap
        + AudioPanelLayout::kEnvCaptionH + AudioPanelLayout::kEnvH + AudioPanelLayout::kEnvGap
        + AudioPanelLayout::gridH(4)
        + AudioPanelLayout::kSecGapV
        + AudioPanelLayout::sectionH(3, true)
        + AudioPanelLayout::kBottomPad;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::ToggleButton stereoEnableToggle;
    juce::Slider luxstralVolumeSlider, stereoTempSlider, sumExpSlider, noiseGateSlider;

    // Attack/Release rendered as a draggable AR envelope editor.
    std::unique_ptr<EnvelopeEditorComponent> arEnv;

    juce::ToggleButton sfEnabledToggle, sfFocusOnlyToggle;
    juce::Slider sfMorphWidthSlider, sfFocusSigmaSlider, sfSpectralThreshSlider;

    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<BtnAttach> stereoEnableAttachment,
                               sfEnabledAttachment, sfFocusOnlyAttachment;
    std::unique_ptr<SldAttach> luxstralVolumeAttachment, stereoTempAttachment,
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
