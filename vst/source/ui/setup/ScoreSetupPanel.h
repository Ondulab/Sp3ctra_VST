/**
 * @file ScoreSetupPanel.h
 * @brief SETUP face of the SCORE block (zone 3).
 *
 * Holds every NON-essential SCORE generation parameter (dynamic range, gamma,
 * contrast, HF boost, high-pass, page format, overlap, DPI and the processing
 * toggles). The essential controls (Load / Generate / Export / Writing Speed /
 * Play-Stop-Loop-Speed) stay on the PLAY page (ScoreGenTabComponent).
 *
 * Parameters are NOT host-automatable (offline export) — they live in the
 * processor's shared ScoreSettings (Sp3ctraAudioProcessor::getScoreSettings()),
 * edited here and read by the PLAY page at GENERATE time.
 *
 * FREQUENCY RANGE: by default the SCORE mirrors LuxStral's Tuning + Root Note +
 * Octaves (shown read-only / greyed). Tick "Manual" to override with a
 * SCORE-specific tuning/root/octaves (stored in the processor's
 * ScoreFreqOverride).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../PluginProcessor.h"

class ScoreSetupPanel : public juce::Component,
                        private juce::Timer
{
public:
    ScoreSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~ScoreSetupPanel() override;

    /** Natural content height (freq-range + dynamic-range + CIS-height rows). */
    static constexpr int kPreferredH = 400;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void initLabel(juce::Label& l, const juce::String& text);
    void initSlider(juce::Slider& s, double lo, double hi, double step, double val);
    void initToggle(juce::ToggleButton& t, const juce::String& text, bool on);
    void timerCallback() override;

    /** Refresh the freq-range controls + enabled state from the current mode. */
    void refreshFreqControls();
    /** Refresh the CIS-height control: greyed + locked to the sensor length
     *  unless the Manual override is enabled. */
    void refreshHeightControls();
    /** Compute & display the resulting Hz span for the current settings. */
    void updateRangeInfo();

    Sp3ctraAudioProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    ScoreSettings& settings;     // shared, owned by the processor

    // ── Frequency range (mirror LuxStral or manual override) ───────────────
    juce::Label        freqSectionLabel;
    juce::ToggleButton manualToggle;
    juce::Label        tuningLabel;
    juce::Slider       tuningSlider;
    juce::Label        rootLabel;
    juce::ComboBox     rootCombo;
    juce::Label        octavesLabel;
    juce::Slider       octavesSlider;
    juce::Label        rangeInfoLabel;

    // ── Image processing — PhonoPaper-conforming controls only ─────────────
    // Everything else (gamma, contrast, HF boost, high-pass, gate, dither,
    // overlap) is fixed to PhonoPaper-neutral values and no longer exposed.
    // Page Format / Printer DPI moved to the PLAY page (format options).
    juce::Label  dynLabel;
    juce::Slider dynSlider;

    // ── Print size — spectro band height = CIS sensor active length ────────
    // Locked to SCORE_CIS_HEIGHT_MM (219.456 mm) by default so a 100%-scale
    // print plays in tune; tick Manual to override (non-standard sensor/scaling).
    juce::Label        printSectionLabel;
    juce::ToggleButton heightManualToggle;
    juce::Label        heightLabel;
    juce::Slider       heightSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreSetupPanel)
};
