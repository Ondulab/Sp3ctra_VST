/**
 * @file EngineAudioPanels.h
 * @brief Audio-parameter panels for the ♪ engine blocks (M4 four-zone shell).
 *
 * These components host the EXACT controls that previously lived directly in
 * PluginEditor under the SYNTH tab's AUDIOSTRAL / AUDIOSYNTH / AUDIOWAVE
 * sub-pages (same APVTS parameter IDs, same attachments, same painted
 * labels).  Packaging them as components lets ZONE 3 host them per block:
 *
 *   ♪ LUXWAVE  block → AudioWavePanel (alone — a full 2-column module page)
 *
 * (LUXSTRAL's and LUXSYNTH's audio params live inside their module pages —
 *  LuxStralTabComponent / LuxSynthTabComponent are single 2-column pages, so
 *  they have no separate audio panel here. AudioWavePanel follows the same
 *  2-column layout: Volume strip + WAVETABLE on the left, identity chip +
 *  FILTER + LFO on the right.)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "EnvelopeEditorComponent.h"
#include "AudioPanelWidgets.h"   // AudioPanelLayout + AudioPanelUI (shared visual language)
#include <memory>
#include <vector>

//==============================================================================
// NOTE: AUDIOSTRAL was absorbed into LuxStralTabComponent and AUDIOSYNTH into
// LuxSynthTabComponent (both modules are single 2-column pages owning their
// own envelopes + knobs).

//==============================================================================
/**
 * LUXWAVE — the whole module UI on one page, laid out in 2 columns
 * (same visual language as LuxStralTabComponent):
 *
 *   ┌ Volume ═══════════════════┐   ┌ LUXWAVE -- OPTICAL WAVETABLE ┐
 *   │ ┌ WAVETABLE ───────────┐  │   │ ┌ FILTER ──────────────────┐ │
 *   │ │ Scan [Forward ▾]     │  │   │ │ LOWPASS -- ADSR MODULATED│ │
 *   │ │ AMPLITUDE ADSR       │  │   │ │  Cutoff / Env Depth      │ │
 *   │ │  [ env curve ]       │  │   │ └──────────────────────────┘ │
 *   │ │  Amplitude           │  │   │ ┌ LFO ─────────────────────┐ │
 *   │ └──────────────────────┘  │   │ │ VIBRATO -- PITCH MOD     │ │
 *   │                           │   │ │  Rate / Depth            │ │
 *   └───────────────────────────┘   └──────────────────────────────┘
 *
 * Signal flow: image line → WAVETABLE scan (amplitude ADSR voice) on the
 * left; per-voice LOWPASS + vibrato LFO modulation on the right.
 */
class AudioWavePanel : public juce::Component
{
public:
    explicit AudioWavePanel(Sp3ctraAudioProcessor& p);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // ── Vertical layout tokens (mirrors LuxStralTabComponent) ───────────────
    static constexpr int kTopPad    = 6;
    static constexpr int kColGap    = 16;                            // between columns
    static constexpr int kHeaderH   = 30;                            // Volume strip / chip
    static constexpr int kBadgeH    = Sp3ctraTheme::kSectionH;       // 24
    static constexpr int kBadgeGap  = Sp3ctraTheme::kSectionGap;     // 4
    static constexpr int kRowH      = Sp3ctraTheme::kControlH;       // 22
    static constexpr int kRowGap    = Sp3ctraTheme::kRowGap;         // 4
    static constexpr int kSecGapV   = 10;                            // between sections
    static constexpr int kSecPadB   = 8;                             // section bottom pad
    static constexpr int kSecInsetX = 8;                             // content inset
    static constexpr int kLabelW    = 96;                            // Volume label column
    static constexpr int kCapH      = AudioPanelLayout::kEnvCaptionH;// 13
    static constexpr int kToggleGap = AudioPanelLayout::kToggleGap;  // 6
    static constexpr int kKnobH     = AudioPanelLayout::kKnobCellH;  // 71
    static constexpr int kEnvH      = AudioPanelLayout::kEnvH;       // 124
    static constexpr int kEnvGap    = AudioPanelLayout::kEnvGap;     // 10

    // (The LuxWave OUT conditioning lives on the OUT/send page — P2.)
    static constexpr int kWaveSecH  = kBadgeH + kBadgeGap + kRowH + kToggleGap
                                    + kCapH + kEnvH + kEnvGap + kKnobH + kSecPadB;
    static constexpr int kFltSecH   = kBadgeH + kBadgeGap + kCapH + kKnobH + kSecPadB;
    static constexpr int kLfoSecH   = kFltSecH;

    static constexpr int kLeftColH  = kHeaderH + kSecGapV + kWaveSecH;
    static constexpr int kRightColH = kHeaderH + kSecGapV + kFltSecH + kSecGapV + kLfoSecH;

public:
    /** Natural content height — the taller of the two columns. */
    static constexpr int kPreferredH =
        kTopPad + (kLeftColH > kRightColH ? kLeftColH : kRightColH) + kSecPadB;

private:
    // ── Resolved layout (single source for paint + resized) ─────────────────
    struct Geom
    {
        int colW = 0, leftX = 0, rightX = 0;
        // left
        juce::Rectangle<int> volStrip, volLabel, volSlider;
        juce::Rectangle<int> waveBg, waveBadge, scanCaption, scanCombo, env;
        int waveCaptionY = 0, waveGridX = 0, waveGridW = 0, waveGridY = 0;
        // right
        juce::Rectangle<int> chip;
        juce::Rectangle<int> fltBg, fltBadge;
        int fltCaptionY = 0, fltGridX = 0, fltGridW = 0, fltGridY = 0;
        juce::Rectangle<int> lfoBg, lfoBadge;
        int lfoCaptionY = 0, lfoGridX = 0, lfoGridW = 0, lfoGridY = 0;
    };

    Geom computeGeom(int w) const;

    juce::Label  volumeLabel;
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
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioWavePanel)
};
