/**
 * @file CentroSetupPanel.h
 * @brief SETUP face of the CENTROID block (zone 3).
 *
 * Hosts the WIDTH LAW selector (PX / ERB) — a structural choice about how
 * the redrawn line width reads along the frequency axis, so it lives here
 * rather than on the PLAY page — with its explanation printed underneath.
 * setSlot(slot) rebinds the attachment to the selected instance's bank
 * (luxcentro{slot}_*), same pattern as PitchSetupPanel.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class CentroSetupPanel : public juce::Component
{
public:
    CentroSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~CentroSetupPanel() override;

    /** Bind the control to the CENTROID bank of `slot` (0..7) — the selected
     *  instance's parameters. */
    void setSlot(int slot);
    int  slot() const noexcept { return slot_; }

    /** Natural content height (header + 1 row + explanation block). */
    static constexpr int kPreferredH = 210;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ = 0;   // pool slot of the bound instance

    // Width law (PX / ERB)
    juce::Label    lawLabel;
    juce::ComboBox lawCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lawAttachment;

    juce::Rectangle<int> helpArea_;   // explanation block (painted)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CentroSetupPanel)
};
