#include "SequencerComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// ─── Helper ──────────────────────────────────────────────────────────────────
// Cycle a step through: EMPTY → every NON-EMPTY bank of THIS engine → LIVE.
// Empty banks are skipped so the user only ever lands on real recorded
// samples. Content is the ONLY filter — no isEnabled() gate: the engine
// enable (module presence × rack LED) is restored as-saved and can lag the
// visible rack state, which silently hid every recorded bank from this cycle.
static void cycleStep(FrameSequencer* seq, int stepIdx, int delta)
{
    if (!seq) return;
    if (stepIdx >= seq->getNumSteps()) return;

    juce::Array<int> options;
    options.add(FrameSequencer::STEP_EMPTY);
    if (LuxSampler* fs = seq->getSampler())
        for (int slot = 0; slot < LuxSamplerConstants::NUM_SLOTS; ++slot)
            if (fs->slotHasContent(slot))
                options.add(slot);
    options.add(FrameSequencer::STEP_LIVE);

    // A step may hold a value no longer offered (bank cleared since
    // assignment) — restart the cycle from EMPTY.
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
            cycleStep(processor.getFrameSequencer(samplerIndex_), i, delta);
            updateButton(i);
            // Steps persist in the SEQS tree (not APVTS) — mark dirty.
            processor.sessions()->markStateDirty();
        };
        addAndMakeVisible(stepBtns[i]);
    }
    startTimer(100); // 10 Hz — playing highlight
}

SequencerComponent::~SequencerComponent() { stopTimer(); }

void SequencerComponent::setSamplerIndex(int i)
{
    if (samplerIndex_ == i) return;
    samplerIndex_ = i;
    cachedNumSteps = -1;   // the new engine may expose a different step count
    resized();             // ...so re-derive the visible tiles right away
    for (int s = 0; s < kDisplaySteps; ++s)
        updateButton(s);
    repaint();
}

// =============================================================================
// updateButton — refresh colours, label, bankSlot and luxSampler pointer
// for a single cell. The thumbnail is drawn by StepCell::paintButton itself.
// =============================================================================

void SequencerComponent::updateButton(int i)
{
    if (i >= kDisplaySteps) return; // only first 16 are visible

    auto* seq = processor.getFrameSequencer(samplerIndex_);
    if (!seq) return;

    const int  nSteps   = seq->getNumSteps();
    const int  curStep  = seq->getCurrentStep();
    const bool isActive = (i < nSteps);

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
        // provides the main visual identity; the tag is just the bank number
        // — the grid is bound to ONE engine, no cross-engine addressing).
        bg    = isCurrent ? juce::Colour(0xff1a6a1a) : juce::Colour(0xff1e3028);
        txt   = isCurrent ? juce::Colours::white     : juce::Colour(0xff66cc88);
        label = juce::String(bank + 1);
    }

    stepBtns[i].setButtonText(label);
    stepBtns[i].setColour(juce::TextButton::buttonColourId,  bg);
    stepBtns[i].setColour(juce::TextButton::textColourOffId, txt);

    // Hand off the slot + the bound engine so paintButton can render the
    // spectral thumbnail. Sentinels keep a negative bankSlot (no thumbnail).
    stepBtns[i].bankSlot   = bank;
    stepBtns[i].luxSampler = (bank >= 0) ? seq->getSampler() : nullptr;
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
// resized — header + the ACTIVE steps only: the grid shows exactly
// SeqNumSteps tiles (1 row up to 8 steps, 2 rows beyond).
// =============================================================================

void SequencerComponent::resized()
{
    constexpr int gap     = 3;
    constexpr int headerH = 30; // 4 top + 22 title + 4 gap

    auto* seq = processor.getFrameSequencer(samplerIndex_);
    const int nSteps = juce::jlimit(1, kDisplaySteps,
                                    seq != nullptr ? seq->getNumSteps()
                                                   : kDisplaySteps);
    const int rows = (nSteps + kDisplayCols - 1) / kDisplayCols;

    const int w = getWidth();
    const int h = getHeight();

    // Square cells sized on the FULL 8-column budget (stable tile size while
    // the step count changes) and bounded by the height left under the
    // header, so the last row is never clipped on short windows.
    const int cellFromW = (w - 8 - (kDisplayCols - 1) * gap) / kDisplayCols;
    const int cellFromH = (h - headerH - 8 - (rows - 1) * gap) / rows;
    const int cell      = juce::jmax(12, juce::jmin(cellFromW, cellFromH));

    // Tiles flow from the LEFT margin (reads like a timeline: step 1 first).
    const int x0 = 4;

    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        if (i >= nSteps || i >= kDisplaySteps)
        {
            stepBtns[i].setVisible(false);
            continue;
        }
        const int col = i % kDisplayCols;
        const int row = i / kDisplayCols;
        stepBtns[i].setBounds(x0 + col * (cell + gap),
                              headerH + 4 + row * (cell + gap),
                              cell, cell);
        stepBtns[i].setVisible(true);
    }
}

// =============================================================================
// timerCallback — 10 Hz
// =============================================================================

void SequencerComponent::timerCallback()
{
    auto* seq = processor.getFrameSequencer(samplerIndex_);
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
