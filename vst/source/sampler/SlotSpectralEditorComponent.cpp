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

    // ── Rotation arrows — translucent overlay top-centre OF THE IMAGE (image
    // banks only). Deliberately OUTSIDE the fade strip: its handles can
    // legitimately travel to the centre (full fades), so anything living in
    // the strip would end up fighting them for the mouse.
    const auto initRot = [this](juce::TextButton& b, const char* glyph,
                                int delta, const char* tip)
    {
        b.setButtonText(juce::String::fromUTF8(glyph));
        b.setTooltip(tip);
        b.setColour(juce::TextButton::buttonColourId, juce::Colour(0x99101418));
        b.setColour(juce::TextButton::textColourOffId,
                    juce::Colours::white.withAlpha(0.85f));
        b.onClick = [this, delta]
        {
            if (auto* fs = processor.getSampler(samplerIndex_))
                if (fs->rotateSlotImage(selectedSlot_, delta))
                {
                    markDirty();
                    if (onContentRotated) onContentRotated();
                }
        };
        addChildComponent(b);   // visibility follows the bank (timer)
    };
    initRot(rotCcwBtn_, "\xe2\x86\xba", -1,   // ↺
            "Rotate the source image 90 deg counter-clockwise (lossless)");
    initRot(rotCwBtn_,  "\xe2\x86\xbb", +1,   // ↻
            "Rotate the source image 90 deg clockwise (lossless)");

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

// Fade curves drawn full-height ON the image (no widget row, no top strip).
// Each fade has two handles: END (coloured, at the peak) = length; MID
// (white, on the curve) = shape — drag bends the curve through the mouse.
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
    const float hy   = img.getY() + (float) kHandleR + 2.0f;  // curve peak (gain 1)
    const float sBot = img.getBottom() - 1.0f;                // curve foot (gain 0)
    const float H    = juce::jmax(1.0f, sBot - hy);

    const juce::Colour cIn (0xff44ee88);
    const juce::Colour cOut(0xffff6633);

    // Draw one curve-shaped fade ramp + END handle + MID (shape) handle.
    //  gain(p): 0 at the silent edge → 1 at full; rising=true for fade-in.
    const auto drawOne = [&](float x0, float x1, FadeCurveType type, float power,
                             juce::Colour col, bool rising, bool active, bool hover,
                             bool midActive, bool midHover)
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

        // MID (shape) handle — white node ON the curve at mid-span. Hidden
        // for near-zero fades (nothing to bend).
        if (x1 > x0 + 8.0f)
        {
            const float mx = (x0 + x1) * 0.5f;
            const float mg = applyFadeCurve(0.5f, type, power);
            const float my = sBot - mg * H;
            const float mr = (midActive || midHover) ? (float) kHandleR + 1.5f
                                                     : (float) kHandleR + 0.5f;
            if (midActive || midHover)
            {
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.fillEllipse(mx - mr - 2.5f, my - mr - 2.5f,
                              2 * (mr + 2.5f), 2 * (mr + 2.5f));
            }
            g.setColour(juce::Colour(0xff20202a));
            g.fillEllipse(mx - mr, my - mr, 2 * mr, 2 * mr);
            g.setColour(midActive ? col.brighter(0.5f)
                                  : juce::Colours::white.withAlpha(0.9f));
            g.drawEllipse(mx - mr, my - mr, 2 * mr, 2 * mr, 1.6f);
        }
    };

    drawOne(sx, fracToX(sf + atk * (ef - sf)),
            fs->getSlotAttackCurveType(selectedSlot_), fs->getSlotAttackCurvePower(selectedSlot_),
            cIn,  /*rising=*/true,  mode_ == Mode::Attack, fadeHover_ == 1,
            mode_ == Mode::AttackShape, fadeHover_ == 3);
    drawOne(fracToX(ef - dec * (ef - sf)), ex,
            fs->getSlotDecayCurveType(selectedSlot_), fs->getSlotDecayCurvePower(selectedSlot_),
            cOut, /*rising=*/false, mode_ == Mode::Decay, fadeHover_ == 2,
            mode_ == Mode::DecayShape, fadeHover_ == 4);
    // (captions moved to the owner's info labels UNDER the view)
}

