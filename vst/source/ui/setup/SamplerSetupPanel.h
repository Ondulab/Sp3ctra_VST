/**
 * @file SamplerSetupPanel.h
 * @brief SETUP face of the SAMPLER block (zone 3, M5).
 *
 * The whole former gear-wheel LuxSamplerSettingsTab component, moved and
 * re-parented. Play params are PER-ENGINE (fsEngineParam: engine A keeps the
 * legacy "luxSampler*" ids, engine B owns "luxSamplerB*"); setSamplerIndex()
 * rebinds every engine-scoped attachment:
 *   - Banks (1-6, default 4)             (luxSampler[B]NumBanks)
 *   - Max Duration (1..60 s)             (luxSampler[B]MaxDuration)
 * (MIDI Channel / Octave Offset removed 2026-07-13 with the note-triggered
 *  play path — banks are no longer note-addressed. REC / PLAY / SAVE
 *  triggering lives in the unified right-click MIDI-Learn.)
 * Shared (not per-engine):
 *   - Image export toggle + format       (luxSamplerExportImages/-Format)
 *   - Bank status grid (state, duration, clear) — shows the first N banks,
 *     refreshed at 10 Hz
 * (The Output Directory control was retired with the project-session model:
 *  slot saves/exports land in the working session's exports/ folder.)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class SamplerSetupPanel final : public juce::Component,
                                private juce::Timer
{
public:
    SamplerSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~SamplerSetupPanel() override;

    /** Natural content height (header + 7 control rows + 12-slot grid).
     *  (MIDI Channel + Octave Offset rows replaced by the single Banks row —
     *  one kRowStep less than the previous 8-row layout.) */
    static constexpr int kPreferredH = 594;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Bind this panel to sampler engine 0 (A) or 1 (B) — rebinds every
     *  engine-scoped attachment (MIDI channel, octave, duration, bindings)
     *  to that engine's APVTS bank and cancels any pending MIDI-learn. */
    void setSamplerIndex(int i);

private:
    // Timer — refreshes slot display at 10 Hz
    void timerCallback() override;
    void updateSlotDisplays();

    // ─────────────────────────────────────────────────────────────────────────
    Sp3ctraAudioProcessor&              audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour                        accent;
    int                                 samplerIndex_ = 0;  // 0 = engine A, 1 = engine B

    // Number of banks shown in the SAMPLER page (1..8)
    juce::Label    banksLabel;
    juce::ComboBox banksCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> banksAttachment;

    // Max Duration
    juce::Label  maxDurationLabel;
    juce::Slider maxDurationSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxDurationAttachment;

    // REC button mode (Toggle / Momentary) — per engine
    juce::Label    recModeLabel;
    juce::ComboBox recModeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> recModeAttachment;

    // PLAY button mode (Toggle / Momentary) — per engine
    juce::Label    playModeLabel;
    juce::ComboBox playModeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> playModeAttachment;

    // Image export on slot SAVE
    juce::Label        exportImagesLabel;
    juce::ToggleButton exportImagesToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> exportImagesAttachment;

    juce::Label    exportFormatLabel;
    juce::ComboBox exportFormatCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> exportFormatAttachment;

    // Per-slot displays (12 rows)
    static constexpr int NUM_SLOTS = 12;
    juce::Label slotIndexLabel [NUM_SLOTS];
    juce::Label slotStateLabel [NUM_SLOTS];
    juce::Label slotDurLabel   [NUM_SLOTS];
    juce::TextButton slotClearBtn[NUM_SLOTS];

    /** (Re)create every engine-scoped attachment against the bank of
     *  samplerIndex_ (fsEngineParam). */
    void rebindEngineParams();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerSetupPanel)
};
