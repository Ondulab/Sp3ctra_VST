#include "EnvelopeEditorComponent.h"
#include "../processing/lux_env_shape.h"
#include <cmath>

namespace
{
    constexpr float kNodeR   = 4.5f;   // drawn A/D/S/R node radius
    constexpr float kBendR   = 3.2f;   // drawn bend handle radius
    constexpr float kHitR    = 11.0f;  // grab radius
    constexpr float kPadX    = 8.0f;
    constexpr float kLaneTop = 9.0f;   // room for readouts inside each lane
    constexpr float kLanePad = 6.0f;
    constexpr float kSusFrac = 0.16f;  // fixed sustain plateau display width

    constexpr int kBoxH    = 16;
    constexpr int kLabelH  = 9;
    constexpr int kRowGap  = 3;
    constexpr int kBoxRowH = kBoxH + kLabelH;

    juce::String formatTime(float ms)
    {
        if (ms < 1000.0f) return juce::String(juce::roundToInt(ms)) + " ms";
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
        const juce::String& attackCurveParamId,
        const juce::String& decayCurveParamId,
        const juce::String& releaseCurveParamId,
        const juce::String& widthBaseParamId,
        const juce::String& widthAttackParamId,
        const juce::String& widthReleaseParamId)
    : apvts(apvtsIn), accent(accentColour)
{
    setParamIds(attackParamId, decayParamId, sustainParamId, releaseParamId,
                attackCurveParamId, decayCurveParamId, releaseCurveParamId,
                widthBaseParamId, widthAttackParamId, widthReleaseParamId);

    setRepaintsOnMouseActivity(true);
}

void EnvelopeEditorComponent::setParamIds(
        const juce::String& attackParamId,
        const juce::String& decayParamId,
        const juce::String& sustainParamId,
        const juce::String& releaseParamId,
        const juce::String& attackCurveParamId,
        const juce::String& decayCurveParamId,
        const juce::String& releaseCurveParamId,
        const juce::String& widthBaseParamId,
        const juce::String& widthAttackParamId,
        const juce::String& widthReleaseParamId)
{
    // Drop every previous binding first — this is also the per-instance rebind
    // path (contextual pages switch the editor onto another slot's bank).
    for (Bound* b : { &a, &d, &s, &r, &aCurve, &dCurve, &rCurve,
                      &wBase, &wAtk, &wRel })
    {
        b->attach.reset();
        b->param = nullptr;
    }
    boxAAtt.reset(); boxDAtt.reset(); boxSAtt.reset(); boxRAtt.reset();
    boxWAtkAtt.reset(); boxWAtt.reset(); boxWRelAtt.reset();

    isAR = sustainParamId.isEmpty();   // AR envelope: no decay/sustain stage

    bind(a, attackParamId);
    if (!isAR)
    {
        bind(d, decayParamId);
        bind(s, sustainParamId, /*readRange*/ false);
    }
    bind(r, releaseParamId);

    hasCurve = attackCurveParamId.isNotEmpty();
    if (hasCurve)
    {
        bind(aCurve, attackCurveParamId,  false);
        if (!isAR) bind(dCurve, decayCurveParamId, false);
        bind(rCurve, releaseCurveParamId, false);
    }

    initBox(boxA, attackParamId,  boxAAtt);
    if (!isAR)
    {
        initBox(boxD, decayParamId,   boxDAtt);
        initBox(boxS, sustainParamId, boxSAtt);
    }
    initBox(boxR, releaseParamId, boxRAtt);

    hasWidth = widthBaseParamId.isNotEmpty();
    if (hasWidth)
    {
        bind(wBase, widthBaseParamId);
        bind(wAtk,  widthAttackParamId);
        bind(wRel,  widthReleaseParamId);
        initBox(boxWAtk, widthAttackParamId,  boxWAtkAtt);
        initBox(boxW,    widthBaseParamId,    boxWAtt);
        initBox(boxWRel, widthReleaseParamId, boxWRelAtt);
    }

    // Right-click MIDI Learn on the value boxes (per-instance ids).
    learnAtts_.clear();
    if (midiMap_ != nullptr)
    {
        auto learn = [&](juce::Component& c, const juce::String& id)
        {
            if (id.isNotEmpty())
                learnAtts_.push_back(
                    std::make_unique<MidiLearnAttachment>(*midiMap_, c, id));
        };
        learn(boxA, attackParamId);
        if (!isAR)
        {
            learn(boxD, decayParamId);
            learn(boxS, sustainParamId);
        }
        learn(boxR, releaseParamId);
        if (hasWidth)
        {
            learn(boxWAtk, widthAttackParamId);
            learn(boxW,    widthBaseParamId);
            learn(boxWRel, widthReleaseParamId);
        }
    }

    repaint();
}

EnvelopeEditorComponent::~EnvelopeEditorComponent() = default;

//==============================================================================
void EnvelopeEditorComponent::bind(Bound& b, const juce::String& id, bool readRange)
{
    if (id.isEmpty()) return;   // optional parameter (e.g. curve on audio ADSRs)
    b.param = apvts.getParameter(id);
    jassert(b.param != nullptr);
    if (b.param == nullptr) return;

    if (readRange)
    {
        const auto rg = apvts.getParameterRange(id);
        b.min = rg.start;
        b.max = rg.end;
    }
    b.attach = std::make_unique<juce::ParameterAttachment>(
        *b.param, [this, &b](float v) { b.value = v; repaint(); });
    b.attach->sendInitialUpdate();
}

void EnvelopeEditorComponent::initBox(
        Sp3ctraBarSlider& box, const juce::String& paramId,
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
{
    box.setAccent(accent);
    addAndMakeVisible(box);
    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, paramId, box);
}

//==============================================================================
float EnvelopeEditorComponent::timeToX(float ms, float maxMs, float segMaxW) noexcept
{
    if (maxMs <= 0.0f) return 0.0f;
    return std::sqrt(juce::jlimit(0.0f, 1.0f, ms / maxMs)) * segMaxW;
}

float EnvelopeEditorComponent::xToTime(float dx, float maxMs, float segMaxW) noexcept
{
    if (segMaxW <= 0.0f) return 0.0f;
    const float t = juce::jlimit(0.0f, 1.0f, dx / segMaxW);
    return t * t * maxMs;
}

float EnvelopeEditorComponent::curveFromHalfValue(float targetS) noexcept
{
    // shape(0.5, curve) is monotonically decreasing in curve (≈0.95 → 0.05).
    targetS = juce::jlimit(0.02f, 0.98f, targetS);
    float lo = -1.0f, hi = 1.0f;
    for (int i = 0; i < 24; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if (lux_env_shape(0.5f, mid) > targetS) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

//==============================================================================
void EnvelopeEditorComponent::resized()
{
    auto area = getLocalBounds().reduced(6);
    const int rows        = hasWidth ? 2 : 1;
    const int totalBoxRows = rows * kBoxRowH + rows * kRowGap;
    const int laneTotal   = juce::jmax(20, area.getHeight() - totalBoxRows);
    const int alphaLaneH  = hasWidth ? juce::roundToInt(laneTotal * 0.6f) : laneTotal;
    const int widthLaneH  = hasWidth ? (laneTotal - alphaLaneH) : 0;

    alphaLaneRect_ = area.removeFromTop(alphaLaneH).toFloat();

    auto layoutBoxRow = [](juce::Rectangle<int> row, std::initializer_list<juce::Slider*> boxes)
    {
        row.removeFromTop(kLabelH);                 // label strip above
        const int n   = (int) boxes.size();
        const int gap = 5;
        const int bw  = (row.getWidth() - (n - 1) * gap) / juce::jmax(1, n);
        int i = 0;
        for (auto* b : boxes)
        {
            b->setBounds(row.getX() + i * (bw + gap), row.getY(), bw, kBoxH);
            ++i;
        }
    };

    if (isAR) layoutBoxRow(area.removeFromTop(kBoxRowH), { &boxA, &boxR });
    else      layoutBoxRow(area.removeFromTop(kBoxRowH), { &boxA, &boxD, &boxS, &boxR });

    if (hasWidth)
    {
        area.removeFromTop(kRowGap);
        widthLaneRect_ = area.removeFromTop(widthLaneH).toFloat();
        area.removeFromTop(kRowGap);
        layoutBoxRow(area.removeFromTop(kBoxRowH), { &boxWAtk, &boxW, &boxWRel });
    }
}

EnvelopeEditorComponent::Geometry EnvelopeEditorComponent::computeGeometry() const
{
    Geometry geo;
    if (alphaLaneRect_.getWidth() < 60.0f || alphaLaneRect_.getHeight() < 24.0f)
        return geo;

    geo.alpha = alphaLaneRect_.reduced(kPadX, 0.0f)
                    .withTrimmedTop(kLaneTop).withTrimmedBottom(kLanePad);
    geo.width = widthLaneRect_.reduced(kPadX, 0.0f)
                    .withTrimmedTop(kLaneTop).withTrimmedBottom(kLanePad);

    geo.aYBase = geo.alpha.getBottom();
    geo.aYPeak = geo.alpha.getY();
    geo.xStart = geo.alpha.getX();

    if (isAR)
    {
        // Two segments (attack rise, release fall), no sustain plateau.
        geo.susW    = 0.0f;
        geo.segMaxW = geo.alpha.getWidth() * 0.5f;
        geo.xA      = geo.xStart + timeToX(a.value, a.max, geo.segMaxW);
        geo.xD      = geo.xA;                          // no decay
        geo.xSusEnd = geo.xA;                          // no plateau
        geo.xR      = geo.xA + timeToX(r.value, r.max, geo.segMaxW);
        geo.aYSus   = geo.aYPeak;                      // release starts from the peak
    }
    else
    {
        geo.susW    = geo.alpha.getWidth() * kSusFrac;
        geo.segMaxW = (geo.alpha.getWidth() - geo.susW) / 3.0f;
        geo.xA      = geo.xStart + timeToX(a.value, a.max, geo.segMaxW);
        geo.xD      = geo.xA     + timeToX(d.value, d.max, geo.segMaxW);
        geo.xSusEnd = geo.xD     + geo.susW;
        geo.xR      = geo.xSusEnd + timeToX(r.value, r.max, geo.segMaxW);
        geo.aYSus   = geo.aYBase - juce::jlimit(0.0f, 1.0f, s.value) * (geo.aYBase - geo.aYPeak);
    }

    if (hasWidth && wBase.param != nullptr)
    {
        auto yOfWidth = [&geo](const Bound& b) -> float
        {
            const float n = juce::jlimit(0.0f, 1.0f, b.param->convertTo0to1(b.value));
            return geo.width.getBottom() - n * geo.width.getHeight();
        };
        geo.wYAtk  = yOfWidth(wAtk);
        geo.wYBase = yOfWidth(wBase);
        geo.wYRel  = yOfWidth(wRel);
    }

    geo.valid = true;
    return geo;
}

//==============================================================================
juce::Point<float> EnvelopeEditorComponent::handlePos(Handle h, const Geometry& geo) const
{
    auto shapedHalf = [](float v0, float v1, float curve, float yTop, float yBot)
    {
        const float vv = v0 + (v1 - v0) * lux_env_shape(0.5f, curve);
        return yBot + vv * (yTop - yBot);
    };
    switch (h)
    {
        case Handle::Attack:  return { geo.xA, geo.aYPeak };
        case Handle::Decay:   return { geo.xD, geo.aYSus  };
        case Handle::Sustain: return { geo.xD + geo.susW * 0.5f, geo.aYSus };
        case Handle::Release: return { geo.xR, geo.aYBase };
        case Handle::BendA:   return { 0.5f * (geo.xStart + geo.xA),
                                       shapedHalf(0.0f, 1.0f, aCurve.value, geo.aYPeak, geo.aYBase) };
        case Handle::BendD:   return { 0.5f * (geo.xA + geo.xD),
                                       shapedHalf(1.0f, 0.0f, dCurve.value, geo.aYPeak, geo.aYSus) };
        case Handle::BendR:   return { 0.5f * (geo.xSusEnd + geo.xR),
                                       shapedHalf(1.0f, 0.0f, rCurve.value, geo.aYSus, geo.aYBase) };
        case Handle::WAttack: return { 0.5f * (geo.xStart + geo.xA), geo.wYAtk };
        case Handle::WBase:   return { 0.5f * (geo.xD + geo.xSusEnd), geo.wYBase };
        case Handle::WRelease:return { geo.xR, geo.wYRel };
        case Handle::None:
        default:              return {};
    }
}

EnvelopeEditorComponent::Handle
EnvelopeEditorComponent::handleAt(juce::Point<float> p, const Geometry& geo) const
{
    if (!geo.valid) return Handle::None;

    // Node + bend handles in the alpha lane, then width-lane nodes.
    std::initializer_list<Handle> handles = {
        Handle::Attack, Handle::Decay, Handle::Sustain, Handle::Release,
        Handle::BendA, Handle::BendD, Handle::BendR,
        Handle::WAttack, Handle::WBase, Handle::WRelease };

    Handle best = Handle::None;
    float  bestDist = kHitR;
    for (Handle h : handles)
    {
        if (!hasWidth && (h == Handle::WAttack || h == Handle::WBase || h == Handle::WRelease))
            continue;
        if (!hasCurve && (h == Handle::BendA || h == Handle::BendD || h == Handle::BendR))
            continue;
        if (isAR && (h == Handle::Decay || h == Handle::Sustain || h == Handle::BendD))
            continue;
        const float dist = p.getDistanceFrom(handlePos(h, geo));
        if (dist < bestDist) { bestDist = dist; best = h; }
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
        case Handle::BendA: case Handle::BendD: case Handle::BendR:
        case Handle::WAttack: case Handle::WBase: case Handle::WRelease:
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor); break;
        case Handle::None:
        default:
            setMouseCursor(juce::MouseCursor::NormalCursor); break;
    }
}

//==============================================================================
void EnvelopeEditorComponent::beginHandleGesture(Handle h)
{
    switch (h)
    {
        case Handle::Attack:  if (a.attach) a.attach->beginGesture(); break;
        case Handle::Decay:   if (d.attach) d.attach->beginGesture(); break;
        case Handle::Sustain: if (s.attach) s.attach->beginGesture(); break;
        case Handle::Release: if (r.attach) r.attach->beginGesture(); break;
        case Handle::BendA:   if (aCurve.attach) aCurve.attach->beginGesture(); break;
        case Handle::BendD:   if (dCurve.attach) dCurve.attach->beginGesture(); break;
        case Handle::BendR:   if (rCurve.attach) rCurve.attach->beginGesture(); break;
        case Handle::WAttack: if (wAtk.attach)  wAtk.attach->beginGesture();  break;
        case Handle::WBase:   if (wBase.attach) wBase.attach->beginGesture(); break;
        case Handle::WRelease:if (wRel.attach)  wRel.attach->beginGesture();  break;
        case Handle::None: default: break;
    }
}

void EnvelopeEditorComponent::endHandleGesture(Handle h)
{
    switch (h)
    {
        case Handle::Attack:  if (a.attach) a.attach->endGesture(); break;
        case Handle::Decay:   if (d.attach) d.attach->endGesture(); break;
        case Handle::Sustain: if (s.attach) s.attach->endGesture(); break;
        case Handle::Release: if (r.attach) r.attach->endGesture(); break;
        case Handle::BendA:   if (aCurve.attach) aCurve.attach->endGesture(); break;
        case Handle::BendD:   if (dCurve.attach) dCurve.attach->endGesture(); break;
        case Handle::BendR:   if (rCurve.attach) rCurve.attach->endGesture(); break;
        case Handle::WAttack: if (wAtk.attach)  wAtk.attach->endGesture();  break;
        case Handle::WBase:   if (wBase.attach) wBase.attach->endGesture(); break;
        case Handle::WRelease:if (wRel.attach)  wRel.attach->endGesture();  break;
        case Handle::None: default: break;
    }
}

void EnvelopeEditorComponent::applyDrag(Handle h, juce::Point<float> p, const Geometry& geo)
{
    // Bend handles: solve the curvature that puts the segment midpoint under the
    // cursor.  v0/v1 are the normalised segment endpoints (in [yBot,yTop]).
    auto bend = [&](Bound& curve, float v0, float v1, float yTop, float yBot)
    {
        const float span = (yTop - yBot);
        if (std::abs(span) < 1.0f) return;
        const float vv = juce::jlimit(0.0f, 1.0f, (p.y - yBot) / span);
        const float sTarget = (std::abs(v1 - v0) > 1e-3f) ? (vv - v0) / (v1 - v0) : 0.5f;
        if (curve.attach) curve.attach->setValueAsPartOfGesture(curveFromHalfValue(sTarget));
    };
    auto setWidth = [&](Bound& b, float y)
    {
        if (b.param == nullptr || b.attach == nullptr) return;
        const float n = juce::jlimit(0.0f, 1.0f,
            (geo.width.getBottom() - y) / juce::jmax(1.0f, geo.width.getHeight()));
        b.attach->setValueAsPartOfGesture(b.param->convertFrom0to1(n));
    };

    switch (h)
    {
        case Handle::Attack:
            if (a.attach) a.attach->setValueAsPartOfGesture(
                juce::jlimit(a.min, a.max, xToTime(p.x - geo.xStart, a.max, geo.segMaxW)));
            break;
        case Handle::Decay:
            if (d.attach) d.attach->setValueAsPartOfGesture(
                juce::jlimit(d.min, d.max, xToTime(p.x - geo.xA, d.max, geo.segMaxW)));
            break;
        case Handle::Sustain:
        {
            const float hgt = geo.aYBase - geo.aYPeak;
            const float lvl = hgt > 0.0f
                ? juce::jlimit(0.0f, 1.0f, (geo.aYBase - p.y) / hgt) : 0.0f;
            if (s.attach) s.attach->setValueAsPartOfGesture(lvl);
            break;
        }
        case Handle::Release:
            if (r.attach) r.attach->setValueAsPartOfGesture(
                juce::jlimit(r.min, r.max, xToTime(p.x - geo.xSusEnd, r.max, geo.segMaxW)));
            break;
        case Handle::BendA: bend(aCurve, 0.0f, 1.0f, geo.aYPeak, geo.aYBase); break;
        case Handle::BendD: bend(dCurve, 1.0f, 0.0f, geo.aYPeak, geo.aYSus);  break;
        case Handle::BendR: bend(rCurve, 1.0f, 0.0f, geo.aYSus,  geo.aYBase); break;
        case Handle::WAttack:  setWidth(wAtk,  p.y); break;
        case Handle::WBase:    setWidth(wBase, p.y); break;
        case Handle::WRelease: setWidth(wRel,  p.y); break;
        case Handle::None: default: break;
    }
}

//==============================================================================
void EnvelopeEditorComponent::mouseMove(const juce::MouseEvent& e)
{
    if (dragging != Handle::None) return;
    const Handle h = handleAt(e.position, computeGeometry());
    if (h != hovered) { hovered = h; repaint(); }
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
    beginHandleGesture(h);
    if (h != Handle::None) repaint();
}

void EnvelopeEditorComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging == Handle::None) return;
    const Geometry geo = computeGeometry();
    if (!geo.valid) return;
    applyDrag(dragging, e.position, geo);
}