//──────────────────────────────────────────────────────────────────────────────
// Fade handle geometry + curve-type menu
//──────────────────────────────────────────────────────────────────────────────
juce::Point<float> SlotSpectralEditorComponent::fadeEndPoint(bool in) const
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (!fs || !fs->slotHasContent(selectedSlot_)) return { -1.0f, -1.0f };
    const float sf = fs->getSlotStartFrac(selectedSlot_);
    const float ef = fs->getSlotEndFrac(selectedSlot_);
    const auto  im = imageArea();
    const float x  = in ? fracToX(sf + fs->getSlotAttackLen(selectedSlot_) * (ef - sf))
                        : fracToX(ef - fs->getSlotDecayLen(selectedSlot_)  * (ef - sf));
    return { x, im.getY() + (float) kHandleR + 2.0f };
}

juce::Point<float> SlotSpectralEditorComponent::fadeMidPoint(bool in) const
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (!fs || !fs->slotHasContent(selectedSlot_)) return { -1.0f, -1.0f };
    const float sf = fs->getSlotStartFrac(selectedSlot_);
    const float ef = fs->getSlotEndFrac(selectedSlot_);

    float x0, x1; FadeCurveType type; float power;
    if (in)
    {
        x0 = fracToX(sf);
        x1 = fracToX(sf + fs->getSlotAttackLen(selectedSlot_) * (ef - sf));
        type  = fs->getSlotAttackCurveType(selectedSlot_);
        power = fs->getSlotAttackCurvePower(selectedSlot_);
    }
    else
    {
        x0 = fracToX(ef - fs->getSlotDecayLen(selectedSlot_) * (ef - sf));
        x1 = fracToX(ef);
        type  = fs->getSlotDecayCurveType(selectedSlot_);
        power = fs->getSlotDecayCurvePower(selectedSlot_);
    }
    if (x1 - x0 <= 8.0f)
        return { -1.0f, -1.0f };   // near-zero fade: no shape handle

    const auto  im   = imageArea();
    const float peak = im.getY() + (float) kHandleR + 2.0f;
    const float bot  = im.getBottom() - 1.0f;
    const float mg   = applyFadeCurve(0.5f, type, power);
    return { (x0 + x1) * 0.5f, bot - mg * juce::jmax(1.0f, bot - peak) };
}

