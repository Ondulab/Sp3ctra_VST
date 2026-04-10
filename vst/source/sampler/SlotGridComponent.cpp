#include "SlotGridComponent.h"
#include "../PluginProcessor.h"

const char* const SlotGridComponent::kNoteNames[FrameSamplerConstants::NUM_SLOTS] = {
    "C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1"
};

SlotGridComponent::SlotGridComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    startTimer(100); // 10 Hz — drives blink + repaint
}

SlotGridComponent::~SlotGridComponent()
{
    stopTimer();
}

void SlotGridComponent::setSelectedSlot(int idx) noexcept
{
    const int clamped = juce::jlimit(0, FrameSamplerConstants::NUM_SLOTS - 1, idx);
    if (clamped != selectedSlot)
    {
        selectedSlot = clamped;
        repaint();
    }
}

juce::Rectangle<int> SlotGridComponent::cellBounds(int i) const noexcept
{
    constexpr int kGap    = 3;
    constexpr int kUnderH = 4; // reserved for sequencer underline
    const int totalGaps   = (FrameSamplerConstants::NUM_SLOTS - 1) * kGap;
    const int cellW       = (getWidth() - totalGaps) / FrameSamplerConstants::NUM_SLOTS;
    const int cellH       = getHeight() - kUnderH;
    return { i * (cellW + kGap), 0, cellW, cellH };
}

void SlotGridComponent::paint(juce::Graphics& g)
{
    auto* fs  = processor.getFrameSampler();
    auto* seq = processor.getFrameSequencer();

    // Determine which bank is currently sequencer-active
    int activeSeqBank = -1;
    if (seq != nullptr && seq->isPlaying())
    {
        const int step = seq->getCurrentStep();
        if (step >= 0)
            activeSeqBank = seq->getStep(step);
    }
    const int activePlaySlot = (fs != nullptr) ? fs->getActivePlaySlot() : -1;

    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
    {
        const auto  cell       = cellBounds(i);
        const auto  st         = (fs != nullptr) ? fs->getSlotState(i) : SlotState::IDLE;
        const bool  hasContent = (fs != nullptr) && fs->slotHasContent(i);
        const bool  isSelected = (i == selectedSlot);
        const bool  isSeqActive = (i == activeSeqBank) || (i == activePlaySlot);

        // ── Background colour per state ───────────────────────────────────────
        juce::Colour bgCol, textCol;
        switch (st)
        {
            case SlotState::RECORDING:
                bgCol   = blinkOn ? juce::Colour(0xffcc2222) : juce::Colour(0xff7a1010);
                textCol = juce::Colours::white;
                break;
            case SlotState::ARMED:
                bgCol   = blinkOn ? juce::Colour(0xffcc6600) : juce::Colour(0xff7a3300);
                textCol = juce::Colour(0xffffcc66);
                break;
            case SlotState::PLAYING:
                bgCol   = juce::Colour(0xff1a6a1a);
                textCol = juce::Colour(0xff88ff88);
                break;
            default: // IDLE
                bgCol   = hasContent ? juce::Colour(0xff1e3028) : juce::Colour(0xff282828);
                textCol = hasContent ? juce::Colour(0xff66cc88) : juce::Colour(0xff484848);
                break;
        }

        g.setColour(bgCol);
        g.fillRoundedRectangle(cell.toFloat(), 3.0f);

        // ── Selection border ──────────────────────────────────────────────────
        if (isSelected)
        {
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(cell.toFloat().reduced(1.0f), 3.0f, 2.0f);
        }

        // ── Note name (top half) ──────────────────────────────────────────────
        g.setColour(textCol.withAlpha(0.75f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(kNoteNames[i],
                   cell.withHeight(cell.getHeight() / 2),
                   juce::Justification::centredBottom, false);

        // ── State label (bottom half) ─────────────────────────────────────────
        juce::String stateStr;
        switch (st)
        {
            case SlotState::RECORDING: stateStr = "REC";  break;
            case SlotState::ARMED:     stateStr = "ARM";  break;
            case SlotState::PLAYING:   stateStr = "PLAY"; break;
            default:
                if (hasContent && fs != nullptr)
                {
                    const float s = static_cast<float>(fs->getSlotDurationUs(i)) * 1e-6f;
                    stateStr = juce::String(s, 1) + "s";
                }
                else
                    stateStr = "--";
                break;
        }
        g.setColour(textCol);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(stateStr,
                   cell.withTrimmedTop(cell.getHeight() / 2),
                   juce::Justification::centredTop, false);

        // ── White underline for sequencer-active slot ─────────────────────────
        if (isSeqActive)
        {
            g.setColour(juce::Colours::white);
            g.fillRect(cell.getX(), cell.getBottom() + 1, cell.getWidth(), 3);
        }
    }
}

void SlotGridComponent::mouseDown(const juce::MouseEvent& e)
{
    constexpr int kGap    = 3;
    const int totalGaps   = (FrameSamplerConstants::NUM_SLOTS - 1) * kGap;
    const int cellW       = (getWidth() - totalGaps) / FrameSamplerConstants::NUM_SLOTS;
    if (cellW <= 0) return;

    const int idx = e.x / (cellW + kGap);
    if (idx < 0 || idx >= FrameSamplerConstants::NUM_SLOTS) return;

    selectedSlot = idx;
    repaint();
    if (onSlotSelected)
        onSlotSelected(idx);
}

void SlotGridComponent::timerCallback()
{
    blinkOn = !blinkOn;
    repaint();
}
