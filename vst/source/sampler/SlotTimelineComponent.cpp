#include "SlotTimelineComponent.h"
#include "../PluginProcessor.h"
#include <cmath>

SlotTimelineComponent::SlotTimelineComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    startTimer(80); // ~12 Hz playhead repaint
}

SlotTimelineComponent::~SlotTimelineComponent() { stopTimer(); }

void SlotTimelineComponent::setSelectedSlot(int idx)
{
    selectedSlot = juce::jlimit(0, FrameSamplerConstants::NUM_SLOTS - 1, idx);
    markDirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thumbnail — dense spectral sampling (bass ↓ / treble ↑)
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::rebuildThumbnail()
{
    thumbnailDirty = false;
    numSamples     = 0;
    auto* fs = processor.getFrameSampler();
    if (!fs || !fs->slotHasContent(selectedSlot)) return;
    const int w = getWidth();
    if (w <= 0) return;
    const int N = juce::jlimit(2, kMaxSamples, w);
    fs->sampleSpectralForTimeline(selectedSlot, bass, treble, N);
    numSamples = N;
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::paint(juce::Graphics& g)
{
    const int w  = getWidth();
    const int h  = getHeight();
    const int cy = h / 2;

    if (thumbnailDirty && w > 0) rebuildThumbnail();

    // Background
    g.setColour(juce::Colour(0xff0a0a14));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);

    auto* fs = processor.getFrameSampler();
    const bool hasContent = fs && fs->slotHasContent(selectedSlot);

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

    // ── Spectral waveform — bass ↓ treble ↑ (γ=0.4) ─────────────────────────
    {
        for (int col = 0; col < w; ++col)
        {
            const int   si = juce::jlimit(0, numSamples - 1, col * numSamples / w);
            // Gamma compression γ=0.4 — expands low-energy signals
            const float bg = std::pow(bass[si],   0.4f);
            const float tg = std::pow(treble[si], 0.4f);

            // Bass bar — blue, downward from cy
            const int bH = std::max(1, static_cast<int>(bg * (float)cy));
            g.setColour(juce::Colour(0xff00446a).brighter(bg * 1.2f));
            g.fillRect(col, cy, 1, bH);

            // Treble bar — teal, upward from cy
            const int tH = std::max(1, static_cast<int>(tg * (float)cy));
            g.setColour(juce::Colour(0xff006655).brighter(tg * 1.2f));
            g.fillRect(col, cy - tH, 1, tH);
        }
        // Centre reference line
        g.setColour(juce::Colour(0x22ffffff));
        g.fillRect(0, cy, w, 1);
        // Frequency labels
        g.setColour(juce::Colour(0xff334455));
        g.setFont(juce::FontOptions(8.0f));
        g.drawText("B", 3, h - 10, 8, 9, juce::Justification::left);
        g.drawText("T", 3,     1,  8, 9, juce::Justification::left);
    }

    // Retrieve common params
    const float sf = fs->getSlotStartFrac(selectedSlot);
    const float ef = fs->getSlotEndFrac(selectedSlot);
    const int   sx = fracToX(sf);
    const int   ex = fracToX(ef);

    // ── TrebleCut overlay + handle (drag from top → fade upper bars) ─────────
    // trebleCut=0 → no fade; trebleCut=1 → upper half fully white.
    // The fade covers y=0 → trebleCut*cy.  Handle line at y=trebleCut*cy.
    {
        const float tc   = fs->getSlotTrebleCut(selectedSlot);
        const int   tcY  = static_cast<int>(tc * (float)cy);  // cutoff y from top
        const int   span = ex - sx;

        // Gradient overlay (only when tc > 0)
        if (tc > 0.001f && span > 0)
        {
            juce::ColourGradient grad(
                juce::Colours::white.withAlpha(0.35f), 0.0f, 0.0f,
                juce::Colours::white.withAlpha(0.0f),  0.0f, (float)tcY,
                false);
            g.setGradientFill(grad);
            g.fillRect(sx, 0, span, tcY);
            // Cutoff horizontal line
            g.setColour(juce::Colour(0xff44ddaa).withAlpha(0.7f));
            g.fillRect(sx, tcY, span, 1);
        }

        // Handle tab — centred between Start and End cursors
        // Shape: small downward-pointing triangle (▼) at the cutoff line
        const float tabCx = (float)((sx + ex) / 2);  // horizontal centre
        const float tabX  = tabCx - 7.0f;
        const float tabY  = (float)std::max(0, tcY);
        juce::Path tab;
        tab.addTriangle(tabX, tabY, tabX + 14.0f, tabY, tabCx, tabY + 9.0f);
        g.setColour(juce::Colour(0xff44ddaa).withAlpha(0.85f));
        g.fillPath(tab);
        // Label just above the triangle
        g.setColour(juce::Colour(0xff44ddaa).withAlpha(0.6f));
        g.setFont(juce::FontOptions(7.0f));
        g.drawText("HF", (int)tabX, std::max(0, (int)tabY - 8), 14, 8,
                   juce::Justification::centred);
    }

    // ── BassCut overlay + handle (drag from bottom → fade lower bars) ────────
    // bassCut=0 → no fade; bassCut=1 → lower half fully white.
    // Fade covers y=(h-bassCut*cy) → h.  Handle line at y=h-bassCut*cy.
    {
        const float bc   = fs->getSlotBassCut(selectedSlot);
        const int   bcY  = h - static_cast<int>(bc * (float)cy); // cutoff y from bottom
        const int   span = ex - sx;

        if (bc > 0.001f && span > 0)
        {
            juce::ColourGradient grad(
                juce::Colours::white.withAlpha(0.0f),  0.0f, (float)bcY,
                juce::Colours::white.withAlpha(0.35f), 0.0f, (float)h,
                false);
            g.setGradientFill(grad);
            g.fillRect(sx, bcY, span, h - bcY);
            // Cutoff line
            g.setColour(juce::Colour(0xff4488dd).withAlpha(0.7f));
            g.fillRect(sx, bcY, span, 1);
        }

        // Handle tab — upward-pointing triangle (▲) at bottom right
        const float tabX = (float)(ex - 14);
        const float tabY = (float)std::min(h, h - static_cast<int>(bc * (float)cy));
        juce::Path tab;
        tab.addTriangle(tabX, tabY, tabX + 14.0f, tabY, tabX + 7.0f, tabY - 9.0f);
        g.setColour(juce::Colour(0xff4488dd).withAlpha(0.85f));
        g.fillPath(tab);
        // Label
        g.setColour(juce::Colour(0xff4488dd).withAlpha(0.6f));
        g.setFont(juce::FontOptions(7.0f));
        g.drawText("LF", ex - 14, h - 9, 14, 8, juce::Justification::centred);
    }

    // ── Dim zones outside [start, end] ────────────────────────────────────────
    g.setColour(juce::Colour(0xaa080810));
    g.fillRect(0, 0, sx, h);
    g.fillRect(ex + 2, 0, w - ex - 2, h);

    // Start marker — green bar
    g.setColour(juce::Colour(0xff44ee88));
    g.fillRect(sx, 0, 2, h);

    // End marker — orange bar
    g.setColour(juce::Colour(0xffff5533));
    g.fillRect(ex, 0, 2, h);

    // ── Attack fade-in (Start → atkX) ─────────────────────────────────────────
    // Triangle handle ALWAYS drawn (visible even at 0).
    // Gradient overlay only when attackLen > 0.
    {
        const float attackLen = fs->getSlotAttackLen(selectedSlot);
        const int   ax        = sx + static_cast<int>(attackLen * (float)(ex - sx));

        // Gradient (only if > 0)
        if (attackLen > 0.001f && ex > sx)
        {
            juce::ColourGradient grad(
                juce::Colours::white.withAlpha(0.30f), (float)sx, 0.0f,
                juce::Colours::white.withAlpha(0.0f),  (float)ax, 0.0f,
                false);
            g.setGradientFill(grad);
            g.fillRect(sx, 0, ax - sx, h);
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawVerticalLine(ax, 0.0f, (float)h);
        }

        // Handle triangle — always visible ▷ at top
        juce::Path p;
        p.addTriangle((float)ax, 0.0f,
                      (float)(ax + 10), 0.0f,
                      (float)ax, 10.0f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.fillPath(p);
    }

    // ── Decay fade-out (decX → End) ───────────────────────────────────────────
    {
        const float decayLen = fs->getSlotDecayLen(selectedSlot);
        const int   dx       = ex - static_cast<int>(decayLen * (float)(ex - sx));

        if (decayLen > 0.001f && ex > sx)
        {
            juce::ColourGradient grad(
                juce::Colours::white.withAlpha(0.0f),  (float)dx, 0.0f,
                juce::Colours::white.withAlpha(0.30f), (float)ex, 0.0f,
                false);
            g.setGradientFill(grad);
            g.fillRect(dx, 0, ex - dx, h);
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawVerticalLine(dx, 0.0f, (float)h);
        }

        // Handle triangle — always visible ◁ at top
        juce::Path p;
        p.addTriangle((float)dx, 0.0f,
                      (float)(dx - 10), 0.0f,
                      (float)dx, 10.0f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.fillPath(p);
    }

    // ── Playhead ──────────────────────────────────────────────────────────────
    if (fs->getSlotState(selectedSlot) == SlotState::PLAYING)
    {
        const int fc = fs->getSlotFrameCount(selectedSlot);
        if (fc > 0)
        {
            const float frac = juce::jlimit(0.0f, 1.0f,
                (float)fs->getSlotPlayHead(selectedSlot) / (float)fc);
            const int phX = fracToX(frac);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.fillRect(phX, 0, 1, h);
            // Triangle centred on the 1-px bar (centre = phX + 0.5)
            const float cx = (float)phX + 0.5f;
            juce::Path d;
            d.addTriangle(cx,        0.0f,
                          cx - 4.5f, 7.0f,
                          cx + 4.5f, 7.0f);
            g.fillPath(d);
        }
    }

    // Border
    g.setColour(juce::Colour(0xff334455));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 0.5f);

    // Time labels
    g.setColour(juce::Colour(0xff557788));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("0", 12, h - 11, 20, 10, juce::Justification::left);
    g.drawText("1", w - 20, h - 11, 12, 10, juce::Justification::right);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::timerCallback()
{
    auto* fs = processor.getFrameSampler();
    if (!fs) return;
    const SlotState st = fs->getSlotState(selectedSlot);
    if (st == SlotState::IDLE && thumbnailDirty) repaint();
    if (st == SlotState::PLAYING)               repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// Cursor
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::updateCursor(const juce::MouseEvent& e)
{
    auto* fs = processor.getFrameSampler();
    if (!fs || !fs->slotHasContent(selectedSlot))
    {
        setMouseCursor(juce::MouseCursor::NormalCursor); return;
    }

    const int   h  = getHeight();
    const int   cy = h / 2;
    const int   sx = fracToX(fs->getSlotStartFrac(selectedSlot));
    const int   ex = fracToX(fs->getSlotEndFrac(selectedSlot));

    // Same priority as mouseDown — triangles first, then bars.

    // ── Attack triangle ───────────────────────────────────────────────────────
    const float atkLen = fs->getSlotAttackLen(selectedSlot);
    const int   atkX   = sx + static_cast<int>(atkLen * (float)(ex - sx));
    if (e.y <= kAtkH && e.x >= sx && std::abs(e.x - atkX) <= kSnap)
        { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }

    // ── Decay triangle ────────────────────────────────────────────────────────
    const float decLen = fs->getSlotDecayLen(selectedSlot);
    const int   decX   = ex - static_cast<int>(decLen * (float)(ex - sx));
    if (e.y <= kAtkH && e.x <= ex && std::abs(e.x - decX) <= kSnap)
        { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }

    // ── TrebleCut tab (upper half, near HF cutoff line) ──────────────────────
    const float tc  = fs->getSlotTrebleCut(selectedSlot);
    const int   tcY = static_cast<int>(tc * (float)cy);
    if (e.x >= sx && e.x <= ex && e.y < cy
        && (std::abs(e.y - tcY) <= kEdge || (tc < 0.01f && e.y <= kEdge)))
        { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }

    // ── BassCut tab (lower half, near LF cutoff line) ────────────────────────
    const float bc  = fs->getSlotBassCut(selectedSlot);
    const int   bcY = h - static_cast<int>(bc * (float)cy);
    if (e.x >= sx && e.x <= ex && e.y >= cy
        && (std::abs(e.y - bcY) <= kEdge || (bc < 0.01f && e.y >= h - kEdge)))
        { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }

    // ── Start/End vertical bars — only if no triangle was hit ────────────────
    if (std::abs(e.x - sx) <= kSnap || std::abs(e.x - ex) <= kSnap)
        { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); return; }

    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void SlotTimelineComponent::mouseMove(const juce::MouseEvent& e) { updateCursor(e); }

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — drag handles
// Priority: Attack > Decay > TrebleCut > BassCut > Start > End > nearest
// ─────────────────────────────────────────────────────────────────────────────
void SlotTimelineComponent::mouseDown(const juce::MouseEvent& e)
{
    auto* fs = processor.getFrameSampler();
    if (!fs || !fs->slotHasContent(selectedSlot)) return;

    const int   h      = getHeight();
    const int   cy     = h / 2;
    const float sf     = fs->getSlotStartFrac(selectedSlot);
    const float ef     = fs->getSlotEndFrac(selectedSlot);
    const int   startX = fracToX(sf);
    const int   endX   = fracToX(ef);

    // ── Attack (top strip, near atkX) ────────────────────────────────────────
    const float atkLen = fs->getSlotAttackLen(selectedSlot);
    const int   atkX   = startX + static_cast<int>(atkLen * (float)(endX - startX));
    if (e.y <= kAtkH && e.x >= startX && std::abs(e.x - atkX) <= kSnap)
        { dragging = DragTarget::Attack; return; }

    // ── Decay (top strip, near decX) ─────────────────────────────────────────
    const float decLen = fs->getSlotDecayLen(selectedSlot);
    const int   decX   = endX - static_cast<int>(decLen * (float)(endX - startX));
    if (e.y <= kAtkH && e.x <= endX && std::abs(e.x - decX) <= kSnap)
        { dragging = DragTarget::Decay; return; }

    // ── TrebleCut (near the horizontal cut line in upper half) ───────────────
    const float tc  = fs->getSlotTrebleCut(selectedSlot);
    const int   tcY = static_cast<int>(tc * (float)cy);
    if (e.x >= startX && e.x <= endX && e.y < cy)
    {
        if (std::abs(e.y - tcY) <= kEdge || (tc < 0.01f && e.y <= kEdge))
            { dragging = DragTarget::TrebleCut; return; }
    }

    // ── BassCut (near the horizontal cut line in lower half) ──────────────────
    const float bc  = fs->getSlotBassCut(selectedSlot);
    const int   bcY = h - static_cast<int>(bc * (float)cy);
    if (e.x >= startX && e.x <= endX && e.y >= cy)
    {
        if (std::abs(e.y - bcY) <= kEdge || (bc < 0.01f && e.y >= h - kEdge))
            { dragging = DragTarget::BassCut; return; }
    }

    // ── Start / End bars ─────────────────────────────────────────────────────
    if (std::abs(e.x - startX) <= kSnap)
        dragging = DragTarget::Start;
    else if (std::abs(e.x - endX) <= kSnap)
        dragging = DragTarget::End;
    else
    {
        const float mid = (sf + ef) * 0.5f;
        dragging = (xToFrac(e.x) <= mid) ? DragTarget::Start : DragTarget::End;
    }
}

void SlotTimelineComponent::mouseDrag(const juce::MouseEvent& e)
{
    auto* fs = processor.getFrameSampler();
    if (!fs) return;

    const int   h  = getHeight();
    const int   cy = h / 2;

    if (dragging == DragTarget::Attack)
    {
        const int sx2  = fracToX(fs->getSlotStartFrac(selectedSlot));
        const int ex2  = fracToX(fs->getSlotEndFrac(selectedSlot));
        const int span = ex2 - sx2;
        if (span > 0)
        {
            fs->setSlotAttackLen(selectedSlot,
                juce::jlimit(0.0f, 1.0f, (float)(e.x - sx2) / (float)span));
            repaint();
        }
        return;
    }
    if (dragging == DragTarget::Decay)
    {
        const int sx2  = fracToX(fs->getSlotStartFrac(selectedSlot));
        const int ex2  = fracToX(fs->getSlotEndFrac(selectedSlot));
        const int span = ex2 - sx2;
        if (span > 0)
        {
            fs->setSlotDecayLen(selectedSlot,
                juce::jlimit(0.0f, 1.0f, (float)(ex2 - e.x) / (float)span));
            repaint();
        }
        return;
    }
    if (dragging == DragTarget::TrebleCut)
    {
        // e.y in [0..cy] → trebleCut in [0..1]
        const float tc = juce::jlimit(0.0f, 1.0f, (float)e.y / (float)std::max(1, cy));
        fs->setSlotTrebleCut(selectedSlot, tc);
        repaint();
        return;
    }
    if (dragging == DragTarget::BassCut)
    {
        // e.y in [cy..h] → bassCut: 0 when e.y=h, 1 when e.y=cy
        const float bc = juce::jlimit(0.0f, 1.0f,
            (float)(h - e.y) / (float)std::max(1, h - cy));
        fs->setSlotBassCut(selectedSlot, bc);
        repaint();
        return;
    }
    if (dragging == DragTarget::Start)
    {
        const float clamped = juce::jlimit(
            0.0f, fs->getSlotEndFrac(selectedSlot) - 0.01f, xToFrac(e.x));
        fs->setSlotStartFrac(selectedSlot, clamped);
        if (onStartChanged) onStartChanged(clamped);
        repaint();
    }
    else if (dragging == DragTarget::End)
    {
        const float clamped = juce::jlimit(
            fs->getSlotStartFrac(selectedSlot) + 0.01f, 1.0f, xToFrac(e.x));
        fs->setSlotEndFrac(selectedSlot, clamped);
        if (onEndChanged) onEndChanged(clamped);
        repaint();
    }
}

void SlotTimelineComponent::mouseUp(const juce::MouseEvent& e)
{
    dragging = DragTarget::None;
    updateCursor(e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────
float SlotTimelineComponent::xToFrac(int x) const noexcept
{
    if (getWidth() <= 0) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (float)x / (float)getWidth());
}
int SlotTimelineComponent::fracToX(float f) const noexcept
{
    return juce::jlimit(0, getWidth() - 1, (int)(f * (float)getWidth()));
}
