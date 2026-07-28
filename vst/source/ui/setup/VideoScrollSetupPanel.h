/**
 * @file VideoScrollSetupPanel.h
 * @brief SETUP face of a VIDEO SCROLL output block (zone 3), per instance.
 *
 * Hosts the per-instance background/frame colour — the colour painted where the
 * zoomed or rotated waterfall no longer covers the viewport (the border a
 * negative zoom leaves, especially visible once the image is inverted). Bound to
 * videoScroll{slot}_bgR / bgG / bgB (0..1 each, default white).
 *
 * Per-instance like VideoScrollPage: setSlot(slot) re-points the colour picker
 * at that instance's bank; slot < 0 unbinds.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"

class VideoScrollSetupPanel : public juce::Component,
                              private juce::ChangeListener
{
public:
    VideoScrollSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~VideoScrollSetupPanel() override;

    /** Natural content height (header + label + colour selector). */
    static constexpr int kPreferredH = 330;

    /** Bind the colour picker to the VideoScroll bank of `slot` (0..7), or
     *  unbind (slot < 0). Called by the editor when a VIDEO SCROLL block is
     *  selected and on session restore. */
    void setSlot(int slot);
    int  slot() const noexcept { return slot_; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    float readParam(const char* suffix, float def) const;
    void  writeParam(const char* suffix, float value);

    Sp3ctraAudioProcessor& processor_;
    juce::AudioProcessorValueTreeState& apvts_;
    juce::Colour accent_;
    int  slot_ { -1 };
    bool updating_ { false };   // guard: programmatic set must not write back

    juce::ColourSelector selector_ {
        juce::ColourSelector::showColourAtTop
      | juce::ColourSelector::showSliders
      | juce::ColourSelector::showColourspace };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollSetupPanel)
};
