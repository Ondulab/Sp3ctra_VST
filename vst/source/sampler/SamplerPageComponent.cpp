#include "SamplerPageComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// (The .sp3s "session" file format and its NEW/SAVE/LOAD toolbar were retired
// with the project-session model: sample banks now persist through the
// SessionManager — sidecar banks/engineN.fsmp in the working session folder,
// or embedded SAMPLER_BANKS in the DAW state blob. Legacy .sp3s files are
// absorbed once at restore time by session/Sp3sImporter.)

// =============================================================================
// Constructor
// =============================================================================

SamplerPageComponent::SamplerPageComponent(Sp3ctraAudioProcessor& proc)
    : slotGrid  (proc),
      slotEditor(proc),
      sequencer (proc),
      seqTransport(proc)
{
    slotGrid.onSlotSelected = [this](int idx) { onSlotSelected(idx); };
    // Reopen on the bank the user was editing (the processor mirrors the last
    // selection; the editor restores it across launches via "selSamplerBank").
    const int sel = juce::jlimit(0, LuxSamplerConstants::NUM_SLOTS - 1,
                                 proc.getSamplerSelectedSlot());
    slotGrid  .setSelectedSlot(sel);
    slotEditor.setSelectedSlot(sel);

    addAndMakeVisible(slotGrid);
    addAndMakeVisible(slotEditor);
    addAndMakeVisible(sequencer);
    addAndMakeVisible(seqTransport);
}

SamplerPageComponent::~SamplerPageComponent() = default;

// =============================================================================
// Slot selection
// =============================================================================

void SamplerPageComponent::onSlotSelected(int idx)
{
    slotGrid  .setSelectedSlot(idx);
    slotEditor.setSelectedSlot(idx);
}

// =============================================================================
// paint / resized
// =============================================================================

void SamplerPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
}

void SamplerPageComponent::resized()
{
    const int w      = getWidth();
    constexpr int pad    = Sp3ctraTheme::kPad;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int gridH  = kGridH;
    // Editor: 2 param columns + image editor + a SCORE-style EQ panel underneath.
    constexpr int editH  = kEditH;

    // ── Zone 1: sample bank ───────────────────────────────────────────────────
    slotGrid.setBounds(pad, pad, w - 2 * pad, gridH);

    // ── Zone 2: slot editor ───────────────────────────────────────────────────
    const int editY = pad + gridH + gap;
    slotEditor.setBounds(pad, editY, w - 2 * pad, editH);

    // ── Step sequencer (internal to this engine): grid + transport bar ────────
    const int seqY   = editY + editH + gap;
    sequencer.setBounds(pad, seqY, w - 2 * pad, kSeqH);
    const int transH = TransportBarComponent::requiredHeight(w - 2 * pad);
    seqTransport.setBounds(pad, seqY + kSeqH + gap, w - 2 * pad, transH);
}
