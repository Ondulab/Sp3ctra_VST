#include "EnvelopeEditorComponent.h"
#include <cmath>

namespace
{
    constexpr float kHandleR    = 4.5f;     // drawn handle radius
    constexpr float kHitR       = 10.0f;    // grab radius
    constexpr float kPadX       = 8.0f;
    constexpr float kPadTop     = 10.0f;    // room for readouts / legend
    constexpr float kPadBottom  = 8.0f;
    constexpr float kSusFrac    = 0.16f;    // fixed plateau display width

    juce::String formatTime(float ms)
    {
        if (ms < 1000.0f)
            return juce::String(juce::roundToInt(ms)) + " ms";
        return juce::String(ms / 1000.0f, 2) + " s";
    }
}

//==============================================================================
EnvelopeEditorComponent::EnvelopeEditorComponent(
        juce::AudioProcessorValueTreeState& apvtsIn,
        juce::Colour accentColour,
        const juce::String& attackParamId,
        const juce::String& decayParamId,
        const juce::String& sustainParamId,
        const juce::String& releaseParamId,
        const juce::String& widthBaseParamId,
        const juce::String& widthAttackParamId,
        const juce::String& widthReleaseParamId)
    : apvts(apvtsIn), accent(accentColour)
{
    aParam = apvts.getParameter(attackParamId);
    dParam = apvts.getParameter(decayParamId);
    sParam = apvts.getParameter(sustainParamId);
    rParam = apvts.getParameter(releaseParamId);
    jassert(aParam != nullptr && dParam != nullptr
         && sParam != nullptr && rParam != nullptr);

    // Real parameter ranges (PITCH and MASK differ on decay/release max)
    if (aParam != nullptr)
    {
        const auto rg = apvts.getParameterRange(attackParamId);
        aMin = rg.start;  aMax = rg.end;
        aAttach = std::make_unique<juce::ParameterAttachment>(
            *aParam, [this](float v) { aMs = v; repaint(); });
        aAttach->sendInitialUpdate();
    }
    if (dParam != nullptr)
    {
        const auto rg = apvts.getParameterRange(decayParamId);
        dMin = rg.start;  dMax = rg.end;
        dAttach = std::make_unique<juce::ParameterAttachment>(
            *dParam, [this](float v) { dMs = v; repaint(); });
        dAttach->sendInitialUpdate();
    }
    if (sParam != nullptr)
    {
        sAttach = std::make_unique<juce::ParameterAttachment>(
            *sParam, [this](float v) { sLvl = v; repaint(); });
        sAttach->sendInitialUpdate();
    }
    if (rParam != nullptr)
    {
        const auto rg = apvts.getParameterRange(releaseParamId);
        rMin = rg.start;  rMax = rg.end;
        rAttach = std::make_unique<juce::ParameterAttachment>(
            *rParam, [this](float v) { rMs = v; repaint(); });
        rAttach->sendInitialUpdate();
    }

    // Optional MASK width-bloom overlay (read-only — no attachments needed,
    // a slow 4 Hz poll keeps the dashed curve in sync with the sliders).
    if (widthBaseParamId.isNotEmpty())
    {
        wBaseParam = apvts.getParameter(widthBaseParamId);
        wAtkParam  = apvts.getParameter(widthAttackParamId);
        wRelParam  = apvts.getParameter(widthReleaseParamId);
        jassert(wBaseParam != nullptr && wAtkParam != nullptr && wRelParam != nullptr);
        timerCallback();                    // seed cached values
        startTimerHz(4);
    }

    setRepaintsOnMouseActivity(true);
}

EnvelopeEditorComponent::~EnvelopeEditorComponent()
{
    stopTimer();
}

//==============================================================================
void EnvelopeEditorComponent::timerCallback()
{
    // Width params are message-thread values; poll + repaint on change only.
    auto denorm = [](juce::RangedAudioParameter* p) -> float
    { return p != nullptr ? p->convertFrom0to1(p->getValue()) : 0.0f; };

    const float b = denorm(wBaseParam);
    const float a = denorm(wAtkParam);
    const float r = denorm(wRelParam);
    if (b != wBase || a != wAtk || r != wRel)
    {
        wBase = b;  wAtk = a;  wRel = r;
        repaint();
    }
}

