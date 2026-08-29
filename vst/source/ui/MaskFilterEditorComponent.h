/**
 * @file MaskFilterEditorComponent.h
 * @brief Interactive editor for the LuxMask bandpass filter (Width / Bias / Slope).
 *
 * Mirrors the EnvelopeEditorComponent feel: a ModuleChrome frame ("FILTER")
 * holding the graphic + its lime handles (Sp3ctraHandles), with the compact
 * numeric boxes in a row BELOW the frame — all bound to APVTS params
 * (host-automatable, MIDI-mappable).  The x-axis is the pitch offset from the
 * played note (note marker at centre); y is the reveal alpha.
 *
 *   • Left / Right edge handles  → drag the band edges → set Width + Offset.
 *   • Slope handle (right foot)   → drag horizontally → set the edge softness.
 *
 * A faint animated fill shows the LIVE reveal (ADSR openness) breathing inside
 * the editable full-open outline, so you both shape and monitor the filter.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleEditorChrome.h"
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraHandles.h"
#include "../processing/lux_mask.h"   // self-manages extern "C" linkage

class MaskFilterEditorComponent : public juce::Component,
                                  private juce::Timer
{
public:
    static constexpr int kFrameH     = 72;   // graphic frame (plot = plotOf(frame), 46 px)
    static constexpr int kPreferredH = kFrameH + ModuleChrome::kBelowFrameH;   // 108 — frame + box row below

    MaskFilterEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                              juce::Colour accentColour,
                              const juce::String& widthParamId,
                              const juce::String& offsetParamId,
                              const juce::String& slopeParamId)
        : apvts(apvtsIn), accent(accentColour)
    {
        setInstance(0, widthParamId, offsetParamId, slopeParamId);
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~MaskFilterEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the next setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  openness overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& widthParamId,
                     const juce::String& offsetParamId,
                     const juce::String& slopeParamId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        w.attach.reset(); o.attach.reset(); s.attach.reset();
        boxWAtt.reset(); boxOAtt.reset(); boxSAtt.reset();
        bind(w, widthParamId);
        bind(o, offsetParamId);
        bind(s, slopeParamId);
        initBox(boxW, widthParamId,  boxWAtt);
        initBox(boxO, offsetParamId, boxOAtt);
        initBox(boxS, slopeParamId,  boxSAtt);
        learnW_.reset(); learnO_.reset(); learnS_.reset();
        if (midiMap_ != nullptr)
        {
            learnW_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxW, widthParamId);
            learnO_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxO, offsetParamId);
            learnS_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxS, slopeParamId);
        }
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Frame = the graphic only; the Width / Offset / Slope boxes sit below
        // it (label strip + boxes — ModuleChrome::kBoxRowH).
        frameRect_ = area.removeFromTop(juce::jmax(2 * ModuleChrome::kFrameInset + 16,
                                               area.getHeight() - ModuleChrome::kBelowFrameH)).toFloat();
        area.removeFromTop(ModuleChrome::kRowGap);
        ModuleChrome::layoutBoxRow(area.removeFromTop(ModuleChrome::kBoxRowH), { &boxW, &boxO, &boxS });
    }

    void paint(juce::Graphics& g) override
    {
        // Chrome: frame + caption, box labels below the frame (module colour).
        ModuleChrome::drawFrame   (g, frameRect_, accent);
        ModuleChrome::drawCaption (g, frameRect_, accent, "FILTER");
        ModuleChrome::drawBoxLabel(g, boxW, accent, "Width");
        ModuleChrome::drawBoxLabel(g, boxO, accent, "Offset");
        ModuleChrome::drawBoxLabel(g, boxS, accent, "Slope");

        const Geometry geo = computeGeometry();
        if (!geo.valid) return;

        // Note anchor marker.
        g.setColour(accent.withAlpha(0.30f));
        g.drawVerticalLine((int) geo.xN, geo.topY, geo.botY);

        // Live reveal fill (ADSR openness breathing inside the outline).
        const float liveOpen = liveOpenness();
        if (liveOpen > 0.001f)
        {
            juce::Path live = buildCurve(geo, liveOpen);
            g.setColour(accent.withAlpha(0.22f));
            g.fillPath(live);
        }

        // Editable full-open outline.
        juce::Path outline = buildCurve(geo, 1.0f);
        g.setColour(accent.withAlpha(0.07f));
        g.fillPath(outline);
        g.setColour(accent.withAlpha(0.85f));
        g.strokePath(outline, juce::PathStrokeType(1.5f));

        // Handles — Sp3ctraHandles (lime): filled nodes for the edges, a hollow
        // ring for the slope. Hover and Drag are distinct states.
        auto handleState = [this](Handle h)
        {
            return Sp3ctraHandles::stateOf(h == dragging,
                                           dragging == Handle::None && h == hovered);
        };
        for (Handle h : { Handle::LeftEdge, Handle::RightEdge })
            Sp3ctraHandles::drawNode(g, handlePos(h, geo), handleState(h));
        Sp3ctraHandles::drawRing(g, handlePos(Handle::Slope, geo), handleState(Handle::Slope));
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != Handle::None) return;
        const Handle h = handleAt(e.position, computeGeometry());
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == Handle::None ? juce::MouseCursor::NormalCursor
                                         : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == Handle::None && hovered != Handle::None) { hovered = Handle::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const Geometry geo = computeGeometry();
        dragging = handleAt(e.position, geo);
        hovered  = dragging;
        if (dragging == Handle::LeftEdge)  { dragFixedOff_ = geo.hiOff; w.begin(); o.begin(); }
        else if (dragging == Handle::RightEdge) { dragFixedOff_ = geo.loOff; w.begin(); o.begin(); }
        else if (dragging == Handle::Slope) { s.begin(); }
        if (dragging != Handle::None) repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const Geometry geo = computeGeometry();
        if (!geo.valid || dragging == Handle::None) return;
        applyDrag(dragging, e.position, geo);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::LeftEdge || dragging == Handle::RightEdge) { w.end(); o.end(); }
        else if (dragging == Handle::Slope) { s.end(); }
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, LeftEdge, RightEdge, Slope };

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float xN = 0, topY = 0, botY = 0, midY = 0;
        float scale = 1;          // image-px -> screen-px
        float Nimg = 1;           // image width (px)
        float loOff = 0, hiOff = 0;   // full-open band edge offsets from note (image px)
        float softImg = 1;        // soft-edge half-width (image px)
        bool  valid = false;
    };

    Geometry computeGeometry() const
    {
        Geometry geo;
        const auto graph = ModuleChrome::graphOf(frameRect_);
        if (graph.getWidth() < 30.0f || graph.getHeight() < 16.0f) return geo;

        geo.plot = ModuleChrome::plotOf(frameRect_);
        float N = (float) lux_mask_instance(slot_)->last_pixel_count;
        if (N <= 0.0f) N = (float) (LUX_MASK_MAX_PIXELS / 2);
        geo.Nimg = N;

        const float halfSpanImg = 0.5f * N;                 // axis covers +/-50% of image
        geo.scale = (geo.plot.getWidth() * 0.5f) / halfSpanImg;
        geo.xN    = geo.plot.getCentreX();
        geo.topY  = geo.plot.getY();
        geo.botY  = geo.plot.getBottom();
        geo.midY  = 0.5f * (geo.topY + geo.botY);

        const float offImg = juce::jlimit(-100.0f, 100.0f, o.value) * 0.01f * N;
        const float fullW  = juce::jlimit(0.0f, 100.0f, w.value) * 0.01f * N;
        geo.loOff = offImg - fullW * 0.5f;
        geo.hiOff = offImg + fullW * 0.5f;
        geo.softImg = (1.0f - juce::jlimit(0.0f, 1.0f, s.value)) * 0.15f * N + 1.0f;
        geo.valid = true;
        return geo;
    }

    /* alpha(x) curve for a given openness (1 = the editable outline). */
    juce::Path buildCurve(const Geometry& geo, float openness) const
    {
        const float lo = geo.loOff * openness;
        const float hi = geo.hiOff * openness;
        const float invSoft = 1.0f / geo.softImg;
        const int   W = juce::jmax(2, (int) geo.plot.getWidth());

        juce::Path p;
        p.startNewSubPath(geo.plot.getX(), geo.botY);
        for (int px = 0; px <= W; ++px)
        {
            const float x    = geo.plot.getX() + (float) px;
            const float iOff = (x - geo.xN) / geo.scale;
            float a = gate((iOff - lo) * invSoft) * gate((hi - iOff) * invSoft);
            a = juce::jlimit(0.0f, 1.0f, a);
            p.lineTo(x, geo.botY - a * (geo.botY - geo.topY));
        }
        p.lineTo(geo.plot.getRight(), geo.botY);
        p.closeSubPath();
        return p;
    }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        // Pin handles inside the plot so they stay grabbable even when the band
        // (or the soft foot) extends beyond the visible axis — the user can then
        // always drag them back inward.
        const float xMin = geo.plot.getX()     + kNodeR;
        const float xMax = geo.plot.getRight()  - kNodeR;
        const float yMin = geo.plot.getY()     + kNodeR;
        const float yMax = geo.plot.getBottom() - kNodeR;
        auto clampPt = [&](float x, float y) {
            return juce::Point<float>(juce::jlimit(xMin, xMax, x),
                                      juce::jlimit(yMin, yMax, y));
        };
        switch (h)
        {
            case Handle::LeftEdge:  return clampPt(geo.xN + geo.loOff * geo.scale, geo.midY);
            case Handle::RightEdge: return clampPt(geo.xN + geo.hiOff * geo.scale, geo.midY);
            case Handle::Slope:
                // Sits on the right rolloff itself: at iOff = hi + soft the
                // curve alpha is gate(-1) ≈ 0.12, so the handle rides the flank.
                return clampPt(geo.xN + (geo.hiOff + geo.softImg) * geo.scale,
                               geo.botY - gate(-1.0f) * (geo.botY - geo.topY));
            default: return {};
        }
    }

    Handle handleAt(juce::Point<float> p, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        Handle best = Handle::None;
        float bestD = kHitR;
        for (Handle h : { Handle::LeftEdge, Handle::RightEdge, Handle::Slope })
        {
            const float d = p.getDistanceFrom(handlePos(h, geo));
            if (d < bestD) { bestD = d; best = h; }
        }
        return best;
    }

    void applyDrag(Handle h, juce::Point<float> p, const Geometry& geo)
    {
        if (h == Handle::Slope)
        {
            const float softScreen = juce::jmax(1.0f, p.x - (geo.xN + geo.hiOff * geo.scale));
            const float softImg    = softScreen / geo.scale;
            const float slope = juce::jlimit(0.0f, 1.0f, 1.0f - (softImg - 1.0f) / (0.15f * geo.Nimg));
            s.setGesture(slope);
            return;
        }

        // Edge drag: move one edge, hold the opposite edge fixed (captured at
        // mouseDown), then solve Width + Bias from the resulting offsets.  Edges
        // may cross the note (centre): that pushes the whole band off one side
        // (Bias beyond +/-1) for the glide-like swept offset.
        float loOff, hiOff;
        if (h == Handle::LeftEdge)
        {
            loOff = (p.x - geo.xN) / geo.scale;
            hiOff = dragFixedOff_;
        }
        else // RightEdge
        {
            hiOff = (p.x - geo.xN) / geo.scale;
            loOff = dragFixedOff_;
        }

        const float Wimg = hiOff - loOff;
        if (Wimg < 1.0f) return;            // left must stay left of right
        const float widthPct  = juce::jlimit(0.0f, 100.0f, Wimg / geo.Nimg * 100.0f);
        const float offsetPct = juce::jlimit(-100.0f, 100.0f,
                                             (loOff + hiOff) * 0.5f / geo.Nimg * 100.0f);
        w.setGesture(widthPct);
        o.setGesture(offsetPct);
    }

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    static inline float gate(float x)
    {
        if (x >  4.0f) return 1.0f;
        if (x < -4.0f) return 0.0f;
        return 0.5f * (1.0f + std::tanh(x));
    }

    /* Most-open alive voice → live openness for the breathing fill. */
    float liveOpenness() const
    {
        const LuxMaskState& st = *lux_mask_instance(slot_);
        const auto& cfg = st.config;
        const int maxV = cfg.polyphony_enabled ? LUX_MASK_MAX_VOICES : 1;
        float best = 0.0f;
        for (int v = 0; v < maxV; ++v)
        {
            const auto& vs = st.voices[v];
            if (vs.envelope_stage == LUX_MASK_ENV_IDLE) continue;
            best = juce::jmax(best, juce::jlimit(0.0f, 1.0f, vs.envelope_level));
        }
        return best;
    }

    //==========================================================================
    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float value = 0.0f;
        void begin()            { if (attach) attach->beginGesture(); }
        void end()              { if (attach) attach->endGesture(); }
        void setGesture(float v){ if (attach) attach->setValueAsPartOfGesture(v); }
    };

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.param = apvts.getParameter(id);
        jassert(bnd.param != nullptr);
        if (bnd.param == nullptr) return;
        bnd.attach = std::make_unique<juce::ParameterAttachment>(
            *bnd.param, [this, &bnd](float v) { bnd.value = v; repaint(); });
        bnd.attach->sendInitialUpdate();
    }

    void initBox(Sp3ctraBarSlider& box, const juce::String& id,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
    {
        addAndMakeVisible(box);   // bars keep the handle colour (Sp3ctraBarSlider default)
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kNodeR = Sp3ctraHandles::kNodeR;   // drawn node radius (pin margin)
    static constexpr float kHitR  = 12.0f;                    // grab radius

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound w, o, s;   // width%, offset%, slope
    Sp3ctraBarSlider boxW, boxO, boxS;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxWAtt, boxOAtt, boxSAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnW_, learnO_, learnS_;

    juce::Rectangle<float> frameRect_;   // graphic frame (ModuleChrome); plot = plotOf(frameRect_)
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };
    float  dragFixedOff_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MaskFilterEditorComponent)
};
