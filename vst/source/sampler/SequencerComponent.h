#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesequencer/FrameSequencer.h"
#include "../framesampler/FrameSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Step sequencer grid for the FrameSampler sampler page.
 *
 * Displays up to FrameSequencer::MAX_STEPS cells in 2 rows of 16.
 * Each cell shows the assigned bank (1..12) or '-' for empty/passthrough.
 * The currently playing step is highlighted.
 * Click → cycle bank assignment (empty → 1 → 2 → ... → 12 → empty).
 *
 * Internal 200 ms Timer refreshes display and handles numSteps changes.
 */
class SequencerComponent : public juce::Component,
                           private juce::Timer
{
public:
    explicit SequencerComponent(Sp3ctraAudioProcessor& proc);
    ~SequencerComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    Sp3ctraAudioProcessor& processor;
    juce::TextButton stepBtns[FrameSequencer::MAX_STEPS];
    int cachedNumSteps = -1; // detect step count changes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerComponent)
};
