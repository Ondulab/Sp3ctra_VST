/**
 * @file EchoEditorComponent.h
 * @brief Interactive editor for the LuxEcho repeats (Delay / Feedback / Mix).
 *
 * Mirrors the MaskFilterEditorComponent feel: a graphic with draggable handles
 * plus compact numeric boxes, all bound to APVTS params (host-automatable,
 * MIDI-mappable).  The x-axis is the line offset (0..255); the dry impulse
 * sits at x = 0 and each repeat appears at n × delay with level mix·fb^(n-1).
 *
 *   • Repeat-1 node (filled)  → drag → Delay (x) + Mix (y).
 *   • Repeat-2 node (hollow)  → drag vertically → Feedback (its level / mix).
 *
 * The repeat bars brighten while the slot-0 pool instance is actually
 * processing a stream (ring active), so you see the module living.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_echo.h"   // self-manages extern "C" linkage

class EchoEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    static constexpr int kPreferredH = 110;  // graphic + box row

    EchoEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                        juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxecho{slot}_*).
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~EchoEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  activity overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& delayId,
                     const juce::String& feedbackId,
                     const juce::String& mixId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        del.attach.reset(); fbk.attach.reset(); mix.attach.reset();
        boxDAtt.reset(); boxFAtt.reset(); boxMAtt.reset();
        bind(del, delayId);
        bind(fbk, feedbackId);
        bind(mix, mixId);
        initBox(boxD, delayId,    boxDAtt);
        initBox(boxF, feedbackId, boxFAtt);
        initBox(boxM, mixId,      boxMAtt);
        learnD_.reset(); learnF_.reset(); learnM_.reset();
        if (midiMap_ != nullptr)
        {
            learnD_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxD, delayId);
            learnF_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxF, feedbackId);
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
            const LuxEchoState& st = *lux_echo_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.ring_active != 0);

            // Dry impulse at line 0.
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(geo.x0 - 1.0f, geo.topY, 2.5f, geo.botY - geo.topY));

            // Repeat train: level(n) = mix * fb^(n-1) at x = n * delay.
            for (int n = 1; ; ++n)
            {
                const float h = repeatLevel(geo, n);
                const float x = xForLines(geo, (float) n * geo.delayL);
                if (h < 0.015f || x > geo.plot.getRight()) break;
                const float barTop = geo.botY - h * (geo.botY - geo.topY);
                g.setColour(accent.withAlpha(live ? 0.85f : 0.45f));
                g.fillRoundedRectangle(x - 1.5f, barTop, 3.0f, geo.botY - barTop, 1.5f);
            }

            // Decaying envelope hint through the repeat tops.
            if (geo.fbN > 0.01f && geo.mixN > 0.01f)
            {
                juce::Path env;
                bool started = false;
                for (int n = 1; n <= 64; ++n)
                {
                    const float h = repeatLevel(geo, n);
                    const float x = xForLines(geo, (float) n * geo.delayL);
                    if (x > geo.plot.getRight()) break;
                    const float y = geo.botY - h * (geo.botY - geo.topY);
                    if (! started) { env.startNewSubPath(x, y); started = true; }
                    else             env.lineTo(x, y);
                }
                if (started)
                {
                    g.setColour(accent.withAlpha(0.30f));
                    g.strokePath(env, juce::PathStrokeType(1.0f));
                }
            }

            drawNode(g, handlePos(Handle::Repeat1, geo), Handle::Repeat1, /*hollow*/ false);
            drawNode(g, handlePos(Handle::Repeat2, geo), Handle::Repeat2, /*hollow*/ true);
        }

        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("REPEATS", (int) bounds.getX() + 8, (int) bounds.getY() + 2,
                   90, 9, juce::Justification::centredLeft, false);

        g.setColour(accent.withAlpha(0.6f));
        auto label = [&g](const juce::Slider& box, const juce::String& t)
        {
            auto bb = box.getBounds();
            g.drawText(t, bb.getX(), bb.getY() - kLabelH, bb.getWidth(), kLabelH,
                       juce::Justification::centred, false);
        };
        label(boxD, "Delay"); label(boxF, "Feedback"); label(boxM, "Mix");
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != Handle::None) return;
        const Handle h = handleAt(e.position, computeGeometry());
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == Handle::None    ? juce::MouseCursor::NormalCursor
                     : h == Handle::Repeat2 ? juce::MouseCursor::UpDownResizeCursor
                                            : juce::MouseCursor::DraggingHandCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == Handle::None && hovered != Handle::None) { hovered = Handle::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Repeat1) { del.begin(); mix.begin(); }
        if (dragging == Handle::Repeat2) { fbk.begin(); }
        if (dragging != Handle::None) repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const Geometry geo = computeGeometry();
        if (!geo.valid) return;
        if (dragging == Handle::Repeat1)
        {
            del.setGesture(juce::jlimit(1.0f, (float) LUX_ECHO_MAX_DELAY,
                                        std::round(linesForX(geo, e.position.x))));
            const float m = (geo.botY - e.position.y) / juce::jmax(1.0f, geo.botY - geo.topY);
            mix.setGesture(juce::jlimit(0.0f, 100.0f, m * 100.0f));
        }
        else if (dragging == Handle::Repeat2)
        {
            // Feedback from the repeat-2 level: h2 = mix * fb  →  fb = h2 / mix.
            const float h2 = juce::jlimit(0.0f, 1.0f,
                (geo.botY - e.position.y) / juce::jmax(1.0f, geo.botY - geo.topY));
            const float fb = h2 / juce::jmax(0.02f, geo.mixN);
            fbk.setGesture(juce::jlimit(0.0f, 95.0f, fb * 100.0f));
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::Repeat1) { del.end(); mix.end(); }
        if (dragging == Handle::Repeat2) { fbk.end(); }
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, Repeat1, Repeat2 };

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float x0 = 0, topY = 0, botY = 0;
        float delayL = 48, fbN = 0, mixN = 0;   // lines, 0..1, 0..1
        bool  valid = false;
    };

    static constexpr float kMaxLines = (float) LUX_ECHO_MAX_DELAY + 17.0f;  // headroom past 255

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot   = graphRect_.reduced(8.0f, 7.0f);
        geo.x0     = geo.plot.getX() + 4.0f;
        geo.topY   = geo.plot.getY();
        geo.botY   = geo.plot.getBottom();
        geo.delayL = juce::jlimit(1.0f, (float) LUX_ECHO_MAX_DELAY, del.value);
        geo.fbN    = juce::jlimit(0.0f, 0.95f, fbk.value / 100.0f);
        geo.mixN   = juce::jlimit(0.0f, 1.0f,  mix.value / 100.0f);
        geo.valid  = true;
        return geo;
    }

    float xForLines(const Geometry& geo, float lines) const
    { return geo.x0 + (lines / kMaxLines) * (geo.plot.getRight() - geo.x0); }

    float linesForX(const Geometry& geo, float x) const
    {
        return kMaxLines * juce::jlimit(0.0f, 1.0f,
            (x - geo.x0) / juce::jmax(1.0f, geo.plot.getRight() - geo.x0));
    }

    float repeatLevel(const Geometry& geo, int n) const
    { return geo.mixN * std::pow(geo.fbN, (float) (n - 1)); }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        const int   n = (h == Handle::Repeat1) ? 1 : 2;
        const float x = juce::jlimit(geo.plot.getX() + kNodeR, geo.plot.getRight() - kNodeR,
                                     xForLines(geo, (float) n * geo.delayL));
        const float lvl = repeatLevel(geo, n);
        const float y = juce::jlimit(geo.topY + kNodeR, geo.botY - kNodeR,
                                     geo.botY - lvl * (geo.botY - geo.topY));
        return { x, y };
    }

    Handle handleAt(juce::Point<float> p, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        Handle best = Handle::None;
        float bestD = kHitR;
        for (Handle h : { Handle::Repeat1, Handle::Repeat2 })
        {
            const float d = p.getDistanceFrom(handlePos(h, geo));
            if (d < bestD) { bestD = d; best = h; }
        }
        return best;
    }

    void drawNode(juce::Graphics& g, juce::Point<float> pt, Handle h, bool hollow)
    {
        const bool active = (h == dragging) || (dragging == Handle::None && h == hovered);
        if (hollow)
        {
            const float rad = active ? kBendR + 1.2f : kBendR;
            g.setColour(active ? juce::Colours::white : accent.withAlpha(0.55f));
            g.drawEllipse(pt.x - rad, pt.y - rad, 2 * rad, 2 * rad, active ? 1.6f : 1.2f);
            return;
        }
        const float rad = active ? kNodeR + 1.5f : kNodeR;
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
    static constexpr float kBendR = 3.2f;
    static constexpr float kHitR  = 12.0f;
    static constexpr int   kBoxH   = 16;
    static constexpr int   kLabelH = 9;
    static constexpr int   kRowGap = 3;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound del, fbk, mix;
    Sp3ctraBarSlider boxD, boxF, boxM;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxDAtt, boxFAtt, boxMAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnD_, learnF_, learnM_;

    juce::Rectangle<float> graphRect_;
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoEditorComponent)
};
