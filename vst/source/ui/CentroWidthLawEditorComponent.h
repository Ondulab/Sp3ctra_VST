/**
 * @file CentroWidthLawEditorComponent.h
 * @brief WIDTH LAW view for the CENTROID simplifier — how the redrawn line
 *        width reads along the pixel/frequency axis (bass left, treble right).
 *
 * The view draws the STROKE SILHOUETTE: a band centred vertically whose
 * height at x is the width a mass redrawn THERE would get — thickness_px
 * shaped by the active width law (PX: uniform / ERB: the ear's critical
 * band, wider bass, narrower treble; selector on the SETUP face) times the
 * WidthTilt slope (×2^(tilt·(u−½))). The silhouette is auto-normalised (the
 * SHAPE is the message; LINE SHAPE owns the absolute profile) with the
 * resulting px width printed at both ends.
 *
 *   • TILT handles (filled nodes, both ends of the top contour) → drag
 *     vertically → WidthTilt: pull an end wider/narrower, the band pivots
 *     around the axis centre (relative drag — equal moves are equal octave
 *     slopes).
 *
 * Chrome (frame, caption, law/tilt readout) = ModuleChrome; the handles =
 * Sp3ctraHandles (lime, Idle/Hover/Drag), the "Tilt" label follows them.
 * The band brightens while the bound pool instance is actually simplifying
 * a stream. Right-click a handle → MIDI Learn for WidthTilt.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraHandles.h"
#include "ModuleEditorChrome.h"
#include "../processing/lux_centro.h"   // self-manages extern "C" linkage

class CentroWidthLawEditorComponent : public juce::Component,
                                      private juce::Timer
{
public:
    static constexpr int kPreferredH = 78;   // graph only — boxes live on the main editor

    CentroWidthLawEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                                  juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning editor calls setInstance().
        setRepaintsOnMouseActivity(true);
        startTimerHz(15);   // live glow + the law follows the synced config
    }

    ~CentroWidthLawEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance). */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind to one instance's Thickness / WidthTilt params. */
    void setInstance(int slot,
                     const juce::String& thicknessId,
                     const juce::String& widthTiltId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        thk.attach.reset(); wtl.attach.reset();
        widthTiltId_ = widthTiltId;
        bind(thk, thicknessId);
        bind(wtl, widthTiltId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto frame = getLocalBounds().toFloat();
        ModuleChrome::drawFrame(g, frame, accent);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            const LuxCentroState& st = *lux_centro_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.centro_active != 0);

            // Centre axis — the barycentre every stroke is printed around.
            g.setColour(accent.withAlpha(0.30f));
            g.drawHorizontalLine((int) geo.cy, geo.plot.getX(), geo.plot.getRight());

            // The stroke silhouette — mirrored band, auto-normalised.
            {
                juce::Path p;
                const int W = juce::jmax(2, (int) geo.plot.getWidth());
                for (int px = 0; px <= W; ++px)
                {
                    const float x = geo.plot.getX() + (float) px;
                    const float h = halfHeightAt(geo, (float) px / (float) W);
                    if (px == 0) p.startNewSubPath(x, geo.cy - h);
                    else         p.lineTo(x, geo.cy - h);
                }
                for (int px = W; px >= 0; --px)
                {
                    const float x = geo.plot.getX() + (float) px;
                    const float h = halfHeightAt(geo, (float) px / (float) W);
                    p.lineTo(x, geo.cy + h);
                }
                p.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.35f : 0.20f));
                g.fillPath(p);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(p, juce::PathStrokeType(1.4f));
            }

            const auto stL = handleState(Handle::TiltL);
            const auto stR = handleState(Handle::TiltR);
            Sp3ctraHandles::drawNode(g, handlePos(Handle::TiltL, geo), stL);
            Sp3ctraHandles::drawNode(g, handlePos(Handle::TiltR, geo), stR);

            // End widths in px — the absolute meaning of the normalised shape.
            // The hot side shows "Tilt" (lime) in place of its readout, so
            // the two never collide.
            const bool l = Sp3ctraHandles::isHot(stL), r = Sp3ctraHandles::isHot(stR);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            g.setColour(l ? Sp3ctraHandles::colour() : accent.withAlpha(0.55f));
            g.drawText(l ? juce::String("Tilt") : juce::String(widthAt(0.0f), 1) + " px",
                       (int) geo.plot.getX() + 2, (int) geo.plot.getBottom() + 1,
                       64, 9, juce::Justification::centredLeft, false);
            g.setColour(r ? Sp3ctraHandles::colour() : accent.withAlpha(0.55f));
            g.drawText(r ? juce::String("Tilt") : juce::String(widthAt(1.0f), 1) + " px",
                       (int) geo.plot.getRight() - 66, (int) geo.plot.getBottom() + 1,
                       64, 9, juce::Justification::centredRight, false);
        }

        ModuleChrome::drawCaption(g, frame, accent, "WIDTH LAW");
        // Active law (SETUP-face selector) + tilt readout.
        {
            const LuxCentroState& st = *lux_centro_instance(slot_);
            const bool erb = st.config.width_law == LUX_CENTRO_WIDTH_ERB;
            juce::String s (erb ? "ERB" : "PX");
            if (std::abs(wtl.value) >= 0.005f)   // one display digit's worth
                s << "  " << (wtl.value > 0 ? "+" : "")
                  << juce::String(wtl.value, 2) << " oct";
            ModuleChrome::drawReadout(g, frame, accent, s);
        }
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != Handle::None) return;
        const Handle h = handleAt(e.position, computeGeometry());
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == Handle::None ? juce::MouseCursor::NormalCursor
                                         : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == Handle::None && hovered != Handle::None) { hovered = Handle::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const Handle h = handleAt(e.position, computeGeometry());

        // Right-click near a handle → MIDI Learn for WidthTilt.
        if (e.mods.isPopupMenu())
        {
            if (midiMap_ != nullptr && h != Handle::None)
                MidiLearnPopup::show(*midiMap_, widthTiltId_, this);
            return;
        }

        dragging    = h;
        hovered     = h;
        dragStartY_ = e.position.y;
        if (dragging != Handle::None) { wtl.begin(); tilt0_ = wtl.value; repaint(); }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::None) return;
        // Up = wider at the held end; the band pivots around the centre, so
        // the LEFT handle drives the tilt mirrored.
        float dy = dragStartY_ - e.position.y;
        if (dragging == Handle::TiltL) dy = -dy;
        wtl.setGesture(juce::jlimit(-3.0f, 3.0f, tilt0_ + dy / 40.0f));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging != Handle::None) { wtl.end(); }
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, TiltL, TiltR };

    /** Strip under the plot for the px readouts / "Tilt" label. */
    static constexpr float kLabelStripH = 10.0f;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float cy = 0;               // centre axis
        float scale = 1;            // width px → screen px (auto-normalised)
        bool  valid = false;
    };

    /** One ERB in octaves at f — same law as lux_centro_erb_oct(). */
    static float erbOct(float f)
    {
        const float erb = 24.7f * (4.37f * f * 0.001f + 1.0f);
        return std::log2(1.0f + erb / f);
    }

    /** Width-law multiplier at u — MUST mirror lux_centro_redraw() (the
     *  bound instance supplies the law + the axis frequencies). */
    float widthMulAt(float u) const
    {
        const LuxCentroState& cst = *lux_centro_instance(slot_);
        float m = 1.0f;
        if (cst.config.width_law == LUX_CENTRO_WIDTH_ERB)
        {
            const float oct = (cst.ui_axis_oct > 0) ? (float) cst.ui_axis_oct : 8.0f;
            const float low = (cst.config.axis_low_hz > 0.0f)
                            ? cst.config.axis_low_hz : 65.406f;
            m *= erbOct(low * std::exp2(u * oct))
               / erbOct(low * std::exp2(0.5f * oct));
        }
        if (wtl.value != 0.0f)
            m *= std::exp2(wtl.value * (u - 0.5f));
        return m;
    }

    /** Resulting redrawn width in image px at u (thickness × law). */
    float widthAt(float u) const
    {
        const float thickness = juce::jlimit(1.0f, 64.0f, thk.value);
        return juce::jmax(1.0f, thickness * widthMulAt(u));
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (getWidth() < 60 || getHeight() < 30) return geo;
        // The standard plot of the frame, minus the px-label strip.
        geo.plot = ModuleChrome::plotOf(getLocalBounds().toFloat())
                       .withTrimmedBottom(kLabelStripH);
        geo.cy   = geo.plot.getCentreY();

        // Auto-normalise: the widest point of the silhouette fills the plot
        // (the shape is the message — px readouts carry the absolute scale).
        float maxW = 1.0f;
        for (int i = 0; i <= 32; ++i)
            maxW = juce::jmax(maxW, widthAt((float) i / 32.0f));
        geo.scale = (geo.plot.getHeight() * 0.5f - 2.0f - kNodeR) / (0.5f * maxW);   // node stays inside the plot
        geo.valid = true;
        return geo;
    }

    /** Screen half-height of the band at u. */
    float halfHeightAt(const Geometry& geo, float u) const
    { return juce::jmax(1.0f, 0.5f * widthAt(u) * geo.scale); }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        const float x = (h == Handle::TiltL) ? geo.plot.getX() + kNodeR + 1.0f
                                             : geo.plot.getRight() - kNodeR - 1.0f;
        const float u = (h == Handle::TiltL) ? 0.0f : 1.0f;
        return { x, geo.cy - halfHeightAt(geo, u) };
    }

    Handle handleAt(juce::Point<float> pt, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        Handle best = Handle::None;
        float bestD = kHitR;
        for (Handle h : { Handle::TiltL, Handle::TiltR })
        {
            const float d = pt.getDistanceFrom(handlePos(h, geo));
            if (d < bestD) { bestD = d; best = h; }
        }
        return best;
    }

    /** Idle / Hover / Drag for a handle — Hover only while nothing drags. */
    Sp3ctraHandles::State handleState(Handle h) const noexcept
    {
        return Sp3ctraHandles::stateOf(h == dragging,
                                       dragging == Handle::None && h == hovered);
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

    static constexpr float kNodeR = Sp3ctraHandles::kNodeR;   // handle inset from the plot edge
    static constexpr float kHitR  = 12.0f;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (law + live glow)

    Bound thk, wtl;
    juce::String widthTiltId_;   // MIDI-learn popup target
    MidiMappingEngine* midiMap_ = nullptr;

    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };
    float  dragStartY_ { 0 };
    float  tilt0_      { 0 };   // value at drag start (relative drags)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CentroWidthLawEditorComponent)
};
