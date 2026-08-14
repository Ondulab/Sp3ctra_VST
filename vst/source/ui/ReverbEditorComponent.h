/**
 * @file ReverbEditorComponent.h
 * @brief Interactive editor for the LuxReverb tail (Decay / Diffusion / Mix).
 *
 * Mirrors the MaskFilterEditorComponent feel: a graphic with draggable handles
 * plus compact numeric boxes, all bound to APVTS params (host-automatable,
 * MIDI-mappable).  The x-axis is time (skewed like the Decay param), y is the
 * tail level; the dry impulse sits at t = 0.
 *
 *   • Mix node (left, filled)    → drag vertically → wet level of the tail.
 *   • Decay node (bottom, filled)→ drag horizontally → -60 dB point.
 *   • Diffusion glow around the curve is set from its numeric box.
 *
 * A faint animated fill shows the LIVE tail energy (read from the slot-0 pool
 * instance, like the Pitch/Mask pages) breathing inside the response curve.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_reverb.h"   // self-manages extern "C" linkage

class ReverbEditorComponent : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr int kPreferredH = 110;  // graphic + box row

    ReverbEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                          juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxreverb{slot}_*).
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~ReverbEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  tail overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& decayId,
                     const juce::String& diffusionId,
                     const juce::String& mixId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        dcy.attach.reset(); dif.attach.reset(); mix.attach.reset();
        boxDAtt.reset(); boxFAtt.reset(); boxMAtt.reset();
        bind(dcy, decayId);
        bind(dif, diffusionId);
        bind(mix, mixId);
        initBox(boxD, decayId,     boxDAtt);
        initBox(boxF, diffusionId, boxFAtt);
        initBox(boxM, mixId,       boxMAtt);
        learnD_.reset(); learnF_.reset(); learnM_.reset();
        if (midiMap_ != nullptr)
        {
            learnD_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxD, decayId);
            learnF_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxF, diffusionId);
            learnM_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxM, mixId);
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
        boxD.setBounds(row.getX(),                  row.getY(), bw, kBoxH);
        boxF.setBounds(row.getX() + (bw + gap),     row.getY(), bw, kBoxH);
        boxM.setBounds(row.getX() + 2 * (bw + gap), row.getY(), bw, kBoxH);
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
            // Dry impulse at t = 0.
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(geo.x0 - 1.0f, geo.topY, 2.0f, geo.botY - geo.topY));

            // -60 dB time marker.
            g.setColour(accent.withAlpha(0.28f));
            const float xd = xForTime(geo, geo.decayS);
            for (float y = geo.topY; y < geo.botY; y += 6.0f)
                g.fillRect(juce::Rectangle<float>(xd - 0.5f, y, 1.0f, 3.0f));

            // Diffusion glow — widening echoes of the curve.
            const juce::Path curve = buildCurve(geo);
            const int glowLayers = (int) std::round(geo.diffusion * 4.0f);
            for (int i = 1; i <= glowLayers; ++i)
            {
                g.setColour(accent.withAlpha(0.10f * geo.diffusion));
                g.strokePath(curve, juce::PathStrokeType(1.5f + 2.6f * (float) i));
            }

            // Live tail energy breathing inside the response.
            const float live = liveTailLevel();
            if (live > 0.01f)
            {
                juce::Path fill = curve;
                fill.lineTo(geo.plot.getRight(), geo.botY);
                fill.lineTo(geo.x0, geo.botY);
                fill.closeSubPath();
                g.setColour(accent.withAlpha(0.30f * live));
                g.fillPath(fill);
            }

            // Response outline.
            g.setColour(accent.withAlpha(0.07f));
            {
                juce::Path fill = curve;
                fill.lineTo(geo.plot.getRight(), geo.botY);
                fill.lineTo(geo.x0, geo.botY);
                fill.closeSubPath();
                g.fillPath(fill);
            }
            g.setColour(accent.withAlpha(0.85f));
            g.strokePath(curve, juce::PathStrokeType(1.5f));

            // Handles.
            drawNode(g, handlePos(Handle::Mix,   geo), Handle::Mix);
            drawNode(g, handlePos(Handle::Decay, geo), Handle::Decay);
        }

        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("TAIL", (int) bounds.getX() + 8, (int) bounds.getY() + 2,
                   90, 9, juce::Justification::centredLeft, false);

        g.setColour(accent.withAlpha(0.6f));
        auto label = [&g](const juce::Slider& box, const juce::String& t)
        {
            auto bb = box.getBounds();
            g.drawText(t, bb.getX(), bb.getY() - kLabelH, bb.getWidth(), kLabelH,
                       juce::Justification::centred, false);
        };
        label(boxD, "Decay"); label(boxF, "Diffusion"); label(boxM, "Mix");
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != Handle::None) return;
        const Handle h = handleAt(e.position, computeGeometry());
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == Handle::None  ? juce::MouseCursor::NormalCursor
                     : h == Handle::Decay ? juce::MouseCursor::LeftRightResizeCursor
                                          : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == Handle::None && hovered != Handle::None) { hovered = Handle::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Mix)   mix.begin();
        if (dragging == Handle::Decay) dcy.begin();
        if (dragging != Handle::None) repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const Geometry geo = computeGeometry();
        if (!geo.valid) return;
        if (dragging == Handle::Mix)
        {
            const float m = (geo.botY - e.position.y) / juce::jmax(1.0f, geo.botY - geo.topY);
            mix.setGesture(juce::jlimit(0.0f, 100.0f, m * 100.0f));
        }
        else if (dragging == Handle::Decay)
        {
            dcy.setGesture(juce::jlimit(0.1f, 20.0f, timeForX(geo, e.position.x)));
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::Mix)   mix.end();
        if (dragging == Handle::Decay) dcy.end();
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, Mix, Decay };

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float x0 = 0, topY = 0, botY = 0;
        float decayS = 3, diffusion = 0, mixN = 0;   // s, 0..1, 0..1
        bool  valid = false;
    };

    static constexpr float kMaxT   = 20.0f;   // axis end = Decay param max (s)
    static constexpr float kSkew   = 0.4f;    // same skew as the Decay param

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot      = graphRect_.reduced(8.0f, 7.0f);
        geo.x0        = geo.plot.getX() + 4.0f;
        geo.topY      = geo.plot.getY();
        geo.botY      = geo.plot.getBottom();
        geo.decayS    = juce::jlimit(0.1f, kMaxT, dcy.value);
        geo.diffusion = juce::jlimit(0.0f, 1.0f, dif.value / 100.0f);
        geo.mixN      = juce::jlimit(0.0f, 1.0f, mix.value / 100.0f);
        geo.valid     = true;
        return geo;
    }

    float xForTime(const Geometry& geo, float t) const
    {
        const float n = std::pow(juce::jlimit(0.0f, 1.0f, t / kMaxT), kSkew);
        return geo.x0 + n * (geo.plot.getRight() - geo.x0);
    }

    float timeForX(const Geometry& geo, float x) const
    {
        const float n = juce::jlimit(0.0f, 1.0f,
                                     (x - geo.x0) / juce::jmax(1.0f, geo.plot.getRight() - geo.x0));
        return kMaxT * std::pow(n, 1.0f / kSkew);
    }

    /* level(t) = mix * 10^(-3t / decay) — the tail response. */
    juce::Path buildCurve(const Geometry& geo) const
    {
        const int W = juce::jmax(2, (int) (geo.plot.getRight() - geo.x0));
        juce::Path p;
        p.startNewSubPath(geo.x0, geo.botY - geo.mixN * (geo.botY - geo.topY));
        for (int px = 1; px <= W; ++px)
        {
            const float x = geo.x0 + (float) px;
            const float t = timeForX(geo, x);
            const float lvl = geo.mixN * std::pow(10.0f, -3.0f * t / geo.decayS);
            p.lineTo(x, geo.botY - lvl * (geo.botY - geo.topY));
        }
        return p;
    }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        switch (h)
        {
            case Handle::Mix:
                return { geo.x0 + 6.0f,
                         juce::jlimit(geo.topY + kNodeR, geo.botY - kNodeR,
                                      geo.botY - geo.mixN * (geo.botY - geo.topY)) };
            case Handle::Decay:
                return { juce::jlimit(geo.plot.getX() + kNodeR, geo.plot.getRight() - kNodeR,
                                      xForTime(geo, geo.decayS)),
                         geo.botY - kNodeR };
            default: return {};
        }
    }

    Handle handleAt(juce::Point<float> p, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        Handle best = Handle::None;
        float bestD = kHitR;
        for (Handle h : { Handle::Mix, Handle::Decay })
        {
            const float d = p.getDistanceFrom(handlePos(h, geo));
            if (d < bestD) { bestD = d; best = h; }
        }
        return best;
    }

    void drawNode(juce::Graphics& g, juce::Point<float> pt, Handle h)
    {
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

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    /* Peak of the bound instance's live tail (strided scan — display only). */
    float liveTailLevel() const
    {
        const LuxReverbState& st = *lux_reverb_instance(slot_);
        if (! st.config.enabled || ! st.tail_active) return 0.0f;
        const int n = juce::jlimit(0, LUX_REVERB_MAX_PIXELS, st.last_pixel_count);
        float peak = 0.0f;
        for (int i = 0; i < n; i += 32)
        {
            peak = juce::jmax(peak, st.tail_r[i]);
            peak = juce::jmax(peak, st.tail_g[i]);
            peak = juce::jmax(peak, st.tail_b[i]);
        }
        return juce::jlimit(0.0f, 1.0f, peak / 255.0f);
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
        box.setAccent(accent);
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kNodeR = 4.5f;
    static constexpr float kHitR  = 12.0f;
    static constexpr int   kBoxH   = 16;
    static constexpr int   kLabelH = 9;
    static constexpr int   kRowGap = 3;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound dcy, dif, mix;
    Sp3ctraBarSlider boxD, boxF, boxM;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxDAtt, boxFAtt, boxMAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnD_, learnF_, learnM_;

    juce::Rectangle<float> graphRect_;
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbEditorComponent)
};
