/**
 * @file MaskFilterEditorComponent.h
 * @brief Interactive editor for the LuxMask bandpass filter (Width / Bias / Slope).
 *
 * Mirrors the EnvelopeEditorComponent feel: a graphic with draggable handles
 * plus compact numeric boxes, all bound to APVTS params (host-automatable,
 * MIDI-mappable).  The x-axis is the pitch offset from the played note (note
 * marker at centre); y is the reveal alpha.
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
#include "../processing/lux_mask.h"   // self-manages extern "C" linkage

class MaskFilterEditorComponent : public juce::Component,
                                  private juce::Timer
{
public:
    static constexpr int kPreferredH = 100;  // graphic + box row

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
        auto area = getLocalBounds().reduced(6);
        const int boxRowH = kLabelH + kBoxH;
        graphRect_ = area.removeFromTop(juce::jmax(24, area.getHeight() - boxRowH - kRowGap)).toFloat();
        area.removeFromTop(kRowGap);

        auto row = area.removeFromTop(boxRowH);
        row.removeFromTop(kLabelH);
        const int gap = 5, n = 3;
        const int bw = (row.getWidth() - (n - 1) * gap) / n;
        boxW.setBounds(row.getX(),                  row.getY(), bw, kBoxH);
        boxO.setBounds(row.getX() + (bw + gap),     row.getY(), bw, kBoxH);
        boxS.setBounds(row.getX() + 2 * (bw + gap), row.getY(), bw, kBoxH);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(bounds.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
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

            // Edge handles — filled "node" style (like the ADSR A/D/S/R nodes).
            for (Handle h : { Handle::LeftEdge, Handle::RightEdge })
            {
                const auto pt     = handlePos(h, geo);
                const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
                const float rad   = active ? kNodeR + 1.5f : kNodeR;
                if (active)
                {
                    g.setColour(accent.withAlpha(0.25f));
                    g.fillEllipse(pt.x - rad - 2.5f, pt.y - rad - 2.5f,
                                  2 * (rad + 2.5f), 2 * (rad + 2.5f));
                }
                g.setColour(active ? accent.brighter(0.3f) : juce::Colour(0xff20202a));
                g.fillEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad);
                g.setColour(active ? juce::Colours::white : accent.withAlpha(0.9f));
                g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, 1.4f);
            }

            // Slope handle — hollow "bend" style (like the ADSR curve handles).
            {
                const auto pt     = handlePos(Handle::Slope, geo);
                const bool active = (Handle::Slope == dragging)
                                  || (dragging == Handle::None && hovered == Handle::Slope);
                const float rad   = active ? kBendR + 1.2f : kBendR;
                g.setColour(active ? juce::Colours::white : accent.withAlpha(0.55f));
                g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, active ? 1.6f : 1.2f);
            }
        }

        // Title + box labels.
        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("FILTER", (int) bounds.getX() + 8, (int) bounds.getY() + 2,
                   90, 9, juce::Justification::centredLeft, false);

        g.setColour(accent.withAlpha(0.6f));
        auto label = [&g](const juce::Slider& box, const juce::String& t)
        {
            auto bb = box.getBounds();
            g.drawText(t, bb.getX(), bb.getY() - kLabelH, bb.getWidth(), kLabelH,
                       juce::Justification::centred, false);
        };
        label(boxW, "Width"); label(boxO, "Offset"); label(boxS, "Slope");
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
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;

        geo.plot = graphRect_.reduced(8.0f, 7.0f);
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

    void initBox(juce::Slider& box, const juce::String& id,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
    {
        box.setSliderStyle(juce::Slider::LinearBar);
        box.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 0, 0);
        box.setColour(juce::Slider::trackColourId, accent.withAlpha(0.22f));
        box.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff181820));
        box.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white.withAlpha(0.92f));
        box.setColour(juce::Slider::textBoxOutlineColourId, accent.withAlpha(0.3f));
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kNodeR = 4.5f;
    static constexpr float kBendR = 3.2f;   // hollow slope handle (ADSR-style)
    static constexpr float kHitR  = 12.0f;
    static constexpr int   kBoxH   = 16;
    static constexpr int   kLabelH = 9;
    static constexpr int   kRowGap = 3;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound w, o, s;   // width%, offset%, slope
    juce::Slider boxW, boxO, boxS;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxWAtt, boxOAtt, boxSAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnW_, learnO_, learnS_;

    juce::Rectangle<float> graphRect_;
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };
    float  dragFixedOff_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MaskFilterEditorComponent)
};
