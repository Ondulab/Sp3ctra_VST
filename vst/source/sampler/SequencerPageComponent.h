#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SequencerComponent.h"
#include "TransportBarComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Standalone page for the step SEQUENCER module.
 *
 * Hosts the sequencer grid + its transport/config bar (BPM / Steps / Loop /
 * DAW Sync / Play-Hold-Stop). Extracted from the sampler page so the sequencer
 * is a first-class chain module addressing slots across samplers (A1..A12,
 * B1..B12, …).
 *
 * Layout — grid fills the page, transport bar (h=44) pinned to the bottom.
 */
class SequencerPageComponent : public juce::Component
{
public:
    explicit SequencerPageComponent(Sp3ctraAudioProcessor& proc);
    ~SequencerPageComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    SequencerComponent    sequencer;
    TransportBarComponent transport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerPageComponent)
};
