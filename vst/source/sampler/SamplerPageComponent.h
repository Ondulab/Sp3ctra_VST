#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SlotGridComponent.h"
#include "SlotEditorComponent.h"
#include "SequencerComponent.h"
#include "TransportBarComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Master container for the FrameSampler sampler page.
 *
 * Layout (top → bottom):
 *   1. SlotGridComponent     (h=66)  — 12 horizontal slot cells (MIDI C1..B1)
 *   2. SlotEditorComponent   (40% W) — edit panel for the selected slot
 *      SequencerComponent    (60% W) — step sequencer grid  (side by side)
 *   3. TransportBarComponent (h=44)  — transport + global controls
 *
 * Manages selectedSlot state shared between SlotGrid and SlotEditor.
 */
class SamplerPageComponent : public juce::Component
{
public:
    explicit SamplerPageComponent(Sp3ctraAudioProcessor& proc);
    ~SamplerPageComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void onSlotSelected(int idx);

    SlotGridComponent     slotGrid;
    SlotEditorComponent   slotEditor;
    SequencerComponent    sequencer;
    TransportBarComponent transport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPageComponent)
};