void SlotSpectralEditorComponent::showFadeTypeMenu(bool in)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;
    const auto cur = in ? fs->getSlotAttackCurveType(selectedSlot_)
                        : fs->getSlotDecayCurveType(selectedSlot_);

    juce::PopupMenu m;
    m.addSectionHeader(in ? "Fade in curve" : "Fade out curve");
    static const char* kNames[] = { "LIN", "EXP", "LOG", "S" };
    for (int i = 0; i < kNumFadeCurveTypes; ++i)
        m.addItem(i + 1, kNames[i], true, static_cast<int>(cur) == i);

    juce::Component::SafePointer<SlotSpectralEditorComponent> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
        [safe, in](int result)
        {
            if (safe == nullptr || result <= 0) return;
            auto* fs2 = safe->processor.getSampler(safe->samplerIndex_);
            if (fs2 == nullptr) return;
            const auto t = static_cast<FadeCurveType>(result - 1);
            if (in) fs2->setSlotAttackCurveType(safe->selectedSlot_, t);
            else    fs2->setSlotDecayCurveType (safe->selectedSlot_, t);
            safe->markDirty();
            if (safe->onFadeChanged) safe->onFadeChanged();
        });
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
void SlotSpectralEditorComponent::resized()
{
    imageDirty_ = true;
    // Rotation arrows — overlay centred at the top OF THE IMAGE, just under
    // the fade strip (which stays 100% owned by the fade handles).
    const auto img = imageArea();
    const int  cx  = (int) img.getCentreX();
    const int  y   = (int) img.getY() + 4;
    rotCcwBtn_.setBounds(cx - 24, y, 22, 18);
    rotCwBtn_ .setBounds(cx + 2,  y, 22, 18);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — fade handles (END = length, MID = shape) first, then Start/End bars.
// ─────────────────────────────────────────────────────────────────────────────
void SlotSpectralEditorComponent::mouseMove(const juce::MouseEvent& e)
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (!fs || !fs->slotHasContent(selectedSlot_))
        { setMouseCursor(juce::MouseCursor::NormalCursor);
          if (fadeHover_ != 0) { fadeHover_ = 0; repaint(); } return; }

    const auto near = [&](juce::Point<float> p)
    { return p.x >= 0.0f && e.position.getDistanceFrom(p) <= (float) kGrabR; };

    int newHover = 0;
    if      (near(fadeEndPoint(true)))  newHover = 1;
    else if (near(fadeEndPoint(false))) newHover = 2;
    else if (near(fadeMidPoint(true)))  newHover = 3;
    else if (near(fadeMidPoint(false))) newHover = 4;

    if (newHover != 0)
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
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

    const auto near = [&](juce::Point<float> p)
    { return p.x >= 0.0f && e.position.getDistanceFrom(p) <= (float) kGrabR; };
    const bool endIn  = near(fadeEndPoint(true));
    const bool endOut = near(fadeEndPoint(false));
    const bool midIn  = near(fadeMidPoint(true));
    const bool midOut = near(fadeMidPoint(false));

    // Right-click a fade handle → curve type menu (LIN/EXP/LOG/S).
    if (e.mods.isRightButtonDown())
    {
        mode_ = Mode::None;
        if (endIn || midIn)        showFadeTypeMenu(true);
        else if (endOut || midOut) showFadeTypeMenu(false);
        return;
    }

    // Double-click the MID handle → back to a straight (LIN) fade.
    if ((midIn || midOut) && e.getNumberOfClicks() >= 2)
    {
        if (midIn)
        {
            fs->setSlotAttackCurveType (selectedSlot_, FadeCurveType::LINEAR);
            fs->setSlotAttackCurvePower(selectedSlot_, 1.0f);
        }
        else
        {
            fs->setSlotDecayCurveType (selectedSlot_, FadeCurveType::LINEAR);
            fs->setSlotDecayCurvePower(selectedSlot_, 1.0f);
        }
        mode_ = Mode::None;
        markDirty();
        if (onFadeChanged) onFadeChanged();
        return;
    }

    if      (endIn)  mode_ = Mode::Attack;
    else if (endOut) mode_ = Mode::Decay;
    else if (midIn)  mode_ = Mode::AttackShape;
    else if (midOut) mode_ = Mode::DecayShape;
    else
    {
        const float sf = fs->getSlotStartFrac(selectedSlot_);
        const float ef = fs->getSlotEndFrac(selectedSlot_);
        const float sx = fracToX(sf), ex = fracToX(ef);
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
        case Mode::AttackShape:
        case Mode::DecayShape:
        {
            // Bend the curve so its mid-point passes through the mouse:
            // below the diagonal → EXP, above → LOG, close to it → LIN
            // (FadeCurve.h maps the exponent as 1 + power); an S curve keeps
            // its type and takes the power instead (smoothstep(0.5) = 0.5).
            const bool  in   = (mode_ == Mode::AttackShape);
            const auto  img  = imageArea();
            const float peak = img.getY() + (float) kHandleR + 2.0f;
            const float bot  = img.getBottom() - 1.0f;
            const float gv   = juce::jlimit(0.02f, 0.98f,
                (bot - e.position.y) / juce::jmax(1.0f, bot - peak));
            const float lg   = std::log(gv) / std::log(0.5f); // exponent giving gv at t=0.5

            const auto cur = in ? fs->getSlotAttackCurveType(selectedSlot_)
                                : fs->getSlotDecayCurveType(selectedSlot_);
            FadeCurveType type;
            float         power;
            if (cur == FadeCurveType::SCURVE)
            { type = FadeCurveType::SCURVE;      power = juce::jlimit(0.1f, 10.0f, lg); }
            else if (std::abs(gv - 0.5f) < 0.015f)
            { type = FadeCurveType::LINEAR;      power = 1.0f; }
            else if (gv < 0.5f)
            { type = FadeCurveType::EXPONENTIAL; power = juce::jlimit(0.1f, 10.0f, lg - 1.0f); }
            else
            { type = FadeCurveType::LOGARITHMIC; power = juce::jlimit(0.1f, 10.0f, 1.0f / lg - 1.0f); }

            if (in)
            {
                fs->setSlotAttackCurveType (selectedSlot_, type);
                fs->setSlotAttackCurvePower(selectedSlot_, power);
            }
            else
            {
                fs->setSlotDecayCurveType (selectedSlot_, type);
                fs->setSlotDecayCurvePower(selectedSlot_, power);
            }
            break;
        }
        default: break;
    }
    markDirty();   // rebuild the preview with the new edit

    if (onFadeChanged
        && (mode_ == Mode::Attack || mode_ == Mode::Decay
            || mode_ == Mode::AttackShape || mode_ == Mode::DecayShape))
        onFadeChanged();
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

    // Rotation arrows only make sense for image-loaded banks.
    const bool rotatable = fs->slotHasContent(selectedSlot_)
                        && fs->getSlotSourcePath(selectedSlot_).isNotEmpty();
    rotCcwBtn_.setVisible(rotatable);
    rotCwBtn_ .setVisible(rotatable);

    if (fs->getSlotState(selectedSlot_) == SlotState::PLAYING) repaint();
    else if (imageDirty_)                                      repaint();
}
