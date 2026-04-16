#include "SequencerComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// ─── Helper ──────────────────────────────────────────────────────────────────
static void cycleStep(FrameSequencer* seq, int stepIdx, int delta)
{
    if (!seq) return;
    if (stepIdx >= seq->getNumSteps()) return;

    constexpr int kN         = LuxSamplerConstants::NUM_SLOTS; // 12
    constexpr int kCycleSize = kN + 2; // 14: empty + 12 banks + LIVE

    const int cur = seq->getStep(stepIdx);

    int pos;
    if (cur == FrameSequencer::STEP_LIVE)        pos = kCycleSize - 1;
    else if (cur == FrameSequencer::STEP_EMPTY)  pos = 0;
    else                                         pos = cur + 1;

    pos = ((pos + delta) % kCycleSize + kCycleSize) % kCycleSize;

    int newStep;
    if (pos == 0)                   newStep = FrameSequencer::STEP_EMPTY;
    else if (pos == kCycleSize - 1) newStep = FrameSequencer::STEP_LIVE;
    else                            newStep = pos - 1;

    seq->setStep(stepIdx, newStep);
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
    auto* fs  = processor.getLuxSampler();
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
        // provides the main visual identity; the number is just a tag)
        bg    = isCurrent ? juce::Colour(0xff1a6a1a) : juce::Colour(0xff1e3028);
        txt   = isCurrent ? juce::Colours::white     : juce::Colour(0xff66cc88);
        label = juce::String(bank + 1);
    }

    if (!seqActive) { bg = bg.withAlpha(0.4f); txt = txt.withAlpha(0.4f); }

    stepBtns[i].setButtonText(label);
    stepBtns[i].setColour(juce::TextButton::buttonColourId,  bg);
    stepBtns[i].setColour(juce::TextButton::textColourOffId, txt);

    // Hand off the bank index and LuxSampler pointer so paintButton
    // can render the spectral thumbnail without any extra bookkeeping.
    stepBtns[i].bankSlot     = bank; // >= 0 for normal banks; STEP_EMPTY / STEP_LIVE otherwise
    stepBtns[i].luxSampler = fs;
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
