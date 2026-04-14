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
 * Layout — four full-width zones stacked vertically:
 *   1. SlotGridComponent     (h=66,  fixed)  — 12 slot cells (sample bank)
 *   2. SlotEditorComponent   (h=210, fixed)  — edit panel:
 *        left  (~63 %) : REC/PLAY/CLEAR buttons + large timeline
 *        right (~37 %) : Speed / Loop / Resume controls
 *   3. SequencerComponent    (remaining)     — step-sequencer grid
 *      TransportBarComponent (h=44, bottom) — BPM / Steps / global controls
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

    Sp3ctraAudioProcessor& processor;

    SlotGridComponent     slotGrid;
    SlotEditorComponent   slotEditor;
    SequencerComponent    sequencer;
    TransportBarComponent transport;

    // ── Session toolbar ───────────────────────────────────────────────────────
    juce::TextButton newSessionBtn  { "NEW SESSION"  };
    juce::TextButton saveSessionBtn { "SAVE SESSION" };
    juce::TextButton loadSessionBtn { "LOAD SESSION" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    // ── Session helpers (Non-RT, message thread only) ─────────────────────────
    void doSaveSession(const juce::File& sessionFile);
    void doLoadSession(const juce::File& sessionFile);

    /** Path of the last loaded or saved session.
     *  When set, SAVE SESSION writes directly to this file (no file dialog). */
    juce::File currentSessionFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPageComponent)
};
