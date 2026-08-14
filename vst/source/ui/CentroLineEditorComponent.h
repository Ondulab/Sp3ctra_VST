/**
 * @file CentroLineEditorComponent.h
 * @brief LINE SHAPE view for the CENTROID simplifier — the profile ONE
 *        redrawn barycentre line takes (Thickness / Edge), MASK-style.
 *
 * Same visual idiom as MaskFilterEditorComponent's FILTER window: a single
 * centred profile with edge handles, dedicated to the line SHAPE so the
 * stream view (CentroEditorComponent) can stay a clean floor-only monitor.
 * The profile is the REAL pivot geometry mirrored from lux_centro_redraw():
 * c = thickness/2, skirt = edge·max(c,1), plateau p = c−skirt, foot
 * h = c+skirt — the half-height point stays at c whatever the softness, so
 * the drawn equivalent width IS the Thickness parameter.
 *
 *   • WIDTH handles (filled, both half-height pivots) → drag horizontally →
 *     Thickness (relative/exponential drag: equal moves are equal width
 *     RATIOS, matching the log-skewed parameter).
 *   • EDGE handle (hollow, at the skirt's right foot) → drag horizontally →
 *     Edge softness (0 = square band, 1 = smooth bump).
 *
 * The profile brightens while the bound pool instance is actually
 * simplifying a stream. Right-click a handle → MIDI Learn for its param.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../processing/lux_centro.h"   // self-manages extern "C" linkage

class CentroLineEditorComponent : public juce::Component,
                                  private juce::Timer
{
public:
    static constexpr int kPreferredH = 96;   // graph only — boxes live on the main editor

    CentroLineEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                              juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance().
        setRepaintsOnMouseActivity(true);
        startTimerHz(15);   // only the live glow animates here
    }

    ~CentroLineEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance). */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the handles to one instance's Thickness / Edge params. */
    void setInstance(int slot,
                     const juce::String& thicknessId,
                     const juce::String& edgeId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        thk.attach.reset(); edg.attach.reset();
        thicknessId_ = thicknessId;
        edgeId_      = edgeId;
        bind(thk, thicknessId);
        bind(edg, edgeId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
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
            const LuxCentroState& st = *lux_centro_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.centro_active != 0);

            // Barycentre anchor — the redrawn line is centred here.
            g.setColour(accent.withAlpha(0.30f));
            g.drawVerticalLine((int) geo.cx, geo.topY, geo.botY);

            // Half-height guide — the pivot the equivalent width is read at.
            {
                const float y = yOf(geo, 0.5f);
                const float dash[2] = { 3.0f, 3.0f };
                g.setColour(juce::Colours::white.withAlpha(0.12f));
                g.drawDashedLine(juce::Line<float>(geo.plot.getX(), y,
                                                   geo.plot.getRight(), y),
                                 dash, 2, 1.0f);
            }

            // The line profile — the REAL edge window (plateau + skirt).
            {
                juce::Path p;
                p.startNewSubPath(geo.plot.getX(), geo.botY);
                const int W = juce::jmax(2, (int) geo.plot.getWidth());
                for (int px = 0; px <= W; ++px)
                {
                    const float x = geo.plot.getX() + (float) px;
                    const float d = std::abs(x - geo.cx) / geo.scale;
                    p.lineTo(x, yOf(geo, edgeWin(d, geo.h, geo.p)));
                }
                p.lineTo(geo.plot.getRight(), geo.botY);
                p.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.35f : 0.20f));
                g.fillPath(p);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(p, juce::PathStrokeType(1.4f));
            }

            drawNode(g, handlePos(Handle::WidthL, geo), Handle::WidthL, /*hollow*/ false);
            drawNode(g, handlePos(Handle::WidthR, geo), Handle::WidthR, /*hollow*/ false);
            drawNode(g, handlePos(Handle::Edge,   geo), Handle::Edge,   /*hollow*/ true);

            // Handle labels — below the plot, following their handle.
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            auto handleLabel = [&](Handle h, const juce::String& t, bool active)
            {
                const float x = handlePos(h, geo).x;
                const int   w = 64;
                const int   lx = juce::jlimit((int) geo.plot.getX(),
                                              (int) geo.plot.getRight() - w,
                                              (int) x - w / 2);
                g.setColour(active ? juce::Colours::white.withAlpha(0.85f)
                                   : accent.withAlpha(0.55f));
                g.drawText(t, lx, (int) geo.botY + 2, w, 9,
                           juce::Justification::centred, false);
            };
            // Thickness sits under the LEFT pivot, Edge under the right foot —
            // opposite sides of the centre, so the labels never collide.
            handleLabel(Handle::WidthL, "Thickness",
                        handleActive(Handle::WidthL) || handleActive(Handle::WidthR));
            handleLabel(Handle::Edge, "Edge", handleActive(Handle::Edge));
        }

        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("LINE SHAPE", (int) bounds.getX() + 8, (int) bounds.getY() + 2,
                   110, 9, juce::Justification::centredLeft, false);
        g.setColour(accent.withAlpha(0.5f));
        g.drawText(juce::String(thk.value, 1) + " px",
                   (int) bounds.getRight() - 78, (int) bounds.getY() + 2,
                   70, 9, juce::Justification::centredRight, false);
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
        const Handle h = handleAt(e.position, computeGeometry());

        // Right-click near a handle → MIDI Learn for its parameter.
        if (e.mods.isPopupMenu())
        {
            if (midiMap_ != nullptr && h != Handle::None)
                MidiLearnPopup::show(*midiMap_,
                                     h == Handle::Edge ? edgeId_ : thicknessId_,
                                     this);
            return;
        }

        dragging    = h;
        hovered     = h;
        dragStartX_ = e.position.x;
        if (dragging == Handle::WidthL || dragging == Handle::WidthR)
            { thk.begin(); thickness0_ = thk.value; }
        if (dragging == Handle::Edge)
            { edg.begin(); edge0_ = edg.value; }
        if (dragging != Handle::None) repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::None) return;
        // Outward = wider, whichever side is held (left handle mirrors).
        float dx = e.position.x - dragStartX_;
        if (dragging == Handle::WidthL) dx = -dx;

        if (dragging == Handle::Edge)
        {
            edg.setGesture(juce::jlimit(0.0f, 1.0f, edge0_ + dx / 120.0f));
            return;
        }
        // Relative exponential drag — equal moves are equal width RATIOS
        // (the same non-linearity as the log-skewed parameter).
        thk.setGesture(juce::jlimit(1.0f, 64.0f,
            thickness0_ * std::pow(2.0f, dx / 60.0f)));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::WidthL || dragging == Handle::WidthR) { thk.end(); }
        if (dragging == Handle::Edge) { edg.end(); }
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, WidthL, WidthR, Edge };

    /** Fixed axis half-span in image px — room for the widest line
     *  (thickness 64 → c 32) plus a full soft skirt. */
    static constexpr float kHalfSpanPx = 68.0f;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float cx = 0, topY = 0, botY = 0;
        float scale = 1;            // image px → screen px
        float c = 3, p = 3, h = 3;  // pivot geometry (image px)
        bool  valid = false;
    };

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (getWidth() < 60 || getHeight() < 30) return geo;
        geo.plot  = getLocalBounds().toFloat().reduced(8.0f, 7.0f)
                                    .withTrimmedTop(6.0f)
                                    .withTrimmedBottom(10.0f);   // handle-label strip
        geo.cx    = geo.plot.getCentreX();
        geo.topY  = geo.plot.getY();
        geo.botY  = geo.plot.getBottom();
        geo.scale = (geo.plot.getWidth() * 0.5f) / kHalfSpanPx;

        // Pivot geometry — MUST mirror lux_centro_redraw().
        const float thickness = juce::jlimit(1.0f, 64.0f, thk.value);
        const float soft      = juce::jlimit(0.0f, 1.0f,  edg.value);
        geo.c = juce::jmax(0.5f, 0.5f * thickness);
        const float skirt = soft * juce::jmax(geo.c, 1.0f);
        geo.h = geo.c + skirt;
        geo.p = juce::jmax(0.0f, geo.c - skirt);
        geo.valid = true;
        return geo;
    }

    float yOf(const Geometry& geo, float a) const
    { return geo.botY - juce::jlimit(0.0f, 1.0f, a) * (geo.botY - geo.topY); }

    /** Continuous edge window — same shape as lux_centro_edge_win(). */
    static float edgeWin(float d, float h, float p)
    {
        if (d <= p) return 1.0f;
        if (d >= h) return 0.0f;
        const float t = (d - p) / (h - p);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        // Width handles sit at the half-height pivots (±c); a hair-thin line
        // keeps them grabbable at a minimum offset from the centre.
        const float dxMin = 8.0f;
        if (h == Handle::WidthL || h == Handle::WidthR)
        {
            const float dx = juce::jmax(dxMin, geo.c * geo.scale);
            const float x  = (h == Handle::WidthL) ? geo.cx - dx : geo.cx + dx;
            return { juce::jlimit(geo.plot.getX() + kNodeR,
                                  geo.plot.getRight() - kNodeR, x),
                     yOf(geo, 0.5f) };
        }
        // Edge — rides the skirt's right flank where the curve crosses kEdgeA
        // (t = smoothstep⁻¹(1 − kEdgeA)), so it sits ON the profile: at the
        // vertical drop when edge = 0, sliding down the skirt as it softens.
        const float t = 0.851f;
        const float d = geo.p + t * (geo.h - geo.p);
        const float x = geo.cx + juce::jmax(dxMin, d * geo.scale);
        return { juce::jlimit(geo.plot.getX() + kNodeR,
                              geo.plot.getRight() - kNodeR, x),
                 yOf(geo, kEdgeA) };
    }

    Handle handleAt(juce::Point<float> pt, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        Handle best = Handle::None;
        float bestD = kHitR;
        for (Handle h : { Handle::WidthL, Handle::WidthR, Handle::Edge })
        {
            const float d = pt.getDistanceFrom(handlePos(h, geo));
            if (d < bestD) { bestD = d; best = h; }
        }
        return best;
    }

    bool handleActive(Handle h) const
    { return h == dragging || (dragging == Handle::None && h == hovered); }

    void drawNode(juce::Graphics& g, juce::Point<float> pt, Handle h, bool hollow)
    {
        const bool active = handleActive(h);
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

    static constexpr float kNodeR = 4.5f;
    static constexpr float kBendR = 3.2f;
    static constexpr float kHitR  = 12.0f;
    static constexpr float kEdgeA = 0.06f;   // curve alpha the Edge handle rides at

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live glow)

    Bound thk, edg;
    juce::String thicknessId_, edgeId_;   // MIDI-learn popup targets
    MidiMappingEngine* midiMap_ = nullptr;

    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };
    float  dragStartX_ { 0 };
    float  thickness0_ { 6 };   // value at drag start (relative drags)
    float  edge0_      { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CentroLineEditorComponent)
};
