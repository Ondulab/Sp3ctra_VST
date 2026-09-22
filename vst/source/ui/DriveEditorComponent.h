/**
 * @file DriveEditorComponent.h
 * @brief Interactive editor for the LuxDrive LEVELS stage
 *        (Floor / Gamma / Saturation / Invert). Display name LEVELS — the
 *        type/file names keep "Drive".
 *
 * Mirrors the CentroEditorComponent feel AND its visual language: the graphic
 * runs the REAL transfer (lux_drive_transfer, the very function the RT LUT is
 * built from) on the REAL input stream, drawn as two live layers (30 Hz):
 *
 *   • rémanence (ghost fill) — ui_in_peak, the slow-release envelope of the
 *     recent maxima: the HIGHS of the stream stay readable for ~1-2 s;
 *   • live line — ui_in_now, the fast-release profile: the stream breathing,
 *     its dips show the LOWS against the remanent highs.
 *
 * The accent output curve rides the REMANENT envelope, so the picture stays
 * steady while the stream moves. Until the instance has seen a stream, a
 * fixed demo line (the same three masses as CENTROID) keeps the editor
 * readable.
 *
 * The stream view carries ONE control — the FLOOR line (dashed, HORIZONTAL,
 * same as CENTROID): drag vertically → écrêtage bas, profile parts at/below
 * the line are clipped to the background, live in the view. Everything else
 * is set OUTSIDE the graphic window:
 *
 *   • SATURATION — COLOUR saturation, a Photoshop-style ramp cursor: the
 *     strip sweeps B&W (left) → natural colours (centre) → hyper-vibrant
 *     rainbow (right), which IS the parameter's meaning. Drag anywhere to
 *     set, double-click = centre (neutral).
 *   • GAMMA / FLOOR / CONTRAST — numeric boxes in their own row (double-click
 *     = default; Gamma > 1 lifts the faint material (photo convention, same
 *     direction as the VideoScroll gamma), < 1 thins it; Contrast
 *     is the CONTRAST MIN floor — 100 % = off, lower it and a flat/blurred
 *     stream dims on screen as it drops in volume). The accent output curve
 *     rides the live factor (contrast_ema), so the dimming shows in the view.
 *
 * Chrome (frame, caption, box row) = ModuleChrome; the floor line, its node
 * and the ramp thumb = Sp3ctraHandles (lime, Idle/Hover/Drag); the module
 * colour stays on the curves, captions and labels.
 *
 * The output profile brightens while the bound pool instance is actually
 * processing a stream, so you see the module living.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include "ModuleEditorChrome.h"
#include "../processing/lux_drive.h"   // self-manages extern "C" linkage

class DriveEditorComponent : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr int kGraphH     = 150;  // the graphic frame alone
    static constexpr int kStripH     = 22;   // saturation ramp cursor
    // frame + gap + ramp strip + (gap + label + box row)
    static constexpr int kPreferredH = kGraphH + ModuleChrome::kRowGap + kStripH
                                     + ModuleChrome::kBelowFrameH;

    DriveEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                         juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxdrive{slot}_*).
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);   // fluid layers — the rémanence lives on screen
    }

    ~DriveEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  activity overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& gammaId,
                     const juce::String& saturationId,
                     const juce::String& floorId,
                     const juce::String& contrastId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        gm.attach.reset(); flr.attach.reset();
        boxGAtt.reset(); stripAtt.reset(); boxFAtt.reset(); boxCAtt.reset();
        bind(gm,  gammaId);
        bind(flr, floorId);
        initBox(boxG, gammaId, boxGAtt, 1.0);
        initBox(boxF, floorId, boxFAtt, 0.0);
        initBox(boxC, contrastId, boxCAtt, 100.0);

        // Colour-saturation ramp cursor — a plain horizontal Slider with a
        // custom paint (B&W → natural → hyper-vibrant rainbow).
        satStrip.setSliderStyle(juce::Slider::LinearHorizontal);
        satStrip.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        satStrip.setDoubleClickReturnValue(true, 0.0);
        addAndMakeVisible(satStrip);
        stripAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, saturationId, satStrip);

        learnG_.reset(); learnS_.reset(); learnF_.reset(); learnC_.reset();
        if (midiMap_ != nullptr)
        {
            learnG_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxG, gammaId);
            learnS_ = std::make_unique<MidiLearnAttachment>(*midiMap_, satStrip, saturationId);
            learnF_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxF, floorId);
            learnC_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxC, contrastId);
        }
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Controls OUT of the graphic frame — ramp strip + box row below it.
        auto row = area.removeFromBottom(ModuleChrome::kBoxRowH);
        area.removeFromBottom(ModuleChrome::kRowGap);
        satStrip.setBounds(area.removeFromBottom(kStripH));
        area.removeFromBottom(ModuleChrome::kRowGap);
        frameRect_ = area.toFloat();
        graphRect_ = ModuleChrome::graphOf(frameRect_);

        ModuleChrome::layoutBoxRow(row, { &boxG, &boxF, &boxC });
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frameRect_, accent);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            const LuxDriveState& dst = *lux_drive_instance(slot_);
            const bool live = (dst.config.enabled != 0 && dst.drive_active != 0);
            const bool real = dst.ui_in_valid != 0;

            // Rémanence — slow-release envelope of the recent maxima, ghost
            // fill: the HIGHS of the stream stay readable (demo until a
            // stream has been seen).
            {
                juce::Path pk;
                pk.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                    pk.lineTo(xOf(geo, (float) i), yOf(geo, peakAt((float) i)));
                pk.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                pk.closeSubPath();
                g.setColour(juce::Colours::white.withAlpha(0.06f));
                g.fillPath(pk);
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.strokePath(pk, juce::PathStrokeType(1.0f));
            }

            // Live input — fast-release profile, the stream breathing: its
            // dips show the LOWS against the remanent highs.
            if (real)
            {
                juce::Path nw;
                for (int i = 0; i < kView; ++i)
                {
                    const float x = xOf(geo, (float) i);
                    const float y = yOf(geo, nowAt((float) i));
                    if (i == 0) nw.startNewSubPath(x, y);
                    else        nw.lineTo(x, y);
                }
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                g.strokePath(nw, juce::PathStrokeType(1.1f));
            }

            // Output = the remanent envelope through the REAL transfer,
            // filled in accent — a steady target for the handles.
            {
                juce::Path out;
                out.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                    out.lineTo(xOf(geo, (float) i),
                               yOf(geo, outputAt(geo, (float) i)));
                out.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                out.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.45f : 0.28f));
                g.fillPath(out);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(out, juce::PathStrokeType(1.2f));
            }

            // FLOOR — grabbable dashed écrêtage threshold across the plot
            // (HORIZONTAL, same reading as the CENTROID editor), labelled on
            // the line; the label turns lime with its handle.
            {
                const float y  = yOf(geo, geo.floorN);
                const auto  fs = handleState(Handle::Floor);
                Sp3ctraHandles::drawGrabLine(g, juce::Line<float>(geo.plot.getX(), y,
                                                                  geo.plot.getRight(), y),
                                             fs, /*dashed*/ true);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(Sp3ctraHandles::isHot(fs) ? Sp3ctraHandles::colour()
                                                      : accent.withAlpha(0.55f));
                const float ly = (y - 11.0f > geo.topY) ? y - 11.0f : y + 3.0f;
                g.drawText("Floor", (int) (geo.plot.getRight() - 94.0f), (int) ly,
                           70, 9, juce::Justification::centredRight, false);
                Sp3ctraHandles::drawNode(g, handlePos(Handle::Floor, geo), fs);
            }
        }

        ModuleChrome::drawCaption(g, frameRect_, accent, "IN - OUT");

        ModuleChrome::drawBoxLabel(g, boxG, accent, "Gamma");
        ModuleChrome::drawBoxLabel(g, boxF, accent, "Floor");
        ModuleChrome::drawBoxLabel(g, boxC, accent, "Contrast");
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
        if (e.mods.isPopupMenu() || e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Floor) { flr.begin(); }
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
        if (!geo.valid || dragging != Handle::Floor) return;
        // Absolute vertical drag — the line follows the mouse (CENTROID).
        const float plotH = juce::jmax(1.0f, geo.botY - geo.topY);
        const float f = (geo.botY - e.position.y) / plotH;
        flr.setGesture(juce::jlimit(0.0f, 100.0f, f * 100.0f));
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
    enum class Handle { None, Floor };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    /** What a handle drives: the double-click resets it, the long press
     *  types it. */
    Sp3ctraGestures::BoundList boundsOf(Handle h)
    {
        switch (h)
        {
            case Handle::Floor: return { { "Floor", &flr } };
            default: return {};
        }
    }

    void endGesture(Handle h)
    {
        if (h == Handle::Floor) flr.end();
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

    /** View width — one column per profile bin (the C engine publishes the
     *  stream's max-hold profile at this resolution). */
    static constexpr int kView = LUX_DRIVE_UI_BINS;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float topY = 0, botY = 0;
        float floorN = 0;          // 0..1 (energy axis)
        float gamma  = 1;          // mid-tone power law
        bool  valid = false;
    };

    /** Fixed demo input: three masses (tall/narrow, small, broad faint) —
     *  identical to CentroEditorComponent::demoMass. */
    static float demoMass(float x)
    {
        auto gauss = [](float x0, float c0, float s0, float a0)
        {
            const float d = (x0 - c0) / s0;
            return a0 * std::exp(-0.5f * d * d);
        };
        const float m = gauss(x, 34.0f, 7.0f, 0.85f)
                      + gauss(x, 58.0f, 3.5f, 0.40f)
                      + gauss(x, 96.0f, 11.0f, 0.30f);
        return juce::jlimit(0.0f, 1.0f, m);
    }

    /** Rémanence layer — slow-release envelope of the recent maxima. The
     *  transfer preview and the handles ride THIS (steady targets); the demo
     *  masses stand in until a stream has been seen. */
    float peakAt(float x) const
    {
        const LuxDriveState& dst = *lux_drive_instance(slot_);
        if (dst.ui_in_valid)
        {
            const int i = juce::jlimit(0, kView - 1, (int) x);
            return juce::jlimit(0.0f, 1.0f, dst.ui_in_peak[i]);
        }
        return demoMass(x);
    }

    /** Live layer — fast-release profile of the stream as it breathes. */
    float nowAt(float x) const
    {
        const LuxDriveState& dst = *lux_drive_instance(slot_);
        const int i = juce::jlimit(0, kView - 1, (int) x);
        return juce::jlimit(0.0f, 1.0f, dst.ui_in_now[i]);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot    = ModuleChrome::plotOf(frameRect_);
        geo.topY    = geo.plot.getY();
        geo.botY    = geo.plot.getBottom();
        geo.floorN = juce::jlimit(0.0f, 1.0f, flr.value / 100.0f);
        geo.gamma  = juce::jlimit(LUX_DRIVE_GAMMA_MIN, LUX_DRIVE_GAMMA_MAX,
                                  gm.value);
        geo.valid  = true;
        return geo;
    }

    float xOf(const Geometry& geo, float demoPx) const
    { return geo.plot.getX() + (demoPx / (float) (kView - 1)) * geo.plot.getWidth(); }

    float yOf(const Geometry& geo, float energyN) const
    { return geo.botY - juce::jlimit(0.0f, 1.0f, energyN) * (geo.botY - geo.topY); }

    /** The REAL per-pixel transfer on the remanent envelope (ABSOLUTE energy
     *  from the background pole): the same lux_drive_transfer the RT LUT
     *  samples, normalized to 0..1, scaled by the live CONTRAST MIN factor
     *  the C side actually applies (contrast_ema; -1 = knob off → 1). */
    float outputAt(const Geometry& geo, float x) const
    {
        const LuxDriveState& dst = *lux_drive_instance(slot_);
        const float cg = (dst.config.enabled != 0 && dst.contrast_ema >= 0.0f)
                           ? dst.contrast_ema : 1.0f;
        return lux_drive_transfer(peakAt(x) * 255.0f, geo.gamma,
                                  geo.floorN * 255.0f) * cg * (1.0f / 255.0f);
    }

    juce::Point<float> handlePos(Handle, const Geometry& geo) const
    {
        return { geo.plot.getRight() - 12.0f, yOf(geo, geo.floorN) };
    }

    /** Idle / Hover / Drag for a handle — Hover only while nothing drags. */
    Sp3ctraHandles::Look handleState(Handle h) const noexcept
    {
        return Sp3ctraHandles::stateOf(h == dragging,
                                       dragging == Handle::None && h == hovered,
                                       false, handleHeat(h));
    }

    Handle handleAt(juce::Point<float> p, const Geometry& geo) const
    {
        if (!geo.valid) return Handle::None;
        // The floor line is grabbable anywhere along its length.
        const float y = yOf(geo, geo.floorN);
        if (p.x >= geo.plot.getX() && p.x <= geo.plot.getRight()
            && std::abs(p.y - y) < kHitR)
            return Handle::Floor;
        return Handle::None;
    }

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    //==========================================================================
    /** Remote-edit heat of the parameter(s) a handle drives — a change from
     *  the box below, a MIDI CC or automation lights the handle exactly like
     *  a drag (ui/Sp3ctraControls.h). */
    float handleHeat(Handle h) const noexcept
    {
        return h == Handle::Floor ? flr.heat() : 0.0f;
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
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att,
                 double resetValue)
    {
        box.setDoubleClickReturnValue(true, resetValue);
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kHitR = 12.0f;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    /** Photoshop-style ramp cursor for the COLOUR saturation axis: the track
     *  IS the parameter's meaning — a black & white ramp at the far left,
     *  natural colours at the centre tick, a hyper-vibrant rainbow at the
     *  far right. Sp3ctraGestureSlider mechanics (quick click / drag = set,
     *  hold = centre, double-click = type, MIDI-learnable); the grab point
     *  is a lime Sp3ctraHandles thumb. */
    class RampSlider : public Sp3ctraGestureSlider
    {
    public:
        void paint(juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
            g.fillRoundedRectangle(r.reduced(0.5f), 3.0f);

            const auto track = r.reduced(1.5f);
            const int W = juce::jmax(2, (int) track.getWidth());
            for (int px = 0; px < W; ++px)
            {
                const float p = (float) px / (float) (W - 1);
                // Chroma follows the parameter axis: 0 at the far left
                // (B&W), natural at the centre, full-blast at the right.
                const float chroma = (p <= 0.5f)
                    ? (p * 2.0f) * 0.55f
                    : 0.55f + (p - 0.5f) * 2.0f * 0.45f;
                // Brightness ramps on the left half so the B&W side reads
                // as a grey gradient, then lifts toward the vibrant end.
                const float bright = (p <= 0.5f)
                    ? 0.30f + (p * 2.0f) * 0.42f
                    : 0.72f + (p - 0.5f) * 2.0f * 0.20f;
                g.setColour(juce::Colour::fromHSV(p, chroma, bright, 1.0f));
                g.fillRect(track.getX() + (float) px, track.getY(),
                           1.0f, track.getHeight());
            }

            // Centre tick — the neutral point (double-click returns here).
            {
                const float mx = track.getX() + 0.5f * track.getWidth();
                g.setColour(juce::Colours::white.withAlpha(0.5f));
                g.fillRect(mx - 0.5f, r.getY() + 2.0f, 1.0f, r.getHeight() - 4.0f);
            }

            // Label + value — doubled dark/light so it reads on both ends
            // of the ramp.
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            const juce::String txt = "Saturation  "
                + juce::String(getValue() > 0.0 ? "+" : "")
                + juce::String(getValue(), 1) + " %";
            const auto tr = r.reduced(7.0f, 0.0f).toNearestInt();
            g.setColour(juce::Colours::black.withAlpha(0.65f));
            g.drawText(txt, tr.translated(1, 1), juce::Justification::centredLeft, false);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(txt, tr, juce::Justification::centredLeft, false);

            // Outline + thumb on the shared ladder: calm at rest, full accent
            // under the pointer, white while it is moved — by the mouse or by
            // a MIDI CC (Sp3ctraControls::heatOf, stamped by the learn
            // attachment on this strip).
            const auto st = Sp3ctraHandles::stateOf(isMouseButtonDown(),
                                                    isMouseOverOrDragging(), false,
                                                    Sp3ctraControls::heatOf(*this));
            g.setColour(Sp3ctraHandles::ringColour(st).withMultipliedAlpha(0.5f));
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);

            // The grab point — a thumb at the saturation value.
            const float t  = (float) valueToProportionOfLength(getValue());
            const float cx = track.getX() + t * track.getWidth();
            Sp3ctraHandles::drawThumb(g,
                juce::Rectangle<float>(cx - 2.5f, r.getY() + 1.0f, 5.0f, r.getHeight() - 2.0f),
                st);
        }
    };

    Bound gm, flr;
    Sp3ctraBarSlider boxG, boxF, boxC;
    RampSlider   satStrip;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        boxGAtt, boxFAtt, boxCAtt, stripAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnG_, learnS_, learnF_, learnC_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // graph area inside the frame
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DriveEditorComponent)
};