//==============================================================================
float EnvelopeEditorComponent::timeToX(float ms, float maxMs, float segMaxW) noexcept
{
    // "Log-ish" axis: sqrt of ms over max — keeps short times editable.
    if (maxMs <= 0.0f)
        return 0.0f;
    return std::sqrt(juce::jlimit(0.0f, 1.0f, ms / maxMs)) * segMaxW;
}

float EnvelopeEditorComponent::xToTime(float dx, float maxMs, float segMaxW) noexcept
{
    if (segMaxW <= 0.0f)
        return 0.0f;
    const float t = juce::jlimit(0.0f, 1.0f, dx / segMaxW);
    return t * t * maxMs;
}

//==============================================================================
EnvelopeEditorComponent::Geometry EnvelopeEditorComponent::computeGeometry() const
{
    Geometry geo;
    auto b = getLocalBounds().toFloat();
    if (b.getWidth() < 60.0f || b.getHeight() < 40.0f)
        return geo;

    geo.inner = { b.getX() + kPadX, b.getY() + kPadTop,
                  b.getWidth() - 2.0f * kPadX,
                  b.getHeight() - kPadTop - kPadBottom };

    geo.susW    = geo.inner.getWidth() * kSusFrac;
    geo.segMaxW = (geo.inner.getWidth() - geo.susW) / 3.0f;

    geo.xStart  = geo.inner.getX();
    geo.xA      = geo.xStart + timeToX(aMs, aMax, geo.segMaxW);
    geo.xD      = geo.xA     + timeToX(dMs, dMax, geo.segMaxW);
    geo.xSusEnd = geo.xD     + geo.susW;
    geo.xR      = geo.xSusEnd + timeToX(rMs, rMax, geo.segMaxW);

    geo.yBase = geo.inner.getBottom();
    geo.yPeak = geo.inner.getY();
    geo.ySus  = geo.yBase - juce::jlimit(0.0f, 1.0f, sLvl)
                          * (geo.yBase - geo.yPeak);
    geo.valid = true;
    return geo;
}

juce::Point<float> EnvelopeEditorComponent::handlePos(Handle h, const Geometry& geo) const
{
    switch (h)
    {
        case Handle::Attack:  return { geo.xA, geo.yPeak };
        case Handle::Decay:   return { geo.xD, geo.ySus  };           // y locked to sustain
        case Handle::Sustain: return { geo.xD + geo.susW * 0.5f, geo.ySus };
        case Handle::Release: return { geo.xR, geo.yBase };
        case Handle::None:
        default:              return {};
    }
}

EnvelopeEditorComponent::Handle
EnvelopeEditorComponent::handleAt(juce::Point<float> p, const Geometry& geo) const
{
    if (!geo.valid)
        return Handle::None;

    Handle best     = Handle::None;
    float  bestDist = kHitR;
    for (Handle h : { Handle::Attack, Handle::Decay, Handle::Sustain, Handle::Release })
    {
        const float d = p.getDistanceFrom(handlePos(h, geo));
        if (d < bestDist)
        {
            bestDist = d;
            best     = h;
        }
    }
    return best;
}

void EnvelopeEditorComponent::updateCursor(Handle h)
{
    switch (h)
    {
        case Handle::Attack:
        case Handle::Decay:
        case Handle::Release:
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); break;
        case Handle::Sustain:
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);    break;
        case Handle::None:
        default:
            setMouseCursor(juce::MouseCursor::NormalCursor);          break;
    }
}

//==============================================================================
void EnvelopeEditorComponent::mouseMove(const juce::MouseEvent& e)
{
    if (dragging != Handle::None)
        return;
    const Handle h = handleAt(e.position, computeGeometry());
    if (h != hovered)
    {
        hovered = h;
        repaint();
    }
    updateCursor(h);
}

void EnvelopeEditorComponent::mouseExit(const juce::MouseEvent&)
{
    if (dragging == Handle::None && hovered != Handle::None)
    {
        hovered = Handle::None;
        updateCursor(Handle::None);
        repaint();
    }
}

