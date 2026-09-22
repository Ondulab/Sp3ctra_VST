/**
 * @file ReverbEditorComponent.h
 * @brief Interactive editor for the LuxReverb tail (Decay / Diffusion / Mix
 *        / Damping + law).
 *
 * Standard module editor (ModuleChrome skeleton): a TAIL frame holding the
 * tail response and its draggable handles (Sp3ctraHandles — lime, hover and
 * drag distinct), a DAMPING frame below it (ReverbDampingEditorComponent —
 * the tail length along the frequency axis, treble handle + LIN/AIR law
 * chips), plus compact numeric boxes in their own row BELOW the frames, all
 * bound to APVTS params (host-automatable, MIDI-mappable).  The TAIL x-axis
 * is time (skewed like the Decay param), y is the tail level; the dry
 * impulse sits at t = 0 and the tail fades in a straight line to the pole
 * at t = Decay (linear in energy = exponential in audio through the synth's
 * dB law — lux_reverb.h).
 *
 *   • Mix node (left, filled)    → drag vertically → wet level of the tail.
 *   • Decay node (bottom, filled)→ drag horizontally → where the tail ends.
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
#include "ModuleEditorChrome.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include "Sp3ctraBarSlider.h"
#include "ReverbDampingEditorComponent.h"
#include "../processing/lux_reverb.h"   // self-manages extern "C" linkage

class ReverbEditorComponent : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr int kGraphH     = 82;   // the TAIL frame alone (plot 56 px)
    static constexpr int kViewGap    = ModuleChrome::kEditorGap;   // TAIL → DAMPING
    // TAIL frame + gap + DAMPING frame + gap + label + box row
    static constexpr int kPreferredH = kGraphH + kViewGap
                                     + ReverbDampingEditorComponent::kPreferredH
                                     + ModuleChrome::kBelowFrameH;

    ReverbEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                          juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour), damping_(apvtsIn, accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxreverb{slot}_*).
        addAndMakeVisible(damping_);
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~ReverbEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; damping_.setMidiMap(m); }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  tail overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& decayId,
                     const juce::String& diffusionId,
                     const juce::String& mixId,
                     const juce::String& dampingId,
                     const juce::String& dampTypeId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        dcy.attach.reset(); dif.attach.reset(); mix.attach.reset();
        boxDAtt.reset(); boxFAtt.reset(); boxMAtt.reset(); boxAAtt.reset();
        bind(dcy, decayId);
        bind(dif, diffusionId);
        bind(mix, mixId);
        initBox(boxD, decayId,     boxDAtt);
        initBox(boxF, diffusionId, boxFAtt);
        initBox(boxM, mixId,       boxMAtt);
        initBox(boxA, dampingId,   boxAAtt);
        learnD_.reset(); learnF_.reset(); learnM_.reset(); learnA_.reset();
        if (midiMap_ != nullptr)
        {
            learnD_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxD, decayId);
            learnF_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxF, diffusionId);
            learnM_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxM, mixId);
            learnA_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxA, dampingId);
        }
        damping_.setInstance(slot_, decayId, dampingId, dampTypeId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Controls OUT of the graphic frames — box row below them.
        auto row = area.removeFromBottom(ModuleChrome::kBoxRowH);
        area.removeFromBottom(ModuleChrome::kRowGap);
        damping_.setBounds(area.removeFromBottom(ReverbDampingEditorComponent::kPreferredH));
        area.removeFromBottom(kViewGap);
        frameRect_ = area.toFloat();
        graphRect_ = ModuleChrome::graphOf(frameRect_);
        ModuleChrome::layoutBoxRow(row, { &boxD, &boxF, &boxM, &boxA });
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frameRect_, accent);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            // Dry impulse at t = 0.
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(geo.x0 - 1.0f, geo.topY, 2.0f, geo.botY - geo.topY));

            // End-of-tail marker (t = Decay).
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

        ModuleChrome::drawCaption(g, frameRect_, accent, "TAIL");
        ModuleChrome::drawBoxLabel(g, boxD, accent, "Decay");
        ModuleChrome::drawBoxLabel(g, boxF, accent, "Diffusion");
        ModuleChrome::drawBoxLabel(g, boxM, accent, "Mix");
        ModuleChrome::drawBoxLabel(g, boxA, accent, "Damping");
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
        if (e.mods.isPopupMenu() || e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Mix)   mix.begin();
        if (dragging == Handle::Decay) dcy.begin();
        if (dragging != Handle::None)
        {
            hold_.arm(e, [this] { holdToType(); });   // long press = type
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hold_.fired()) return;            // the entry bubble owns the rest
        hold_.moved(e);
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
        hold_.release();
        endGesture(dragging);
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

    /** Double-click = the handle's parameters back to their defaults. */
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        Sp3ctraGestures::toDefault(boundsOf(handleAt(e.position, computeGeometry())));
    }