void EnvelopeEditorComponent::mouseUp(const juce::MouseEvent& e)
{
    endHandleGesture(dragging);
    dragging = Handle::None;
    hovered  = handleAt(e.position, computeGeometry());
    updateCursor(hovered);
    repaint();
}

//==============================================================================
void EnvelopeEditorComponent::appendShapedSegment(juce::Path& p, float x0, float x1,
                                                  float v0, float v1, float curve,
                                                  float yTop, float yBot)
{
    constexpr int kSteps = 24;
    for (int i = 1; i <= kSteps; ++i)
    {
        const float ph = (float) i / (float) kSteps;
        const float vv = v0 + (v1 - v0) * lux_env_shape(ph, curve);
        const float x  = x0 + (x1 - x0) * ph;
        const float y  = yBot + vv * (yTop - yBot);
        p.lineTo(x, y);
    }
}

void EnvelopeEditorComponent::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff20202a));
    g.fillRoundedRectangle(bounds.reduced(0.5f), 4.0f);
    g.setColour(accent.withAlpha(0.25f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

    const Geometry geo = computeGeometry();
    if (!geo.valid) return;

    // ── Alpha lane ───────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0x14ffffff));
    g.drawHorizontalLine((int) geo.aYBase, geo.alpha.getX(), geo.alpha.getRight());
    g.setColour(juce::Colour(0x0cffffff));
    g.drawHorizontalLine((int) geo.aYSus, geo.xD, geo.xSusEnd);

    juce::Path env;
    env.startNewSubPath(geo.xStart, geo.aYBase);
    appendShapedSegment(env, geo.xStart, geo.xA, 0.0f, 1.0f, aCurve.value, geo.aYPeak, geo.aYBase);
    appendShapedSegment(env, geo.xA, geo.xD, 1.0f, 0.0f, dCurve.value, geo.aYPeak, geo.aYSus);
    env.lineTo(geo.xSusEnd, geo.aYSus);
    appendShapedSegment(env, geo.xSusEnd, geo.xR, 1.0f, 0.0f, rCurve.value, geo.aYSus, geo.aYBase);

    {
        juce::Path fill(env);
        fill.lineTo(geo.xStart, geo.aYBase);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(0.12f));
        g.fillPath(fill);
    }
    g.setColour(accent.withAlpha(0.9f));
    g.strokePath(env, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));

    // Alpha node + bend handles
    for (Handle h : { Handle::Attack, Handle::Decay, Handle::Sustain, Handle::Release })
    {
        if (isAR && (h == Handle::Decay || h == Handle::Sustain)) continue;
        const auto pt     = handlePos(h, geo);
        const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
        const float rad   = active ? kNodeR + 1.5f : kNodeR;
        if (active)
        {
            g.setColour(accent.withAlpha(0.25f));
            g.fillEllipse(pt.x - rad - 2.5f, pt.y - rad - 2.5f, 2 * (rad + 2.5f), 2 * (rad + 2.5f));
        }
        g.setColour(active ? accent.brighter(0.3f) : juce::Colour(0xff20202a));
        g.fillEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad);
        g.setColour(active ? juce::Colours::white : accent.withAlpha(0.9f));
        g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, 1.4f);
    }
    if (hasCurve)
        for (Handle h : { Handle::BendA, Handle::BendD, Handle::BendR })
        {
            if (isAR && h == Handle::BendD) continue;
            const auto pt     = handlePos(h, geo);
            const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
            const float rad   = active ? kBendR + 1.2f : kBendR;
            g.setColour(active ? juce::Colours::white : accent.withAlpha(0.55f));
            g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, active ? 1.6f : 1.2f);
        }

    // ── Width lane (MASK) ─────────────────────────────────────────────────────
    if (hasWidth)
    {
        g.setColour(juce::Colour(0x10ffffff));
        g.drawHorizontalLine((int) geo.width.getBottom(), geo.width.getX(), geo.width.getRight());

        juce::Path wp;
        wp.startNewSubPath(geo.xStart, geo.wYAtk);
        wp.lineTo(geo.xA,      geo.wYAtk);
        wp.lineTo(geo.xD,      geo.wYBase);
        wp.lineTo(geo.xSusEnd, geo.wYBase);
        wp.lineTo(geo.xR,      geo.wYRel);
        g.setColour(accent.withAlpha(0.75f));
        g.strokePath(wp, juce::PathStrokeType(1.4f, juce::PathStrokeType::mitered,
                                              juce::PathStrokeType::rounded));

        g.setColour(accent.withAlpha(0.4f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("WIDTH", (int) geo.width.getX(), (int) widthLaneRect_.getY() + 1,
                   42, 9, juce::Justification::centredLeft, false);

        for (Handle h : { Handle::WAttack, Handle::WBase, Handle::WRelease })
        {
            const auto pt     = handlePos(h, geo);
            const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
            const float rad   = active ? kNodeR + 1.0f : kNodeR - 0.5f;
            g.setColour(active ? accent.brighter(0.3f) : juce::Colour(0xff20202a));
            g.fillEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad);
            g.setColour(active ? juce::Colours::white : accent.withAlpha(0.9f));
            g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, 1.3f);
        }
    }

    // ── Readout near the handle while dragging ─────────────────────────────────
    if (dragging != Handle::None)
    {
        juce::String txt;
        switch (dragging)
        {
            case Handle::Attack:  txt = "A " + formatTime(a.value); break;
            case Handle::Decay:   txt = "D " + formatTime(d.value); break;
            case Handle::Sustain: txt = "S " + juce::String(juce::roundToInt(s.value * 100.0f)) + " %"; break;
            case Handle::Release: txt = "R " + formatTime(r.value); break;
            case Handle::BendA:   txt = "A curve " + juce::String(aCurve.value, 2); break;
            case Handle::BendD:   txt = "D curve " + juce::String(dCurve.value, 2); break;
            case Handle::BendR:   txt = "R curve " + juce::String(rCurve.value, 2); break;
            case Handle::WAttack: txt = juce::String(juce::roundToInt(wAtk.value))  + " px"; break;
            case Handle::WBase:   txt = juce::String(juce::roundToInt(wBase.value)) + " px"; break;
            case Handle::WRelease:txt = juce::String(juce::roundToInt(wRel.value))  + " px"; break;
            case Handle::None: default: break;
        }
        const auto pt = handlePos(dragging, geo);
        const int  w  = 76;
        const int  x  = juce::jlimit((int) bounds.getX() + 2, (int) bounds.getRight() - w - 2,
                                     (int) pt.x - w / 2);
        const int  y  = (pt.y - geo.alpha.getY() < 14.0f) ? (int) pt.y + 8 : (int) pt.y - 16;
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText(txt, x, y, w, 12, juce::Justification::centred, false);
    }

    // ── Box labels ─────────────────────────────────────────────────────────────
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
    g.setColour(accent.withAlpha(0.6f));
    auto label = [&g](const juce::Slider& box, const juce::String& t)
    {
        auto b = box.getBounds();
        g.drawText(t, b.getX(), b.getY() - kLabelH, b.getWidth(), kLabelH,
                   juce::Justification::centred, false);
    };
    label(boxA, "Atck");
    if (!isAR) { label(boxD, "Dcay"); label(boxS, "Sus"); }
    label(boxR, "Rel");
    if (hasWidth)
    {
        label(boxWAtk, "W @ Atk"); label(boxW, "Width"); label(boxWRel, "W @ Rel");
    }
}
