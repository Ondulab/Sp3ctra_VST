#include "SequencerComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include <cmath>

// ─── Helper ──────────────────────────────────────────────────────────────────
static void cycleStep(FrameSequencer* seq, int stepIdx, int delta)
{
    if (!seq) return;
    if (stepIdx >= seq->getNumSteps()) return;

    constexpr int kN         = FrameSamplerConstants::NUM_SLOTS; // 12
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
    startTimer(100); // 10 Hz — playing highlight + mini-timeline repaint
}

SequencerComponent::~SequencerComponent() { stopTimer(); }

// =============================================================================
// updateButton — refresh label + colours for a single cell (no border)
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
        bg    = isCurrent ? juce::Colour(0xff1a6a1a) : juce::Colour(0xff1e3028);
        txt   = isCurrent ? juce::Colours::white     : juce::Colour(0xff66cc88);
        label = juce::String(bank + 1);
    }

    if (!seqActive) { bg = bg.withAlpha(0.4f); txt = txt.withAlpha(0.4f); }

    stepBtns[i].setButtonText(label);
    stepBtns[i].setColour(juce::TextButton::buttonColourId,  bg);
    stepBtns[i].setColour(juce::TextButton::textColourOffId, txt);
}

// =============================================================================
// paintMiniTimeline — spectral view of selected slot (non-RT safe)
// =============================================================================

void SequencerComponent::paintMiniTimeline(juce::Graphics& g) const
{
    const int tlX = 4;
    const int tlY = 30;
    const int tlW = getWidth() - 8;
    const int tlH = cachedTimelineH_;

    if (tlW <= 0 || tlH <= 0) return;

    auto* fs = processor.getFrameSampler();
    const int slot = getSelectedSlot ? getSelectedSlot() : 0;

    // ── Background ────────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff070710));
    g.fillRoundedRectangle(juce::Rectangle<float>((float)tlX, (float)tlY,
                                                   (float)tlW, (float)tlH), 3.0f);

    if (!fs || !fs->slotHasContent(slot))
    {
        // No content: just a label
        g.setColour(juce::Colour(0xff2a2a3a));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("--", juce::Rectangle<int>(tlX, tlY, tlW, tlH),
                   juce::Justification::centred, false);
        return;
    }

    // ── Spectral waveform ─────────────────────────────────────────────────────
    constexpr int kMaxCols = 512;
    const int numCols = juce::jmin(tlW, kMaxCols);
    float bassArr  [kMaxCols] {};
    float trebleArr[kMaxCols] {};
    fs->sampleSpectralForTimeline(slot, bassArr, trebleArr, numCols);

    const int cy = tlY + tlH / 2;

    for (int col = 0; col < tlW; ++col)
    {
        const int si = juce::jlimit(0, numCols - 1, col * numCols / juce::jmax(1, tlW));
        const float bv = std::pow(bassArr  [si], 0.4f);
        const float tv = std::pow(trebleArr[si], 0.4f);

        // Bass: downward bar from centre
        const int bH = juce::jmax(1, (int)(bv * (float)(tlH / 2)));
        g.setColour(juce::Colour(0xff004466).brighter(bv * 1.4f));
        g.fillRect(tlX + col, cy, 1, bH);

        // Treble: upward bar from centre
        const int tH = juce::jmax(1, (int)(tv * (float)(tlH / 2)));
        g.setColour(juce::Colour(0xff006655).brighter(tv * 1.4f));
        g.fillRect(tlX + col, cy - tH, 1, tH);
    }

    // Centre axis
    g.setColour(juce::Colour(0x22ffffff));
    g.fillRect(tlX, cy, tlW, 1);

    // ── Active zone: dim + start/end markers ──────────────────────────────────
    const float sf = fs->getSlotStartFrac(slot);
    const float ef = fs->getSlotEndFrac(slot);
    const int   sx = tlX + (int)(sf * (float)tlW);
    const int   ex = tlX + (int)(ef * (float)tlW);

    // Dim before start
    if (sx > tlX)
    {
        g.setColour(juce::Colour(0xaa050508));
        g.fillRect(tlX, tlY, sx - tlX, tlH);
    }
    // Dim after end
    if (ex < tlX + tlW - 1)
    {
        g.setColour(juce::Colour(0xaa050508));
        g.fillRect(ex + 2, tlY, (tlX + tlW) - (ex + 2), tlH);
    }

    // Start marker (green)
    g.setColour(juce::Colour(0xff44ee88));
    g.fillRect(sx, tlY, 2, tlH);

    // End marker (orange)
    g.setColour(juce::Colour(0xffff5533));
    g.fillRect(ex, tlY, 2, tlH);

    // ── Playhead ──────────────────────────────────────────────────────────────
    if (fs->getSlotState(slot) == SlotState::PLAYING)
    {
        const int fc = fs->getSlotFrameCount(slot);
        if (fc > 0)
        {
            const float frac = juce::jlimit(0.0f, 1.0f,
                (float)fs->getSlotPlayHead(slot) / (float)fc);
            const int phX = tlX + (int)(frac * (float)tlW);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.fillRect(phX, tlY, 1, tlH);
        }
    }

    // ── Subtle border ─────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff334455));
    g.drawRoundedRectangle(juce::Rectangle<int>(tlX, tlY, tlW, tlH)
                               .toFloat().reduced(0.5f), 3.0f, 0.5f);
}

// =============================================================================
// paint
// =============================================================================

void SequencerComponent::paint(juce::Graphics& g)
{
    // ── Outer background ──────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff1a2a1a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // ── Header strip ──────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff1e3e1e));
    g.fillRoundedRectangle(juce::Rectangle<float>(
        4.f, 4.f, (float)(getWidth() - 8), 22.f), 3.f);

    g.setColour(juce::Colour(0xff66cc88));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("STEP SEQUENCER",
               juce::Rectangle<int>(8, 4, getWidth() - 16, 22),
               juce::Justification::centredLeft, false);
    // Help text removed ("L=+bank R=-bank ...")

    // ── Mini-timeline: selected slot spectral view ────────────────────────────
    paintMiniTimeline(g);
}

// =============================================================================
// resized — 8 columns × 2 rows, square cells
// =============================================================================

void SequencerComponent::resized()
{
    constexpr int cols    = kDisplayCols;  // 8
    constexpr int rows    = kDisplayRows;  // 2
    constexpr int gap     = 3;
    constexpr int headerH = 30; // 4 top + 22 title + 4 gap

    const int w = getWidth();
    const int h = getHeight();

    // Square cells: width determines height
    const int cellW   = (w - 8 - (cols - 1) * gap) / cols;
    const int cellH   = cellW; // square
    cachedCellH_      = cellH;

    const int cellsH  = rows * cellH + (rows - 1) * gap;

    // Timeline fills the vertical space between header and cells (min 40 px)
    const int availTl      = h - headerH - cellsH - 4; // 4 px bottom padding
    cachedTimelineH_   = juce::jmax(40, availTl);
    cachedCellsStartY_ = headerH + cachedTimelineH_ + 4;

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
                              cachedCellsStartY_ + row * (cellH + gap),
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

    // Refresh only the visible 16 step cells
    for (int i = 0; i < kDisplaySteps; ++i)
        updateButton(i);

    // Repaint also covers the mini-timeline (playhead animation)
    repaint();
}
