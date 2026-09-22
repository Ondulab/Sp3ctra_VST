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

    // ── Zone 1: sample bank ───────────────────────────────────────────────────
    slotGrid.setBounds(pad, pad, w - 2 * pad, gridH);

    // ── Zone 2: slot editor ───────────────────────────────────────────────────
    // kEditH is the MINIMUM (what kPreferredH guarantees at min window size);
    // any height the zone-3 viewport gives beyond the fixed rows goes to the
    // editor, whose own layout hands all of it to the spectral image (param
    // rows and the EQ panel are fixed-height).
    const int transH = TransportBarComponent::requiredHeight(w - 2 * pad);
    const int editY  = pad + gridH + gap;
    const int fixedBelow = gap + kSeqH + gap + transH + pad;
    const int editH  = juce::jmax((int) kEditH, getHeight() - editY - fixedBelow);
    slotEditor.setBounds(pad, editY, w - 2 * pad, editH);

    // ── Step sequencer (internal to this engine): grid + transport bar ────────
    const int seqY   = editY + editH + gap;
    sequencer.setBounds(pad, seqY, w - 2 * pad, kSeqH);
    seqTransport.setBounds(pad, seqY + kSeqH + gap, w - 2 * pad, transH);
}
