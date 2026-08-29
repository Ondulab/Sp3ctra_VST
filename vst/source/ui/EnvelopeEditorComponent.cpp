#include "EnvelopeEditorComponent.h"
#include "Sp3ctraHandles.h"
#include "../processing/lux_env_shape.h"
#include <cmath>

namespace
{
    // Handles are drawn by Sp3ctraHandles (kNodeR / kRingR); only the grab
    // radius and the graph → plot insets are local.
    constexpr float kHitR    = 11.0f;  // grab radius
    constexpr float kPadX    = 8.0f;   // graph → plot, sides
    constexpr float kLaneTop = ModuleChrome::kPlotInsetTop + Sp3ctraHandles::kNodeR;   // graph → plot, top: caption strip + a node on plot.y (halo included)
    constexpr float kLanePad = 6.0f;   // graph → plot, bottom
    constexpr float kSusFrac = 0.16f;  // fixed sustain plateau display width

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
    addAndMakeVisible(box);   // bars keep the handle colour (Sp3ctraBarSlider default)
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
    auto area = getLocalBounds();

    // Frame = the lane only; its box row (label strip + boxes) sits below it.
    // The width lane keeps a fixed frame height, the alpha lane takes the rest
    // (never below the 24 px graph the geometry needs).
    const int widthBlockH = hasWidth ? ModuleChrome::kEditorGap + kWidthFrameH
                                       + ModuleChrome::kBelowFrameH
                                     : 0;
    const int alphaFrameH = juce::jmax(2 * ModuleChrome::kFrameInset + 24,
                                       area.getHeight() - ModuleChrome::kBelowFrameH - widthBlockH);

    alphaFrame_ = area.removeFromTop(alphaFrameH).toFloat();
    area.removeFromTop(ModuleChrome::kRowGap);
    if (isAR) ModuleChrome::layoutBoxRow(area.removeFromTop(ModuleChrome::kBoxRowH), { &boxA, &boxR });
    else      ModuleChrome::layoutBoxRow(area.removeFromTop(ModuleChrome::kBoxRowH), { &boxA, &boxD, &boxS, &boxR });

    widthFrame_ = {};
    if (hasWidth)
    {
        area.removeFromTop(ModuleChrome::kEditorGap);
        widthFrame_ = area.removeFromTop(kWidthFrameH).toFloat();
        area.removeFromTop(ModuleChrome::kRowGap);
        ModuleChrome::layoutBoxRow(area.removeFromTop(ModuleChrome::kBoxRowH), { &boxWAtk, &boxW, &boxWRel });
    }
}

EnvelopeEditorComponent::Geometry EnvelopeEditorComponent::computeGeometry() const
{
    Geometry geo;
    const auto alphaGraph = ModuleChrome::graphOf(alphaFrame_);
    if (alphaGraph.getWidth() < 60.0f || alphaGraph.getHeight() < 24.0f)
        return geo;

    // Plot = the frame's graph minus the caption / peak-node clearance on top,
    // the baseline clearance below and the side padding.
    auto lanePlot = [](juce::Rectangle<float> graph)
    {
        return graph.reduced(kPadX, 0.0f).withTrimmedTop(kLaneTop).withTrimmedBottom(kLanePad);
    };
    geo.alpha = lanePlot(alphaGraph);
    geo.width = lanePlot(ModuleChrome::graphOf(widthFrame_));

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
    // ── Chrome: one frame per lane (caption top-left), box labels below ──────
    ModuleChrome::drawFrame  (g, alphaFrame_, accent);
    ModuleChrome::drawCaption(g, alphaFrame_, accent, "ENVELOPE");
    ModuleChrome::drawBoxLabel(g, boxA, accent, "Atck");
    if (!isAR)
    {
        ModuleChrome::drawBoxLabel(g, boxD, accent, "Dcay");
        ModuleChrome::drawBoxLabel(g, boxS, accent, "Sus");
    }
    ModuleChrome::drawBoxLabel(g, boxR, accent, "Rel");
    if (hasWidth)
    {
        ModuleChrome::drawFrame  (g, widthFrame_, accent);
        ModuleChrome::drawCaption(g, widthFrame_, accent, "WIDTH");
        ModuleChrome::drawBoxLabel(g, boxWAtk, accent, "W @ Atk");
        ModuleChrome::drawBoxLabel(g, boxW,    accent, "Width");
        ModuleChrome::drawBoxLabel(g, boxWRel, accent, "W @ Rel");
    }

    const Geometry geo = computeGeometry();
    if (!geo.valid) return;

    // Handle states — Hover and Drag are distinct: while one handle is dragged
    // no other one reads as hovered.
    auto handleState = [this](Handle h)
    {
        return Sp3ctraHandles::stateOf(h == dragging,
                                       dragging == Handle::None && h == hovered);
    };

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

    // Alpha nodes (filled) + bend rings — Sp3ctraHandles, lime.
    for (Handle h : { Handle::Attack, Handle::Decay, Handle::Sustain, Handle::Release })
    {
        if (isAR && (h == Handle::Decay || h == Handle::Sustain)) continue;
        Sp3ctraHandles::drawNode(g, handlePos(h, geo), handleState(h));
    }
    if (hasCurve)
        for (Handle h : { Handle::BendA, Handle::BendD, Handle::BendR })
        {
            if (isAR && h == Handle::BendD) continue;
            Sp3ctraHandles::drawRing(g, handlePos(h, geo), handleState(h));
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

        for (Handle h : { Handle::WAttack, Handle::WBase, Handle::WRelease })
            Sp3ctraHandles::drawNode(g, handlePos(h, geo), handleState(h));
    }

    // ── Readout next to the dragged handle (lime pill, kept inside its lane) ──
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
        const bool inWidthLane = dragging == Handle::WAttack || dragging == Handle::WBase
                              || dragging == Handle::WRelease;
        Sp3ctraHandles::drawReadout(g, txt, handlePos(dragging, geo),
                                    ModuleChrome::graphOf(inWidthLane ? widthFrame_ : alphaFrame_));
    }
}
