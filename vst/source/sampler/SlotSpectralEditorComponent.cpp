#include "SlotSpectralEditorComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include <algorithm>
#include <cmath>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
SlotSpectralEditorComponent::SlotSpectralEditorComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    startTimer(60);
}

SlotSpectralEditorComponent::~SlotSpectralEditorComponent() { stopTimer(); }

void SlotSpectralEditorComponent::setSelectedSlot(int idx)
{
    selectedSlot_ = juce::jlimit(0, LuxSamplerConstants::NUM_SLOTS - 1, idx);
    markDirty();
}

void SlotSpectralEditorComponent::setSamplerIndex(int i)
{
    samplerIndex_ = i;
    markDirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry
// ─────────────────────────────────────────────────────────────────────────────
juce::Rectangle<float> SlotSpectralEditorComponent::plotArea() const
{
    return getLocalBounds().toFloat().reduced(3.0f);
}
juce::Rectangle<float> SlotSpectralEditorComponent::imageArea() const
{
    return plotArea().withTrimmedTop((float) kTopStrip);
}
float SlotSpectralEditorComponent::xToFrac(float x) const
{
    const auto a = imageArea();
    return juce::jlimit(0.0f, 1.0f, (x - a.getX()) / juce::jmax(1.0f, a.getWidth()));
}
float SlotSpectralEditorComponent::fracToX(float f) const
{
    const auto a = imageArea();
    return a.getX() + juce::jlimit(0.0f, 1.0f, f) * a.getWidth();
}

// ─────────────────────────────────────────────────────────────────────────────
// Preview image — authentic frames with EQ (per frequency row) and fades (per
// time column) baked in, so the picture shows the actual playback result.
// ─────────────────────────────────────────────────────────────────────────────
void SlotSpectralEditorComponent::rebuildImage()
{
    imageDirty_ = false;
    const auto a = imageArea();
    builtW_ = juce::jmax(1, (int) a.getWidth());
    builtH_ = juce::jmax(1, (int) a.getHeight());

    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) { image_ = {}; return; }

    const int capW = juce::jmin(1100, builtW_);
    const int capH = juce::jmin(420,  builtH_);
    juce::Image raw = fs->renderSlotImage(selectedSlot_, capW, capH, /*timeHorizontal=*/true);
    if (!raw.isValid()) { image_ = {}; return; }

    const int W = raw.getWidth(), H = raw.getHeight();

    // Gather playback params for the preview.
    const float sf  = fs->getSlotStartFrac(selectedSlot_);
    const float ef  = fs->getSlotEndFrac(selectedSlot_);
    const float atk = fs->getSlotAttackLen(selectedSlot_);
    const float dec = fs->getSlotDecayLen(selectedSlot_);
    const float span = juce::jmax(1.0e-4f, ef - sf);
    const auto  atkC = fs->getSlotAttackCurveType(selectedSlot_);
    const float atkP = fs->getSlotAttackCurvePower(selectedSlot_);
    const auto  decC = fs->getSlotDecayCurveType(selectedSlot_);
    const float decP = fs->getSlotDecayCurvePower(selectedSlot_);

    const bool  eqOn = fs->isFreqCurveActive(selectedSlot_);
    const float* eqLut = eqOn ? fs->getFreqLut(selectedSlot_, fs->getFreqLutActive(selectedSlot_))
                              : nullptr;
    const float eqScale = (float) (LuxSamplerConstants::FREQ_LUT_N - 1);
    const float eqRange = LuxSamplerConstants::EQ_DYN_RANGE_DB;

    const float floorThr = fs->getSlotEqFloor(selectedSlot_) * 255.0f; // pre-EQ gate

    const bool needsWork = eqOn || atk > 0.001f || dec > 0.001f || floorThr > 0.25f;
    if (!needsWork) { image_ = raw; return; } // share, no per-pixel work

    // Per-column whiten from the fades (0 = no change, 1 = full white/silence).
    std::vector<float> whiten((size_t) W, 0.0f);
    for (int x = 0; x < W; ++x)
    {
        const float fx = (W > 1) ? (float) x / (float) (W - 1) : 0.0f;
        if (fx < sf || fx > ef) continue;               // outside the play region
        float w = 0.0f;
        if (atk > 0.001f)
        {
            const float ae = sf + atk * span;
            if (fx < ae) { const float t = juce::jlimit(0.0f, 1.0f, (fx - sf) / (atk * span));
                           w = juce::jmax(w, 1.0f - applyFadeCurve(t, atkC, atkP)); }
        }
        if (dec > 0.001f)
        {
            const float ds = ef - dec * span;
            if (fx > ds) { const float t = juce::jlimit(0.0f, 1.0f, (ef - fx) / (dec * span));
                           w = juce::jmax(w, 1.0f - applyFadeCurve(t, decC, decP)); }
        }
        whiten[(size_t) x] = w;
    }

    image_ = raw.createCopy();
    juce::Image::BitmapData bmp(image_, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < H; ++y)
    {
        // EQ: image row 0 = top = treble (position 1), row H-1 = bass (position 0).
        float dShift = 0.0f;
        if (eqLut != nullptr)
        {
            const float pos = (H > 1) ? 1.0f - (float) y / (float) (H - 1) : 0.0f;
            const float gdb = eqLut[(int) (juce::jlimit(0.0f, 1.0f, pos) * eqScale)];
            dShift = gdb / eqRange;
        }
        // Per-row 256→256 EQ LUT (constant along the row).
        juce::uint8 rowLut[256];
        for (int v = 0; v < 256; ++v)
        {
            if (std::abs(dShift) < 1.0e-4f) { rowLut[v] = (juce::uint8) v; continue; }
            if (v >= 255 && dShift > 0.0f)  { rowLut[v] = 255; continue; }
            const float dk = juce::jlimit(0.0f, 1.0f, (1.0f - (float) v / 255.0f) + dShift);
            rowLut[v] = (juce::uint8) juce::jlimit(0, 255, (int) std::lround((1.0f - dk) * 255.0f));
        }
        for (int x = 0; x < W; ++x)
        {
            const juce::Colour c = bmp.getPixelColour(x, y);
            const float w = whiten[(size_t) x];
            auto ch = [&](juce::uint8 v) -> juce::uint8
            {
                if (floorThr > 0.25f && (255.0f - (float) v) < floorThr) v = 255; // pre-EQ floor
                v = rowLut[v];
                if (w > 0.0001f)
                    v = (juce::uint8) juce::jlimit(0, 255,
                            (int) std::lround((float) v + w * (255.0f - (float) v)));
                return v;
            };
            bmp.setPixelColour(x, y, juce::Colour(ch(c.getRed()), ch(c.getGreen()), ch(c.getBlue())));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────
void SlotSpectralEditorComponent::paint(juce::Graphics& g)
{
    const auto bf  = getLocalBounds().toFloat();
    const auto img = imageArea();

    g.setColour(juce::Colour(0xff0a0a14));
    g.fillRoundedRectangle(bf, 4.0f);

    if (imageDirty_ || builtW_ != (int) img.getWidth() || builtH_ != (int) img.getHeight())
        rebuildImage();

    auto* fs = processor.getSampler(samplerIndex_);
    const bool hasContent = fs && fs->slotHasContent(selectedSlot_);

    if (image_.isValid())
        g.drawImage(image_,
                    (int) img.getX(), (int) img.getY(), (int) img.getWidth(), (int) img.getHeight(),
                    0, 0, image_.getWidth(), image_.getHeight());
    else
    {
        g.setColour(juce::Colour(0xff2a2a3a));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("-- no recording --", getLocalBounds(), juce::Justification::centred, false);
    }

    if (hasContent)
    {
        drawTimeBars(g, img);
        drawFades(g, img);
    }

    // Axis labels.
    g.setColour(juce::Colour(0xffcc88ff).withAlpha(0.7f));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
    g.drawText("treble", (int) img.getX() + 3, (int) img.getY() + 2, 60, 11, juce::Justification::left, false);
    g.drawText("bass",   (int) img.getX() + 3, (int) img.getBottom() - 12, 60, 11, juce::Justification::left, false);
    if (image_.isValid())
    {
        g.setColour(juce::Colour(0xff55606f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText(juce::String::fromUTF8("time \xe2\x86\x92"),
                   (int) img.getRight() - 60, (int) img.getBottom() - 12, 57, 11,
                   juce::Justification::right, false);
    }

    g.setColour(juce::Colour(0xff334455));
    g.drawRoundedRectangle(bf.reduced(0.5f), 4.0f, 0.5f);
}

// Reaper-style fade handles in the top strip: top-left = fade-in, top-right = out.
// Each fade is drawn as a filled, curve-shaped ramp with a polished handle.
void SlotSpectralEditorComponent::drawFades(juce::Graphics& g, juce::Rectangle<float> img)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;

    const float sf   = fs->getSlotStartFrac(selectedSlot_);
    const float ef   = fs->getSlotEndFrac(selectedSlot_);
    const float atk  = fs->getSlotAttackLen(selectedSlot_);
    const float dec  = fs->getSlotDecayLen(selectedSlot_);
    const float sx   = fracToX(sf);
    const float ex   = fracToX(ef);
    const float top  = plotArea().getY() + 1.0f;
    const float sBot = img.getY() - 1.0f;          // strip bottom = image top
    const float hy   = top + (float) kHandleR + 1.0f; // handle centre / curve peak
    const float H    = juce::jmax(1.0f, sBot - hy);

    const juce::Colour cIn (0xff44ee88);
    const juce::Colour cOut(0xffff6633);

    // Draw one curve-shaped fade ramp + handle.
    //  gain(p): 0 at the silent edge → 1 at full; rising=true for fade-in.
    const auto drawOne = [&](float x0, float x1, FadeCurveType type, float power,
                             juce::Colour col, bool rising, bool active, bool hover)
    {
        const int steps = juce::jmax(2, (int) std::abs(x1 - x0));
        if (x1 > x0 + 0.5f)
        {
            juce::Path curve, fill;
            for (int s = 0; s <= steps; ++s)
            {
                const float t    = (float) s / (float) steps;      // 0..1 across [x0,x1]
                const float p    = rising ? t : (1.0f - t);        // silent-edge param
                const float gain = applyFadeCurve(juce::jlimit(0.0f, 1.0f, p), type, power);
                const float x    = x0 + t * (x1 - x0);
                const float y    = sBot - gain * H;                // gain 0→bottom, 1→peak
                if (s == 0) { curve.startNewSubPath(x, y); fill.startNewSubPath(x, sBot); fill.lineTo(x, y); }
                else        { curve.lineTo(x, y);          fill.lineTo(x, y); }
            }
            fill.lineTo(x1, sBot);
            fill.closeSubPath();
            g.setColour(col.withAlpha(0.18f));
            g.fillPath(fill);
            g.setColour(col.withAlpha(0.9f));
            g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));
        }

        // Handle at the "full" (peak) end — small node, EQ-sized.
        const float hx = rising ? x1 : x0;
        const float r  = (active || hover) ? (float) kHandleR + 1.0f : (float) kHandleR;
        if (active || hover)
        {
            g.setColour(col.withAlpha(0.25f));
            g.fillEllipse(hx - r - 2.5f, hy - r - 2.5f, 2 * (r + 2.5f), 2 * (r + 2.5f));
        }
        g.setColour(active ? col.brighter(0.3f) : juce::Colour(0xff20202a));
        g.fillEllipse(hx - r, hy - r, 2 * r, 2 * r);
        g.setColour(active ? juce::Colours::white : col.withAlpha(0.9f));
        g.drawEllipse(hx - r, hy - r, 2 * r, 2 * r, 1.4f);
    };

    drawOne(sx, fracToX(sf + atk * (ef - sf)),
            fs->getSlotAttackCurveType(selectedSlot_), fs->getSlotAttackCurvePower(selectedSlot_),
            cIn,  /*rising=*/true,  mode_ == Mode::Attack, fadeHover_ == 1);
    drawOne(fracToX(ef - dec * (ef - sf)), ex,
            fs->getSlotDecayCurveType(selectedSlot_), fs->getSlotDecayCurvePower(selectedSlot_),
            cOut, /*rising=*/false, mode_ == Mode::Decay, fadeHover_ == 2);

    // Strip captions (dim, out of the way).
    g.setColour(juce::Colour(0xff55606f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    g.drawText("fade in",  (int) img.getX() + 4, (int) top - 1, 60, 11, juce::Justification::left, false);
    g.drawText("fade out", (int) img.getRight() - 64, (int) top - 1, 60, 11, juce::Justification::right, false);
}

void SlotSpectralEditorComponent::drawTimeBars(juce::Graphics& g, juce::Rectangle<float> img)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;

    const float top = img.getY(), H = img.getHeight();
    const float sx = fracToX(fs->getSlotStartFrac(selectedSlot_));
    const float ex = fracToX(fs->getSlotEndFrac(selectedSlot_));

    // Dim outside [start, end].
    g.setColour(juce::Colour(0xaa080810));
    g.fillRect(img.getX(), top, sx - img.getX(), H);
    g.fillRect(ex, top, img.getRight() - ex, H);

    // Start / End bars with a dark edge for contrast on light images.
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRect(sx - 1.0f, top, 4.0f, H);
    g.setColour(juce::Colour(0xff33ff99));
    g.fillRect(sx, top, 2.0f, H);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRect(ex - 1.0f, top, 4.0f, H);
    g.setColour(juce::Colour(0xffff6633));
    g.fillRect(ex, top, 2.0f, H);

    // Playhead.
    if (fs->getSlotState(selectedSlot_) == SlotState::PLAYING)
    {
        const int fc = fs->getSlotFrameCount(selectedSlot_);
        if (fc > 0)
        {
            const float frac = juce::jlimit(0.0f, 1.0f,
                (float) fs->getSlotPlayHead(selectedSlot_) / (float) fc);
            const float phX = fracToX(frac);
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRect(phX - 1.0f, top, 3.0f, H);
            g.setColour(juce::Colours::white);
            g.fillRect(phX, top, 1.0f, H);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void SlotSpectralEditorComponent::resized() { imageDirty_ = true; }

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — top strip = fade handles; below = Start/End bars (no overlap).
// ─────────────────────────────────────────────────────────────────────────────
void SlotSpectralEditorComponent::mouseMove(const juce::MouseEvent& e)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (!fs || !fs->slotHasContent(selectedSlot_))
        { setMouseCursor(juce::MouseCursor::NormalCursor);
          if (fadeHover_ != 0) { fadeHover_ = 0; repaint(); } return; }

    const bool inStrip = e.position.y < plotArea().getY() + (float) kTopStrip;
    int newHover = 0;
    if (inStrip)
    {
        const float sf = fs->getSlotStartFrac(selectedSlot_);
        const float ef = fs->getSlotEndFrac(selectedSlot_);
        const float inX  = fracToX(sf + fs->getSlotAttackLen(selectedSlot_) * (ef - sf));
        const float outX = fracToX(ef - fs->getSlotDecayLen(selectedSlot_)  * (ef - sf));
        newHover = (std::abs(e.position.x - inX) <= std::abs(e.position.x - outX)) ? 1 : 2;
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    }
    else
    {
        const float sx = fracToX(fs->getSlotStartFrac(selectedSlot_));
        const float ex = fracToX(fs->getSlotEndFrac(selectedSlot_));
        setMouseCursor((std::abs(e.position.x - sx) <= kSnap || std::abs(e.position.x - ex) <= kSnap)
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
    }
    if (newHover != fadeHover_) { fadeHover_ = newHover; repaint(); }
}

void SlotSpectralEditorComponent::mouseDown(const juce::MouseEvent& e)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (!fs || !fs->slotHasContent(selectedSlot_)) { mode_ = Mode::None; return; }

    const float sf = fs->getSlotStartFrac(selectedSlot_);
    const float ef = fs->getSlotEndFrac(selectedSlot_);
    const float sx = fracToX(sf), ex = fracToX(ef);
    const bool  inStrip = e.position.y < plotArea().getY() + (float) kTopStrip;

    if (inStrip)
    {
        // Fade handles: nearest of fade-in-end / fade-out-start, else by side.
        const float inX  = fracToX(sf + fs->getSlotAttackLen(selectedSlot_) * (ef - sf));
        const float outX = fracToX(ef - fs->getSlotDecayLen(selectedSlot_)  * (ef - sf));
        const float dIn  = std::abs(e.position.x - inX);
        const float dOut = std::abs(e.position.x - outX);
        mode_ = (dIn <= dOut) ? Mode::Attack : Mode::Decay;
    }
    else
    {
        if (std::abs(e.position.x - sx) <= kSnap)      mode_ = Mode::Start;
        else if (std::abs(e.position.x - ex) <= kSnap) mode_ = Mode::End;
        else mode_ = (xToFrac(e.position.x) <= (sf + ef) * 0.5f) ? Mode::Start : Mode::End;
    }
    mouseDrag(e);
}

void SlotSpectralEditorComponent::mouseDrag(const juce::MouseEvent& e)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr || mode_ == Mode::None) return;

    const float sf = fs->getSlotStartFrac(selectedSlot_);
    const float ef = fs->getSlotEndFrac(selectedSlot_);
    const float span = juce::jmax(1.0e-4f, ef - sf);

    switch (mode_)
    {
        case Mode::Start:
            fs->setSlotStartFrac(selectedSlot_, juce::jlimit(0.0f, ef - 0.01f, xToFrac(e.position.x)));
            break;
        case Mode::End:
            fs->setSlotEndFrac(selectedSlot_, juce::jlimit(sf + 0.01f, 1.0f, xToFrac(e.position.x)));
            break;
        case Mode::Attack:
            fs->setSlotAttackLen(selectedSlot_,
                juce::jlimit(0.0f, 1.0f, (xToFrac(e.position.x) - sf) / span));
            break;
        case Mode::Decay:
            fs->setSlotDecayLen(selectedSlot_,
                juce::jlimit(0.0f, 1.0f, (ef - xToFrac(e.position.x)) / span));
            break;
        default: break;
    }
    markDirty();   // rebuild the preview with the new edit
}

void SlotSpectralEditorComponent::mouseUp(const juce::MouseEvent& e)
{
    mode_ = Mode::None;
    mouseMove(e);
}

void SlotSpectralEditorComponent::timerCallback()
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;
    if (fs->getSlotState(selectedSlot_) == SlotState::PLAYING) repaint();
    else if (imageDirty_)                                      repaint();
}
