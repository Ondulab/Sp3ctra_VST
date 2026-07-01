#include "SequencerPageComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

SequencerPageComponent::SequencerPageComponent(Sp3ctraAudioProcessor& proc)
    : sequencer(proc),
      transport(proc)
{
    addAndMakeVisible(sequencer);
    addAndMakeVisible(transport);
}

void SequencerPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
}

void SequencerPageComponent::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    constexpr int pad    = Sp3ctraTheme::kPad;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int transH = 44;

    // Transport bar pinned to the bottom; sequencer grid fills the rest.
    const int transY = h - pad - transH;
    transport.setBounds(pad, transY, w - 2 * pad, transH);

    const int seqH = transY - gap - pad;
    sequencer.setBounds(pad, pad, w - 2 * pad, juce::jmax(80, seqH));
}
