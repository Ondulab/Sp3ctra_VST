#include "SlotTimelineComponent.h"
#include "../PluginProcessor.h"

SlotTimelineComponent::SlotTimelineComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    startTimer(80); // ~12 Hz playhead repaint
}

SlotTimelineComponent::~SlotTimelineComponent()
{
    stopTimer();
}

void SlotTimelineComponent::setSelectedSlot(int idx)
{
    selectedSlot = juce::jlimit(0, FrameSamplerConstants::NUM_SLOTS - 1, idx);
    markDirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thumbnail computation
// Reads frame data from the Non-RT slot (safe from message thread while
// the slot is not currently recording).
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::rebuildThumbnail()
{
    thumbnailDirty = false;
    numSamples     = 0;

    auto* fs = processor.getFrameSampler();
    if (fs == nullptr || !fs->slotHasContent(selectedSlot)) return;

    const int w = getWidth();
    if (w <= 0) return;

    const int N = juce::jlimit(2, kMaxSamples, w);
    fs->sampleBrightnessForTimeline(selectedSlot, brightness, N);
    numSamples = N;
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::paint(juce::Graphics& g)
{
    const int w = getWidth();
    const int h = getHeight();

    // Lazy thumbnail rebuild (message thread only)
    if (thumbnailDirty && w > 0)
        rebuildThumbnail();

    // Background
    g.setColour(juce::Colour(0xff0a0a14));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);

    auto* fs = processor.getFrameSampler();
    const bool hasContent = (fs != nullptr) && fs->slotHasContent(selectedSlot);

    if (!hasContent || numSamples == 0)
    {
        g.setColour(juce::Colour(0xff2a2a3a));
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("-- no recording --", getLocalBounds(),
                   juce::Justification::centred, false);
        g.setColour(juce::Colour(0xff223344));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 0.5f);
        return;
    }

    // ── Waveform bars ─────────────────────────────────────────────────────────
    for (int col = 0; col < w; ++col)
    {
        const int si = juce::jlimit(0, numSamples - 1,
                                    col * numSamples / w);
        const float bri = brightness[si];
        const int   barH = juce::jlimit(1, h - 4,
                                        static_cast<int>(bri * (float)(h - 4)));
        const auto  barCol = juce::Colour(0xff006680).brighter(bri * 1.4f);
        g.setColour(barCol);
        g.fillRect(col, (h - barH) / 2, 1, barH);
    }

    // ── Start / End markers ───────────────────────────────────────────────────
    const float sf = (fs != nullptr) ? fs->getSlotStartFrac(selectedSlot) : 0.0f;
    const float ef = (fs != nullptr) ? fs->getSlotEndFrac(selectedSlot)   : 1.0f;
    const int   sx = fracToX(sf);
    const int   ex = fracToX(ef);

    // Dim zones outside [start, end]
    g.setColour(juce::Colour(0xaa080810));
    g.fillRect(0, 0, sx, h);
    g.fillRect(ex + 2, 0, w - ex - 2, h);

    // Start marker — green
    g.setColour(juce::Colour(0xff44ee88));
    g.fillRect(sx, 0, 2, h);
    // Top handle triangle
    juce::Path startHandle;
    startHandle.addTriangle((float)sx, 0.0f,
                            (float)(sx + 8), 0.0f,
                            (float)sx, 8.0f);
    g.fillPath(startHandle);

    // End marker — red/orange
    g.setColour(juce::Colour(0xffff5533));
    g.fillRect(ex, 0, 2, h);
    // Bottom handle triangle
    juce::Path endHandle;
    endHandle.addTriangle((float)ex, (float)h,
                          (float)(ex - 8), (float)h,
                          (float)ex, (float)(h - 8));
    g.fillPath(endHandle);

    // ── Playhead cursor ───────────────────────────────────────────────────────
    if (fs != nullptr && fs->getSlotState(selectedSlot) == SlotState::PLAYING)
    {
        const int fc = fs->getSlotFrameCount(selectedSlot);
        if (fc > 0)
        {
            const int   ph   = fs->getSlotPlayHead(selectedSlot);
            const float frac = juce::jlimit(0.0f, 1.0f,
                                            (float)ph / (float)fc);
            const int   phX  = fracToX(frac);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.fillRect(phX, 0, 1, h);
            // Small diamond at top
            juce::Path diamond;
            diamond.addTriangle((float)phX, 0.0f,
                                (float)(phX - 4), 6.0f,
                                (float)(phX + 4), 6.0f);
            g.fillPath(diamond);
        }
    }

    // Border
    g.setColour(juce::Colour(0xff334455));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 0.5f);

    // Axis labels
    g.setColour(juce::Colour(0xff557788));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("0", 3, h - 11, 20, 10, juce::Justification::left);
    g.drawText("1", w - 12, h - 11, 12, 10, juce::Justification::right);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer — repaint only when PLAYING (playhead moving)
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::timerCallback()
{
    auto* fs = processor.getFrameSampler();
    if (fs == nullptr) return;

    const SlotState st = fs->getSlotState(selectedSlot);

    // Invalidate thumbnail when recording just finished
    if (st == SlotState::IDLE && thumbnailDirty)
        repaint();

    if (st == SlotState::PLAYING)
        repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — drag Start / End handles
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::mouseDown(const juce::MouseEvent& e)
{
    auto* fs = processor.getFrameSampler();
    if (!fs || !fs->slotHasContent(selectedSlot)) return;

    const float sf   = fs->getSlotStartFrac(selectedSlot);
    const float ef   = fs->getSlotEndFrac(selectedSlot);
    const int   startX = fracToX(sf);
    const int   endX   = fracToX(ef);
    constexpr int kSnap = 10;

    if (std::abs(e.x - startX) <= kSnap)
        dragging = DragTarget::Start;
    else if (std::abs(e.x - endX) <= kSnap)
        dragging = DragTarget::End;
    else
    {
        // Assign click to nearest marker
        const float mid = (sf + ef) * 0.5f;
        dragging = (xToFrac(e.x) <= mid) ? DragTarget::Start : DragTarget::End;
    }
}

void SlotTimelineComponent::mouseDrag(const juce::MouseEvent& e)
{
    auto* fs = processor.getFrameSampler();
    if (!fs) return;

    const float frac = xToFrac(e.x);

    if (dragging == DragTarget::Start)
    {
        const float clamped = juce::jlimit(
            0.0f, fs->getSlotEndFrac(selectedSlot) - 0.01f, frac);
        fs->setSlotStartFrac(selectedSlot, clamped);
        if (onStartChanged) onStartChanged(clamped);
        repaint();
    }
    else if (dragging == DragTarget::End)
    {
        const float clamped = juce::jlimit(
            fs->getSlotStartFrac(selectedSlot) + 0.01f, 1.0f, frac);
        fs->setSlotEndFrac(selectedSlot, clamped);
        if (onEndChanged) onEndChanged(clamped);
        repaint();
    }
}

void SlotTimelineComponent::mouseUp(const juce::MouseEvent&)
{
    dragging = DragTarget::None;
}

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────
float SlotTimelineComponent::xToFrac(int x) const noexcept
{
    if (getWidth() <= 0) return 0.0f;
    return juce::jlimit(0.0f, 1.0f,
                        (float)x / (float)getWidth());
}

int SlotTimelineComponent::fracToX(float f) const noexcept
{
    return juce::jlimit(0, getWidth() - 1,
                        (int)(f * (float)getWidth()));
}
