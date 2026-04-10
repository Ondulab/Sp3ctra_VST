#include "SamplerPageComponent.h"
#include "../PluginProcessor.h"

SamplerPageComponent::SamplerPageComponent(Sp3ctraAudioProcessor& proc)
    : slotGrid  (proc),
      slotEditor(proc),
      sequencer (proc),
      transport (proc)
{
    slotGrid.onSlotSelected = [this](int idx) { onSlotSelected(idx); };
    slotGrid  .setSelectedSlot(0);
    slotEditor.setSelectedSlot(0);

    addAndMakeVisible(slotGrid);
    addAndMakeVisible(slotEditor);
    addAndMakeVisible(sequencer);
    addAndMakeVisible(transport);
}

SamplerPageComponent::~SamplerPageComponent() = default;

void SamplerPageComponent::onSlotSelected(int idx)
{
    slotGrid  .setSelectedSlot(idx);
    slotEditor.setSelectedSlot(idx);
}

void SamplerPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
}

void SamplerPageComponent::resized()
{
    const int w       = getWidth();
    const int h       = getHeight();
    const int pad     = 4;
    const int gap     = 6;
    const int gridH   = 66;
    const int transH  = 44;

    // Middle section: fill remaining space
    const int middleH = h - gridH - transH - 3 * gap - 2 * pad;
    const int editorW = (w - 2 * pad - gap) * 4 / 10;
    const int seqW    = w - 2 * pad - gap - editorW;

    slotGrid .setBounds(pad, pad, w - 2*pad, gridH);

    const int midY = pad + gridH + gap;
    slotEditor.setBounds(pad,              midY, editorW, middleH);
    sequencer .setBounds(pad + editorW + gap, midY, seqW,    middleH);

    const int transY = midY + middleH + gap;
    transport .setBounds(pad, transY, w - 2*pad, transH);
}