void EnvelopeEditorComponent::mouseDown(const juce::MouseEvent& e)
{
    const Handle h = handleAt(e.position, computeGeometry());
    dragging = h;
    hovered  = h;
    updateCursor(h);

    // Host-automation correctness: open the gesture on grab, close on release
    // (ParameterAttachment forwards to begin/endChangeGesture on the param).
    switch (h)
    {
        case Handle::Attack:  if (aAttach) aAttach->beginGesture(); break;
        case Handle::Decay:   if (dAttach) dAttach->beginGesture(); break;
        case Handle::Sustain: if (sAttach) sAttach->beginGesture(); break;
        case Handle::Release: if (rAttach) rAttach->beginGesture(); break;
        case Handle::None:
        default: break;
    }
    if (h != Handle::None)
        repaint();
}

void EnvelopeEditorComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging == Handle::None)
        return;

    const Geometry geo = computeGeometry();
    if (!geo.valid)
        return;

    switch (dragging)
    {
        case Handle::Attack:
        {
            const float ms = juce::jlimit(aMin, aMax,
                xToTime(e.position.x - geo.xStart, aMax, geo.segMaxW));
            if (aAttach) aAttach->setValueAsPartOfGesture(ms);
            break;
        }
        case Handle::Decay:
        {
            const float ms = juce::jlimit(dMin, dMax,
                xToTime(e.position.x - geo.xA, dMax, geo.segMaxW));
            if (dAttach) dAttach->setValueAsPartOfGesture(ms);
            break;
        }
        case Handle::Sustain:
        {
            const float h   = geo.yBase - geo.yPeak;
            const float lvl = h > 0.0f
                ? juce::jlimit(0.0f, 1.0f, (geo.yBase - e.position.y) / h)
                : 0.0f;
            if (sAttach) sAttach->setValueAsPartOfGesture(lvl);
            break;
        }
        case Handle::Release:
        {
            const float ms = juce::jlimit(rMin, rMax,
                xToTime(e.position.x - geo.xSusEnd, rMax, geo.segMaxW));
            if (rAttach) rAttach->setValueAsPartOfGesture(ms);
            break;
        }
        case Handle::None:
        default: break;
    }
}

void EnvelopeEditorComponent::mouseUp(const juce::MouseEvent& e)
{
    switch (dragging)
    {
        case Handle::Attack:  if (aAttach) aAttach->endGesture(); break;
        case Handle::Decay:   if (dAttach) dAttach->endGesture(); break;
        case Handle::Sustain: if (sAttach) sAttach->endGesture(); break;
        case Handle::Release: if (rAttach) rAttach->endGesture(); break;
        case Handle::None:
        default: break;
    }
    dragging = Handle::None;
    hovered  = handleAt(e.position, computeGeometry());
    updateCursor(hovered);
    repaint();
}

