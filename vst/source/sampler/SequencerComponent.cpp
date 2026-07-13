#include "SequencerComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// ─── Helper ──────────────────────────────────────────────────────────────────
// Cycle a step through: EMPTY → every NON-EMPTY bank of every sampler engine
// (all chains, in A1.., B1.. order) → LIVE. Empty banks are skipped so the
// user only ever lands on real recorded samples. Content is the ONLY filter —
// no isEnabled() gate: the engine enable (module presence × rack LED) is
// restored as-saved and can lag the visible rack state, which silently hid
// every recorded bank from this cycle.
static void cycleStep(FrameSequencer* seq, int stepIdx, int delta)
{
    if (!seq) return;
    if (stepIdx >= seq->getNumSteps()) return;

    juce::Array<int> options;
    options.add(FrameSequencer::STEP_EMPTY);
    for (int s = 0; s < seq->getNumSamplers(); ++s)
    {
        LuxSampler* fs = seq->getSampler(s);
        if (fs == nullptr) continue;
        for (int slot = 0; slot < LuxSamplerConstants::NUM_SLOTS; ++slot)
            if (fs->slotHasContent(slot))
                options.add(FrameSequencer::encodeStep(s, slot));
    }
    options.add(FrameSequencer::STEP_LIVE);

    // A step may hold a value no longer offered (bank cleared / module removed
    // from its chain since assignment) — restart the cycle from EMPTY.
    const int n   = options.size();
    int       pos = options.indexOf(seq->getStep(stepIdx));
    if (pos < 0) pos = 0;

    pos = ((pos + delta) % n + n) % n;
    seq->setStep(stepIdx, options[pos]);
}

// =============================================================================
// Constructor
// =============================================================================

SequencerComponent::SequencerComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        stepBtns[i].setButtonText("-");
        stepBtns[i].onStep = [this, i](int delta)
        {
            cycleStep(processor.getFrameSequencer(), i, delta);
            updateButton(i);
        };
        addAndMakeVisible(stepBtns[i]);
    }
    startTimer(100); // 10 Hz — playing highlight
}

SequencerComponent::~SequencerComponent() { stopTimer(); }

// =============================================================================
// updateButton — refresh colours, label, bankSlot and luxSampler pointer
// for a single cell. The thumbnail is drawn by StepCell::paintButton itself.
// =============================================================================

void SequencerComponent::updateButton(int i)
{
    if (i >= kDisplaySteps) return; // only first 16 are visible

    auto* seq = processor.getFrameSequencer();
    if (!seq) return;

    const int  nSteps    = seq->getNumSteps();
    const int  curStep   = seq->getCurrentStep();
    const bool seqActive = seq->isEnabled();
    const bool isActive  = (i < nSteps);

    if (!isActive)
    {
        stepBtns[i].setButtonText("");
        stepBtns[i].setColour(juce::TextButton::buttonColourId,
                               juce::Colour(0xff1e1e1e));
        stepBtns[i].setColour(juce::TextButton::textColourOffId,
                               juce::Colour(0xff333333));
        stepBtns[i].bankSlot     = FrameSequencer::STEP_EMPTY;
        stepBtns[i].luxSampler = nullptr;
        return;
    }

    const int  bank      = seq->getStep(i);
    const bool isCurrent = (i == curStep) && seq->isPlaying();

    juce::Colour bg, txt;
    juce::String label;

    if (bank == FrameSequencer::STEP_LIVE)
    {
        bg    = isCurrent ? juce::Colour(0xff1a5a5a) : juce::Colour(0xff1a3535);
        txt   = isCurrent ? juce::Colour(0xff00ffee) : juce::Colour(0xff44aaaa);
        label = "L";
    }
    else if (bank == FrameSequencer::STEP_EMPTY)
    {
        bg    = isCurrent ? juce::Colour(0xff3a2a2a) : juce::Colour(0xff2a2a2a);
        txt   = isCurrent ? juce::Colour(0xffff8888) : juce::Colour(0xff555555);
        label = "-";
    }
    else
    {
        // Normal bank: slightly brightened when current (the thumbnail
        // provides the main visual identity; the tag is just an A1..B12 label).
        bg    = isCurrent ? juce::Colour(0xff1a6a1a) : juce::Colour(0xff1e3028);
        txt   = isCurrent ? juce::Colours::white     : juce::Colour(0xff66cc88);
        const int samplerIdx = FrameSequencer::decodeSampler(bank); // 0=A, 1=B, …
        const int slot       = FrameSequencer::decodeSlot(bank);    // 0..11
        label = juce::String::charToString(
                    static_cast<juce::juce_wchar>('A' + samplerIdx))
                + juce::String(slot + 1);
    }

    if (!seqActive) { bg = bg.withAlpha(0.4f); txt = txt.withAlpha(0.4f); }

    stepBtns[i].setButtonText(label);
    stepBtns[i].setColour(juce::TextButton::buttonColourId,  bg);
    stepBtns[i].setColour(juce::TextButton::textColourOffId, txt);

    // Hand off the DECODED slot + its resolved engine so paintButton can render
    // the spectral thumbnail. Sentinels keep a negative bankSlot (no thumbnail).
    stepBtns[i].bankSlot   = FrameSequencer::decodeSlot(bank);
    stepBtns[i].luxSampler = (bank >= 0)
        ? seq->getSampler(FrameSequencer::decodeSampler(bank))
        : nullptr;
}

// =============================================================================
// paint — header only (thumbnails are painted by each StepCell::paintButton)
// =============================================================================

void SequencerComponent::paint(juce::Graphics& g)
{
    // Outer background
    g.setColour(juce::Colour(0xff1a2a1a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Header strip
    g.setColour(juce::Colour(0xff1e3e1e));
    g.fillRoundedRectangle(juce::Rectangle<float>(
        4.f, 4.f, (float)(getWidth() - 8), 22.f), 3.f);

    g.setColour(juce::Colour(0xff66cc88));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("STEP SEQUENCER",
               juce::Rectangle<int>(8, 4, getWidth() - 16, 22),
               juce::Justification::centredLeft, false);
}

// =============================================================================
// resized — header + 2 rows of square cells
// =============================================================================

void SequencerComponent::resized()
{
    constexpr int cols    = kDisplayCols; // 8
    constexpr int gap     = 3;
    constexpr int headerH = 30; // 4 top + 22 title + 4 gap

    const int w = getWidth();

    // Square cells
    const int cellW = (w - 8 - (cols - 1) * gap) / cols;
    const int cellH = cellW;

    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        if (i >= kDisplaySteps)
        {
            stepBtns[i].setVisible(false);
            continue;
        }
        const int col = i % cols;
        const int row = i / cols;
        stepBtns[i].setBounds(4 + col * (cellW + gap),
                              headerH + 4 + row * (cellH + gap),
                              cellW, cellH);
        stepBtns[i].setVisible(true);
    }
}

// =============================================================================
// timerCallback — 10 Hz
// =============================================================================

void SequencerComponent::timerCallback()
{
    auto* seq = processor.getFrameSequencer();
    if (!seq) return;

    const int nSteps = seq->getNumSteps();
    if (nSteps != cachedNumSteps)
    {
        cachedNumSteps = nSteps;
        resized();
    }

    for (int i = 0; i < kDisplaySteps; ++i)
        updateButton(i);
}