private:
    enum class Handle { None, Mix, Decay };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    /** What a handle drives: the double-click resets it, the long press
     *  types it. */
    Sp3ctraGestures::BoundList boundsOf(Handle h)
    {
        switch (h)
        {
            case Handle::Mix:   return { { "Mix",   &mix } };
            case Handle::Decay: return { { "Decay", &dcy } };
            default:            return {};
        }
    }

    void endGesture(Handle h)
    {
        if (h == Handle::Mix)   mix.end();
        if (h == Handle::Decay) dcy.end();
    }

    /** Long press on a handle: the drag gesture closes, the bubble opens. */
    void holdToType()
    {
        const Handle h = dragging;
        endGesture(h);
        dragging = Handle::None;
        repaint();
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), boundsOf(h));
    }

    Sp3ctraGestures::Hold hold_;

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
        geo.plot      = ModuleChrome::plotOf(frameRect_);
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

    /* level(t) = mix · max(0, 1 - t / decay) — the tail response of the
     * bass edge (the DAMPING view shows how the treble shortens it). */
    juce::Path buildCurve(const Geometry& geo) const
    {
        const int W = juce::jmax(2, (int) (geo.plot.getRight() - geo.x0));
        juce::Path p;
        p.startNewSubPath(geo.x0, geo.botY - geo.mixN * (geo.botY - geo.topY));
        for (int px = 1; px <= W; ++px)
        {
            const float x = geo.x0 + (float) px;
            const float t = timeForX(geo, x);
            const float lvl = geo.mixN * juce::jmax(0.0f, 1.0f - t / geo.decayS);
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

    /** Handle painter — filled lime node (Sp3ctraHandles: Idle / Hover /
     *  Drag are distinct states). */
    void drawNode(juce::Graphics& g, juce::Point<float> pt, Handle h) const
    {
        Sp3ctraHandles::drawNode(g, pt,
            Sp3ctraHandles::stateOf(h == dragging, dragging == Handle::None && h == hovered,
                                    false, handleHeat(h)));
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
    /** Remote-edit heat of the parameter(s) a handle drives — a change from
     *  the box below, a MIDI CC or automation lights the handle exactly like
     *  a drag (ui/Sp3ctraControls.h). */
    float handleHeat(Handle h) const noexcept
    {
        switch (h)
        {
            case Handle::Mix:   return mix.heat();
            case Handle::Decay: return dcy.heat();
            default:            return 0.0f;
        }
    }

    /** The shared parameter binding — ui/Sp3ctraControls.h. Besides the
     *  attachment and the mirrored value it carries the EDIT HEAT: any
     *  change, from this editor's drag, from the box below, from a MIDI CC
     *  or from automation, lights the handle that owns it. */
    using Bound = Sp3ctraControls::Bound;

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.bind(apvts, id, [this](float) { repaint(); });
    }

    void initBox(Sp3ctraBarSlider& box, const juce::String& id,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
    {
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kNodeR = Sp3ctraHandles::kNodeR;   // handle inset from the plot edges
    static constexpr float kHitR  = 12.0f;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound dcy, dif, mix;
    Sp3ctraBarSlider boxD, boxF, boxM, boxA;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxDAtt, boxFAtt, boxMAtt, boxAAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnD_, learnF_, learnM_, learnA_;
    ReverbDampingEditorComponent damping_;   // DAMPING — Damping + law

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // graph area inside the frame (ModuleChrome::graphOf)
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbEditorComponent)
};