//==============================================================================
void EnvelopeEditorComponent::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Panel background
    g.setColour(juce::Colour(0xff20202a));
    g.fillRoundedRectangle(bounds.reduced(0.5f), 4.0f);
    g.setColour(accent.withAlpha(0.25f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

    const Geometry geo = computeGeometry();
    if (!geo.valid)
        return;

    // Baseline + sustain guide
    g.setColour(juce::Colour(0x14ffffff));
    g.drawHorizontalLine((int) geo.yBase, geo.inner.getX(), geo.inner.getRight());
    g.setColour(juce::Colour(0x0cffffff));
    g.drawHorizontalLine((int) geo.ySus, geo.xD, geo.xSusEnd);

    // ── MASK width-bloom trajectory (read-only, dashed, 50 % alpha) ──────────
    if (wBaseParam != nullptr && wAtkParam != nullptr && wRelParam != nullptr)
    {
        // Normalised through the params' own 8..8192 px range (skewed like
        // the sliders, so equal screen steps feel like equal slider steps).
        auto yOfWidth = [&geo](juce::RangedAudioParameter* p, float w) -> float
        {
            const float n = juce::jlimit(0.0f, 1.0f, p->convertTo0to1(w));
            return geo.yBase - n * (geo.yBase - geo.yPeak);
        };
        const float yAtk  = yOfWidth(wAtkParam,  wAtk);
        const float yBaseW= yOfWidth(wBaseParam, wBase);
        const float yRel  = yOfWidth(wRelParam,  wRel);

        juce::Path wp;
        wp.startNewSubPath(geo.xStart, yAtk);   // attack: holds the attack horizon
        wp.lineTo(geo.xA,      yAtk);
        wp.lineTo(geo.xD,      yBaseW);         // decay: lerps to base width
        wp.lineTo(geo.xSusEnd, yBaseW);         // sustain: holds
        wp.lineTo(geo.xR,      yRel);           // release: goes to release horizon

        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, wp, dashes, 2);
        g.setColour(accent.withAlpha(0.5f));
        g.fillPath(dashed);

        // Legend at the curve start
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("width", (int) geo.xStart + 1,
                   (int) juce::jlimit(geo.yPeak, geo.yBase - 9.0f, yAtk - 11.0f),
                   30, 9, juce::Justification::centredLeft, false);
    }

    // ── ADSR envelope ─────────────────────────────────────────────────────────
    juce::Path env;
    env.startNewSubPath(geo.xStart, geo.yBase);
    env.lineTo(geo.xA,      geo.yPeak);     // A: 0 → peak
    env.lineTo(geo.xD,      geo.ySus);      // D: peak → sustain
    env.lineTo(geo.xSusEnd, geo.ySus);      // S: plateau (fixed display width)
    env.lineTo(geo.xR,      geo.yBase);     // R: sustain → 0

    {
        juce::Path fill(env);
        fill.lineTo(geo.xStart, geo.yBase);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(0.12f));
        g.fillPath(fill);
    }
    g.setColour(accent.withAlpha(0.9f));
    g.strokePath(env, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));

    // ── Handles ───────────────────────────────────────────────────────────────
    for (Handle h : { Handle::Attack, Handle::Decay, Handle::Sustain, Handle::Release })
    {
        const auto p      = handlePos(h, geo);
        const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
        const float r     = active ? kHandleR + 1.5f : kHandleR;

        if (active)
        {
            g.setColour(accent.withAlpha(0.25f));
            g.fillEllipse(p.x - r - 2.5f, p.y - r - 2.5f, 2 * (r + 2.5f), 2 * (r + 2.5f));
        }
        g.setColour(active ? accent.brighter(0.3f) : juce::Colour(0xff20202a));
        g.fillEllipse(p.x - r, p.y - r, 2 * r, 2 * r);
        g.setColour(active ? juce::Colours::white : accent.withAlpha(0.9f));
        g.drawEllipse(p.x - r, p.y - r, 2 * r, 2 * r, 1.4f);
    }

    // ── Value readout near the handle while dragging ──────────────────────────
    if (dragging != Handle::None)
    {
        juce::String txt;
        switch (dragging)
        {
            case Handle::Attack:  txt = "A " + formatTime(aMs);                          break;
            case Handle::Decay:   txt = "D " + formatTime(dMs);                          break;
            case Handle::Sustain: txt = "S " + juce::String(juce::roundToInt(sLvl * 100.0f)) + " %"; break;
            case Handle::Release: txt = "R " + formatTime(rMs);                          break;
            case Handle::None:
            default: break;
        }
        const auto p = handlePos(dragging, geo);
        const int  w = 64;
        const int  x = juce::jlimit((int) bounds.getX() + 2,
                                    (int) bounds.getRight() - w - 2,
                                    (int) p.x - w / 2);
        const int  y = (p.y - geo.yPeak < 14.0f) ? (int) p.y + 8 : (int) p.y - 16;
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText(txt, x, y, w, 12, juce::Justification::centred, false);
    }

    // Tag (top-right, subdued)
    g.setColour(accent.withAlpha(0.4f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
    g.drawText("ADSR", (int) bounds.getRight() - 38, (int) bounds.getY() + 2,
               34, 9, juce::Justification::centredRight, false);
}
