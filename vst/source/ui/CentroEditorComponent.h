/**
 * @file CentroEditorComponent.h
 * @brief Interactive editor for the LuxCentro CENTROID simplifier
 *        (Floor / Thickness / Edge).
 *
 * Mirrors the EchoEditorComponent feel: a graphic with draggable handles plus
 * numeric boxes, all bound to APVTS params (host-automatable, MIDI-mappable).
 * The graphic runs the REAL algorithm on the REAL input stream, drawn as two
 * live layers (30 Hz):
 *
 *   • rémanence (ghost fill) — ui_in_peak, the slow-release envelope of the
 *     recent maxima: the HIGHS of the stream stay readable for ~1-2 s;
 *   • live line — ui_in_now, the fast-release profile: the stream breathing,
 *     its dips show the LOWS against the remanent highs.
 *
 * The mass pass and the accent output ride the REMANENT envelope, so the
 * picture stays steady while the stream moves. Until the instance has seen a
 * stream, a fixed demo line (three masses of different sizes over a 128-px
 * view) keeps the editor readable.
 *
 * The stream view carries ONE control — the FLOOR line (dashed): drag
 * vertically → écrêtage threshold, masses dipping under it die or split,
 * live in the view. Thickness / Edge are shaped in the LINE SHAPE view
 * (CentroLineEditorComponent, MASK-style window profile), a child stacked
 * under the frame, and the numeric boxes live in their own row at the very
 * bottom (double-click = default).
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
#include "../processing/lux_centro.h"   // self-manages extern "C" linkage
#include "CentroLineEditorComponent.h"

class CentroEditorComponent : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr int kGraphH     = 150;  // the graphic frame alone
    static constexpr int kViewGap    = 4;    // frame → LINE SHAPE view
    static constexpr int kPreferredH = kGraphH + kViewGap
                                     + CentroLineEditorComponent::kPreferredH
                                     + 6 + 10 + 18;  // + box row

    CentroEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                          juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour), lineEditor_(apvtsIn, accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxcentro{slot}_*).
        addAndMakeVisible(lineEditor_);
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);   // fluid layers — the rémanence lives on screen
    }

    ~CentroEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept
    { midiMap_ = m; lineEditor_.setMidiMap(m); }

    /** (Re)bind the handles/boxes to one instance's bank and point the live
     *  activity overlay at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& floorId,
                     const juce::String& thicknessId,
                     const juce::String& edgeId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        flr.attach.reset(); thk.attach.reset(); edg.attach.reset();
        boxFAtt.reset(); boxTAtt.reset(); boxEAtt.reset();
        bind(flr, floorId);
        bind(thk, thicknessId);
        bind(edg, edgeId);
        initBox(boxF, floorId,     boxFAtt, 10.0);
        initBox(boxT, thicknessId, boxTAtt, 6.0);
        initBox(boxE, edgeId,      boxEAtt, 0.0);
        lineEditor_.setInstance(slot_, thicknessId, edgeId);
        learnF_.reset(); learnT_.reset(); learnE_.reset();
        if (midiMap_ != nullptr)
        {
            learnF_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxF, floorId);
            learnT_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxT, thicknessId);
            learnE_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxE, edgeId);
        }
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Numeric boxes OUT of the graphic frames — their own row at the very
        // bottom, under the LINE SHAPE view.
        auto row = area.removeFromBottom(kLabelH + kBoxH);
        area.removeFromBottom(kRowGap);
        lineEditor_.setBounds(
            area.removeFromBottom(CentroLineEditorComponent::kPreferredH));
        area.removeFromBottom(kViewGap);
        frameRect_ = area.toFloat();
        graphRect_ = area.reduced(6).toFloat();

        row.removeFromTop(kLabelH);
        const int gap = 8, n = 3;
        const int bw = (row.getWidth() - (n - 1) * gap) / n;
        boxF.setBounds(row.getX(),                  row.getY(), bw, kBoxH);
        boxT.setBounds(row.getX() + (bw + gap),     row.getY(), bw, kBoxH);
        boxE.setBounds(row.getX() + 2 * (bw + gap), row.getY(), bw, kBoxH);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(frameRect_.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(frameRect_.reduced(0.5f), 4.0f, 1.0f);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            const LuxCentroState& st = *lux_centro_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.centro_active != 0);
            const bool real = st.ui_in_valid != 0;

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

            // Simplified output = the redrawn lines on clean paper (the
            // real per-frame composition), filled in accent.
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

            // Barycentre ticks under the surviving masses.
            g.setColour(accent.withAlpha(0.55f));
            for (int s = 0; s < geo.numSegs; ++s)
            {
                const float x = xOf(geo, geo.segPos[s]);
                g.drawLine(x, geo.botY - 3.0f, x, geo.botY + 3.0f, 1.0f);
            }

            // FLOOR — dashed écrêtage threshold across the plot, labelled on
            // the line.
            {
                const float y = yOf(geo, geo.floorN);
                const float dash[2] = { 4.0f, 3.0f };
                const bool  active = handleActive(Handle::Floor);
                g.setColour(active ? juce::Colours::white : accent.withAlpha(0.7f));
                g.drawDashedLine(juce::Line<float>(geo.plot.getX(), y,
                                                   geo.plot.getRight(), y),
                                 dash, 2, 1.2f);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(active ? juce::Colours::white.withAlpha(0.85f)
                                   : accent.withAlpha(0.55f));
                const float ly = (y - 11.0f > geo.topY) ? y - 11.0f : y + 3.0f;
                g.drawText("Floor", (int) (geo.plot.getRight() - 94.0f), (int) ly,
                           70, 9, juce::Justification::centredRight, false);
            }

            drawNode(g, handlePos(Handle::Floor, geo), Handle::Floor, /*hollow*/ false);
        }

        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("MASSES - LINES", (int) frameRect_.getX() + 8,
                   (int) frameRect_.getY() + 2,
                   110, 9, juce::Justification::centredLeft, false);

        g.setColour(accent.withAlpha(0.6f));
        auto label = [&g](const juce::Slider& box, const juce::String& t)
        {
            auto bb = box.getBounds();
            g.drawText(t, bb.getX(), bb.getY() - kLabelH, bb.getWidth(), kLabelH,
                       juce::Justification::centred, false);
        };
        label(boxF, "Floor"); label(boxT, "Thickness");
        label(boxE, "Edge");
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
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Floor) { flr.begin(); }
        if (dragging != Handle::None) repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const Geometry geo = computeGeometry();
        if (!geo.valid || dragging != Handle::Floor) return;
        const float f = (geo.botY - e.position.y) / juce::jmax(1.0f, geo.botY - geo.topY);
        flr.setGesture(juce::jlimit(0.0f, 100.0f, f * 100.0f));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging == Handle::Floor) { flr.end(); }
        dragging = Handle::None;
        hovered  = handleAt(e.position, computeGeometry());
        repaint();
    }

