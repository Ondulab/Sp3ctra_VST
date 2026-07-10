#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../midi/MidiLearnAttachment.h"
#include <vector>

class Sp3ctraAudioProcessor;

// ─────────────────────────────────────────────────────────────────────────────
// IconTextButton — Borderless TextButton with a centred juce::Path icon.
// Path coordinates are normalised [0,1]; the icon is scaled to 60 % of the
// shortest button dimension and centred both horizontally and vertically.
// ─────────────────────────────────────────────────────────────────────────────
class IconTextButton : public juce::TextButton
{
public:
    IconTextButton() = default;
    explicit IconTextButton(const juce::String& t) : juce::TextButton(t) {}

    void setIconPath(const juce::Path& p) { iconPath = p; hasIcon = true; repaint(); }

    void paintButton(juce::Graphics& g, bool isHighlighted, bool isDown) override;

private:
    juce::Path iconPath;
    bool       hasIcon = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconTextButton)
};

// ─────────────────────────────────────────────────────────────────────────────
// TransportBarComponent
//
// Full-width bar at the bottom of the sampler page.
// APVTS-bound: seqBpm, seqLoop, seqDawSync.
// Play/hold/stop drive the seqTransport param (DAW-automatable/MIDI-mappable);
// the processor relays it to FrameSequencer. Steps combo writes seqNumSteps.
// 200 ms Timer disables BPM slider when DAW sync is active.
// ─────────────────────────────────────────────────────────────────────────────
class TransportBarComponent : public juce::Component,
                              private juce::Timer
{
public:
    explicit TransportBarComponent(Sp3ctraAudioProcessor& proc);
    ~TransportBarComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateTransportButtons();   ///< Refreshes play/hold/stop highlight state.
    void requestTransport(int mode); ///< Writes seqTransport (0=Stop 1=Play 2=Hold).

    Sp3ctraAudioProcessor& processor;

    // ── Sequencer play / hold / stop icons ───────────────────────────────────
    IconTextButton     seqPlayBtn;   // ▶ drawn as path
    IconTextButton     seqHoldBtn;   // ⏸ drawn as path (two vertical bars)
    IconTextButton     seqStopBtn;   // ■ drawn as path

    // ── BPM ───────────────────────────────────────────────────────────────────
    juce::Label        bpmLabel         { {}, "BPM" };
    juce::Slider       bpmSlider;

    // ── Steps ─────────────────────────────────────────────────────────────────
    juce::Label        stepsLabel       { {}, "Steps" };
    juce::ComboBox     stepsCombo;

    // ── Loop / DAW sync ───────────────────────────────────────────────────────
    juce::ToggleButton loopToggle       { "Loop" };
    juce::ToggleButton dawSyncToggle    { "DAW Sync" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dawSyncAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBarComponent)
};
