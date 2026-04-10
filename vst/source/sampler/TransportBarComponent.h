#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class Sp3ctraAudioProcessor;

/**
 * @brief Transport control bar for the FrameSampler sampler page.
 *
 * APVTS-bound controls: frameSamplerEnabled, seqEnabled, seqBpm, seqLoop, seqDawSync.
 * Manual controls: play/stop buttons (FrameSequencer::uiPlay/uiStop),
 *                  steps combo box (seqNumSteps param + FrameSequencer::setNumSteps).
 * 200 ms Timer disables BPM slider when DAW sync is active.
 */
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

    Sp3ctraAudioProcessor& processor;

    juce::ToggleButton fsEnabledToggle  { "Sampler" };
    juce::ToggleButton seqEnabledToggle { "Seq" };
    juce::TextButton   seqPlayBtn       { ">" };
    juce::TextButton   seqStopBtn       { "[ ]" };
    juce::Label        bpmLabel         { {}, "BPM" };
    juce::Slider       bpmSlider;
    juce::Label        stepsLabel       { {}, "Steps" };
    juce::ComboBox     stepsCombo;
    juce::ToggleButton loopToggle       { "Loop" };
    juce::ToggleButton dawSyncToggle    { "DAW" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> fsEnabledAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> seqEnabledAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dawSyncAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBarComponent)
};