private:
    enum class Handle { None, Floor };

    /** View width — one column per profile bin (the C engine publishes the
     *  stream's max-hold profile at this resolution). */
    static constexpr int kView = LUX_CENTRO_UI_BINS;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float topY = 0, botY = 0;
        float floorN = 0;                  // 0..1
        float c = 3, p = 3, h = 3;         // half widths in demo px (pivot geometry)
        // Segments of the demo line above the floor (mirrors the C pass).
        int   numSegs = 0;
        float segPos[8] {};
        float segAmp[8] {};                // peak of the remanent envelope in the mass
        int   mainSeg = -1;                // biggest mass — anchors the handles
        bool  valid = false;
    };

    /** Fixed demo input: three masses (tall/narrow, small, broad faint). */
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
     *  mass pass, the output preview and the handles ride THIS (steady
     *  targets); the demo masses stand in until a stream has been seen. */
    float peakAt(float x) const
    {
        const LuxCentroState& cst = *lux_centro_instance(slot_);
        if (cst.ui_in_valid)
        {
            const int i = juce::jlimit(0, kView - 1, (int) x);
            return juce::jlimit(0.0f, 1.0f, cst.ui_in_peak[i]);
        }
        return demoMass(x);
    }

    /** Live layer — fast-release profile of the stream as it breathes. */
    float nowAt(float x) const
    {
        const LuxCentroState& cst = *lux_centro_instance(slot_);
        const int i = juce::jlimit(0, kView - 1, (int) x);
        return juce::jlimit(0.0f, 1.0f, cst.ui_in_now[i]);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot      = graphRect_.reduced(8.0f, 7.0f);
        geo.topY      = geo.plot.getY();
        geo.botY      = geo.plot.getBottom();
        geo.floorN    = juce::jlimit(0.0f, 1.0f, flr.value / 100.0f);

        // Pivot geometry — MUST mirror lux_centro_redraw().
        const float thickness = juce::jlimit(1.0f, 64.0f, thk.value);
        const float soft      = juce::jlimit(0.0f, 1.0f,  edg.value);
        geo.c = juce::jmax(0.5f, 0.5f * thickness);
        const float skirt = soft * juce::jmax(geo.c, 1.0f);
        geo.h = geo.c + skirt;
        geo.p = juce::jmax(0.0f, geo.c - skirt);

        // Segment pass on the remanent envelope — mirrors
        // lux_centro_find_masses(), on steady data (no per-frame jitter).
        const float thr = geo.floorN;
        int   open = 0;
        float wsum = 0, wxsum = 0, segPk = 0;
        for (int i = 0; i <= kView; ++i)
        {
            const float m = (i < kView) ? peakAt((float) i) : -1.0f;
            if (m > thr)
            {
                if (!open) { open = 1; wsum = wxsum = 0; segPk = 0; }
                wsum  += m;
                wxsum += m * (float) i;
                if (m > segPk) segPk = m;
            }
            else if (open)
            {
                open = 0;
                if (geo.numSegs < 8 && wsum > 0)
                {
                    geo.segPos[geo.numSegs] = wxsum / wsum;
                    geo.segAmp[geo.numSegs] = segPk;
                    if (geo.mainSeg < 0 || segPk > geo.segAmp[geo.mainSeg])
                        geo.mainSeg = geo.numSegs;
                    geo.numSegs++;
                }
            }
        }
        geo.valid = true;
        return geo;
    }

    float xOf(const Geometry& geo, float demoPx) const
    { return geo.plot.getX() + (demoPx / (float) (kView - 1)) * geo.plot.getWidth(); }

    float yOf(const Geometry& geo, float energyN) const
    { return geo.botY - juce::jlimit(0.0f, 1.0f, energyN) * (geo.botY - geo.topY); }

    /** Continuous edge window — same shape as lux_centro_edge_win(). */
    static float edgeWin(float d, float h, float p)
    {
        if (d <= p) return 1.0f;
        if (d >= h) return 0.0f;
        const float t = (d - p) / (h - p);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    /** The real per-frame composition on the remanent envelope: the redrawn
     *  lines on clean paper. Mirrors lux_centro_process_frame — écrêtage
     *  erases everything below the floor, each line prints its mass's PEAK
     *  level (the most significant sample), at any thickness. */
    float outputAt(const Geometry& geo, float x) const
    {
        float lines = 0.0f;
        for (int s = 0; s < geo.numSegs; ++s)
            lines += geo.segAmp[s] * edgeWin(std::abs(x - geo.segPos[s]), geo.h, geo.p);
        return juce::jlimit(0.0f, 1.0f, lines);
    }

    juce::Point<float> handlePos(Handle, const Geometry& geo) const
    {
        return { geo.plot.getRight() - 12.0f, yOf(geo, geo.floorN) };
    }

    bool handleActive(Handle h) const
    { return h == dragging || (dragging == Handle::None && h == hovered); }

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

    void initBox(Sp3ctraBarSlider& box, const juce::String& id,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att,
                 double resetValue)
    {
        box.setAccent(accent);
        box.setDoubleClickReturnValue(true, resetValue);
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr float kNodeR  = 4.5f;
    static constexpr float kBendR  = 3.2f;
    static constexpr float kHitR   = 12.0f;
    static constexpr int   kBoxH   = 18;   // kPreferredH = kGraphH + gap + label + box
    static constexpr int   kLabelH = 10;
    static constexpr int   kRowGap = 6;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)

    Bound flr, thk, edg;
    CentroLineEditorComponent lineEditor_;   // LINE SHAPE — Thickness / Edge
    Sp3ctraBarSlider boxF, boxT, boxE;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        boxFAtt, boxTAtt, boxEAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnF_, learnT_, learnE_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // plot area inside the frame
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CentroEditorComponent)
};
