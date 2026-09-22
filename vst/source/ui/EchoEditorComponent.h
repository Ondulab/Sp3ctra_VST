/**
 * @file EchoEditorComponent.h
 * @brief Interactive editor for the LuxEcho repeats (Delay / Feedback / Mix).
 *
 * Standard module editor (ModuleChrome skeleton): a graphic frame holding the
 * repeat train and its draggable handles (Sp3ctraHandles — lime, hover and
 * drag distinct), plus compact numeric boxes in their own row BELOW the
 * frame, all bound to APVTS params (host-automatable, MIDI-mappable).
 *
 * The x-axis is TIME, linear, and the window it shows FITS THE TRAIN: from
 * the dry impulse at x = 0 to the last audible repeat (level ≥ 1.5 %, up to
 * 24 of them), rounded up to a power-of-two number of lines so the ruler
 * stays put while a handle moves, and eased when it does change. A ruler
 * along the bottom reads in seconds (the measured line rate — see
 * setLineRateProvider) or in lines while no stream runs; the top-right
 * readout gives the tail = the whole time window of the echo. The delay
 * spans 1 → 30 000 lines (LUX_ECHO_MAX_DELAY), so the window ranges from
 * 64 lines to a million.
 *
 *   • Repeat-1 node (filled)  → drag → Delay (x) + Mix (y). The window is
 *     frozen for the drag (the node stays under the pointer); holding the
 *     pointer at the right edge zooms out ×2 every 220 ms to reach longer
 *     delays, and the view refits on release.
 *   • Repeat-2 ring (hollow)  → drag vertically → Feedback (its level / mix).
 *
 * Live: the repeat bars brighten while the instance is processing a stream,
 * a band along the ruler shows the ring CHARGING (lines stored so far, until
 * the first repeat can print), and a "mem" marker appears where the ring in
 * place stops short of the delay (pool budget — lux_echo.h).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <functional>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleEditorChrome.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_echo.h"   // self-manages extern "C" linkage

class EchoEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    static constexpr int kGraphH     = 112;  // the graphic frame alone (bars + time ruler)
    // frame + gap + label + box row
    static constexpr int kPreferredH = kGraphH + ModuleChrome::kBelowFrameH;

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

    /** Measured line rate (lines/s) for the ruler and the time readouts;
     *  ≤ 1 means "unknown" and the editor counts lines instead. */
    void setLineRateProvider(std::function<float()> f) { lineRate_ = std::move(f); }

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
        updateView(/*snap*/ true);   // a new instance = its own window, no glide
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Controls OUT of the graphic frame — box row below it.
        auto row = area.removeFromBottom(ModuleChrome::kBoxRowH);
        area.removeFromBottom(ModuleChrome::kRowGap);
        frameRect_ = area.toFloat();
        graphRect_ = ModuleChrome::graphOf(frameRect_);
        ModuleChrome::layoutBoxRow(row, { &boxD, &boxF, &boxM });
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frameRect_, accent);

        const Geometry geo = computeGeometry();
        const float    rate = lineRate();
        if (geo.valid)
        {
            const LuxEchoState& st = *lux_echo_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.ring_active != 0);

            drawRuler(g, geo, rate);

            // Dry impulse at t = 0.
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(geo.x0 - 1.0f, geo.topY, 2.5f, geo.botY - geo.topY));

            // Ring charging: the lines stored since the last re-anchor, until
            // the first repeat can print — you see a long delay coming.
            if (live && (float) st.lines_pushed < geo.delayL)
            {
                const float xe = xForLines(geo, juce::jmin((float) st.lines_pushed, geo.winL));
                g.setColour(accent.withAlpha(0.55f));
                g.fillRect(juce::Rectangle<float>(geo.x0, geo.rulerY - 2.0f, xe - geo.x0, 2.0f));
            }

            // Repeat train: level(n) = mix * fb^(n-1) at x = n * delay.
            const float spacing = xForLines(geo, geo.delayL) - geo.x0;
            const float barW    = spacing < 5.0f ? 1.5f : 3.0f;
            for (int n = 1; n <= 512; ++n)
            {
                const float h = repeatLevel(geo, n);
                const float x = xForLines(geo, (float) n * geo.delayL);
                if (h < kAudible || x > geo.xR) break;
                const float barTop = geo.botY - h * (geo.botY - geo.topY);
                g.setColour(accent.withAlpha(live ? 0.85f : 0.45f));
                g.fillRoundedRectangle(x - barW * 0.5f, barTop, barW, geo.botY - barTop, barW * 0.5f);
            }

            // Decaying envelope hint through the repeat tops.
            if (geo.fbN > 0.01f && geo.mixN > 0.01f)
            {
                juce::Path env;
                bool started = false;
                for (int n = 1; n <= 256; ++n)
                {
                    const float h = repeatLevel(geo, n);
                    const float x = xForLines(geo, (float) n * geo.delayL);
                    if (x > geo.xR) break;
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

            // The train runs past the window (a frozen drag, or > 24 repeats).
            if ((float) juce::jmax(1, geo.nAudible) * geo.delayL > geo.winL)
            {
                juce::Path tri;
                const float cy = (geo.topY + geo.botY) * 0.5f;
                tri.addTriangle(geo.xR - 6.0f, cy - 4.0f, geo.xR - 6.0f, cy + 4.0f, geo.xR, cy);
                g.setColour(accent.withAlpha(0.55f));
                g.fillPath(tri);
            }

            // The ring in place stops short of the delay (pool budget).
            const int cap = lux_echo_capacity(&st);
            if (st.config.enabled != 0 && cap > 0 && (float) cap < geo.delayL)
            {
                const float xc = juce::jmin(geo.xR, xForLines(geo, (float) cap));
                g.setColour(juce::Colour(0xffff6f5a).withAlpha(0.75f));
                for (float y = geo.topY; y < geo.botY; y += 5.0f)
                    g.fillRect(juce::Rectangle<float>(xc - 0.5f, y, 1.0f, 2.5f));
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.drawText("mem", (int) xc + 3, (int) geo.topY, 28, 10,
                           juce::Justification::centredLeft, false);
            }

            drawNode(g, handlePos(Handle::Repeat1, geo), Handle::Repeat1, /*hollow*/ false);
            drawNode(g, handlePos(Handle::Repeat2, geo), Handle::Repeat2, /*hollow*/ true);

            // Live value next to the handle under the hand.
            const Handle shown = (dragging != Handle::None) ? dragging : hovered;
            if (shown == Handle::Repeat1)
                Sp3ctraHandles::drawReadout(g, delayText(geo.delayL, rate),
                                            handlePos(shown, geo), geo.plot);
            else if (shown == Handle::Repeat2)
                Sp3ctraHandles::drawReadout(g, "FB " + juce::String(juce::roundToInt(geo.fbN * 100.0f)) + " %",
                                            handlePos(shown, geo), geo.plot);

            ModuleChrome::drawReadout(g, frameRect_, accent, tailText(geo, rate));
        }

        ModuleChrome::drawCaption(g, frameRect_, accent, "REPEATS");
        ModuleChrome::drawBoxLabel(g, boxD, accent, "Delay");
        ModuleChrome::drawBoxLabel(g, boxF, accent, "Feedback");
        ModuleChrome::drawBoxLabel(g, boxM, accent, "Mix");
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
        if (e.mods.isPopupMenu() || e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick
        dragging = handleAt(e.position, computeGeometry());
        hovered  = dragging;
        if (dragging == Handle::Repeat1)
        {
            del.begin(); mix.begin();
            viewFrozen_     = true;   // the node stays under the pointer
            dragMoved_      = false;  // a plain click must not nudge the values
            lastDragPos_    = e.position;
            lastEdgeZoomMs_ = juce::Time::getMillisecondCounterHiRes();
        }
        if (dragging == Handle::Repeat2) { fbk.begin(); }
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
        Geometry geo = computeGeometry();
        if (!geo.valid) return;
        if (dragging == Handle::Repeat1)
        {
            dragMoved_  = dragMoved_ || e.mouseWasDraggedSinceMouseDown();
            lastDragPos_ = e.position;
            if (dragMoved_) applyRepeat1(e.position);
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
        hold_.release();
        endGesture(dragging);
        dragging = Handle::None;
        viewFrozen_ = false;                  // refit to the train
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
    enum class Handle { None, Repeat1, Repeat2 };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    /** What a handle drives: the double-click resets it, the long press
     *  types it. */
    Sp3ctraGestures::BoundList boundsOf(Handle h)
    {
        switch (h)
        {
            case Handle::Repeat1: return { { "Delay", &del }, { "Mix", &mix } };
            case Handle::Repeat2: return { { "Feedback", &fbk } };
            default: return {};
        }
    }

    void endGesture(Handle h)
    {
        if (h == Handle::Repeat1) { del.end(); mix.end(); }
        if (h == Handle::Repeat2) fbk.end();
    }

    /** Repeat-1 under the pointer: Delay from x, Mix from y. A pointer held
     *  at the right edge zooms the window out (×2 every kEdgeZoomMs) so
     *  longer delays stay one drag away — called from mouseDrag AND from
     *  the timer, since a still pointer sends no drag events. */
    void applyRepeat1(juce::Point<float> p)
    {
        Geometry geo = computeGeometry();
        if (!geo.valid) return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (p.x >= geo.xR - 2.0f && now - lastEdgeZoomMs_ >= kEdgeZoomMs && viewW_ < kWinMax)
        {
            viewW_ = viewTarget_ = juce::jmin(kWinMax, viewW_ * 2.0f);
            lastEdgeZoomMs_ = now;
            geo = computeGeometry();
        }
        del.setGesture(juce::jlimit(1.0f, (float) LUX_ECHO_MAX_DELAY,
                                    std::round(linesForX(geo, p.x))));
        const float m = (geo.botY - p.y) / juce::jmax(1.0f, geo.botY - geo.topY);
        mix.setGesture(juce::jlimit(0.0f, 100.0f, m * 100.0f));
    }

    /** Long press on a handle: the drag gesture closes, the bubble opens. */
    void holdToType()
    {
        const Handle h = dragging;
        endGesture(h);
        dragging    = Handle::None;
        viewFrozen_ = false;
        repaint();
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), boundsOf(h));
    }

    Sp3ctraGestures::Hold hold_;

    //── Time window ──────────────────────────────────────────────────────────
    static constexpr float  kAudible       = 0.015f;   // a repeat below this is not drawn
    static constexpr int    kMaxFitRepeats = 24;       // the window fits at most this many
    static constexpr float  kWinMin        = 64.0f;    // lines
    static constexpr float  kWinMax        = 1048576.0f;
    static constexpr double kEdgeZoomMs    = 220.0;
    static constexpr float  kRulerH        = 14.0f;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float x0 = 0, xR = 0;          // time axis span
        float topY = 0, botY = 0;      // bar area
        float rulerY = 0;              // ruler baseline
        float delayL = 48, fbN = 0, mixN = 0;   // lines, 0..1, 0..1
        float winL = 512;              // window shown (lines)
        int   nAudible = 1;            // repeats at or above kAudible
        bool  valid = false;
    };

    /** Repeats at or above kAudible — the train's length in delays. */
    static int audibleRepeats(float mixN, float fbN) noexcept
    {
        int n = 0;
        float lvl = mixN;
        while (lvl >= kAudible && n < 256) { ++n; lvl *= fbN; }
        return n;
    }

    /** Window that shows the whole train (dry + N repeats + a breath). */
    static float fitWindow(float delayL, int nAudible) noexcept
    {
        return delayL * (float) (juce::jlimit(1, kMaxFitRepeats, nAudible) + 1);
    }

    /** Power-of-two rung holding `fit`, kept while `fit` stays within
     *  (0.4 · current, current] — the ruler does not flicker on a boundary. */
    static float ladder(float fit, float current) noexcept
    {
        if (current > 0.0f && fit <= current && fit > current * 0.4f) return current;
        float w = kWinMin;
        while (w < fit && w < kWinMax) w *= 2.0f;
        return w;
    }

    /** Refit (unless a drag froze the view) and glide the window. */
    void updateView(bool snap = false)
    {
        if (!viewFrozen_)
        {
            const float d  = juce::jlimit(1.0f, (float) LUX_ECHO_MAX_DELAY, del.value);
            const int   n  = audibleRepeats(juce::jlimit(0.0f, 1.0f, mix.value / 100.0f),
                                            juce::jlimit(0.0f, 0.95f, fbk.value / 100.0f));
            viewTarget_ = ladder(fitWindow(d, n), viewTarget_);
        }
        if (snap || viewW_ <= 0.0f) { viewW_ = viewTarget_; return; }
        const float lt = std::log(viewTarget_), lw = std::log(viewW_);
        viewW_ = (std::abs(lt - lw) < 0.004f) ? viewTarget_ : std::exp(lw + (lt - lw) * 0.35f);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 30.0f) return geo;
        geo.plot   = ModuleChrome::plotOf(frameRect_);
        geo.x0     = geo.plot.getX() + 4.0f;
        geo.xR     = geo.plot.getRight() - 2.0f;
        geo.rulerY = geo.plot.getBottom() - kRulerH;
        geo.topY   = geo.plot.getY();
        geo.botY   = geo.rulerY - 3.0f;
        geo.delayL = juce::jlimit(1.0f, (float) LUX_ECHO_MAX_DELAY, del.value);
        geo.fbN    = juce::jlimit(0.0f, 0.95f, fbk.value / 100.0f);
        geo.mixN   = juce::jlimit(0.0f, 1.0f,  mix.value / 100.0f);
        geo.nAudible = audibleRepeats(geo.mixN, geo.fbN);
        geo.winL   = (viewW_ > 0.0f) ? viewW_ : ladder(fitWindow(geo.delayL, geo.nAudible), 0.0f);
        geo.valid  = true;
        return geo;
    }

    float xForLines(const Geometry& geo, float lines) const
    { return geo.x0 + (lines / geo.winL) * (geo.xR - geo.x0); }

    float linesForX(const Geometry& geo, float x) const
    {
        return geo.winL * juce::jlimit(0.0f, 1.0f,
            (x - geo.x0) / juce::jmax(1.0f, geo.xR - geo.x0));
    }

    float repeatLevel(const Geometry& geo, int n) const
    { return geo.mixN * std::pow(geo.fbN, (float) (n - 1)); }

    juce::Point<float> handlePos(Handle h, const Geometry& geo) const
    {
        const int   n = (h == Handle::Repeat1) ? 1 : 2;
        const float x = juce::jlimit(geo.x0 + kNodeR, geo.xR - kNodeR,
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

    //── Ruler + readouts ─────────────────────────────────────────────────────
    float lineRate() const { return lineRate_ ? lineRate_() : 0.0f; }

    /** 1-2-5 step in `unit` giving at most `maxTicks` ticks over `span`. */
    static double niceStep(double span, double maxTicks) noexcept
    {
        const double raw = span / juce::jmax(1.0, maxTicks);
        const double mag = std::pow(10.0, std::floor(std::log10(juce::jmax(1e-9, raw))));
        for (double m : { 1.0, 2.0, 5.0, 10.0 })
            if (m * mag >= raw) return m * mag;
        return 10.0 * mag;
    }

    static juce::String groupDigits(int v)
    {
        juce::String s(v);
        for (int i = s.length() - 3; i > 0; i -= 3)
            s = s.substring(0, i) + " " + s.substring(i);
        return s;
    }

    static juce::String secondsText(double s)
    {
        if (s < 0.9995) return juce::String(juce::roundToInt(s * 1000.0)) + " ms";
        if (s < 9.995)  return juce::String(s, 2) + " s";
        if (s < 99.95)  return juce::String(s, 1) + " s";
        return juce::String(juce::roundToInt(s)) + " s";
    }

    juce::String delayText(float lines, float rate) const
    {
        juce::String t = groupDigits(juce::roundToInt(lines)) + " ln";
        if (rate > 1.0f) t = secondsText(lines / rate) + "  ·  " + t;
        return t;
    }

    /** The whole time window of the echo: dry → last audible repeat. */
    juce::String tailText(const Geometry& geo, float rate) const
    {
        const int   n     = juce::jmax(1, geo.nAudible);
        const float lines = (float) n * geo.delayL;
        juce::String t = "TAIL " + juce::String(n >= 256 ? "> " : "")
                       + (rate > 1.0f ? secondsText(lines / rate) + "  ·  " : juce::String())
                       + groupDigits(juce::roundToInt(lines)) + " ln";
        return t;
    }

    void drawRuler(juce::Graphics& g, const Geometry& geo, float rate) const
    {
        const bool   timed = rate > 1.0f;
        const double span  = timed ? (double) geo.winL / rate : (double) geo.winL;   // s or lines
        const bool   ms    = timed && span < 2.0;
        const double scale = ms ? 1000.0 : 1.0;
        const double spanU = span * scale;
        const double pxPerU = (geo.xR - geo.x0) / juce::jmax(1e-9, spanU);
        const double step  = niceStep(spanU, (geo.xR - geo.x0) / 72.0);
        const int    dec   = step < 1.0 ? (step < 0.1 ? 2 : 1) : 0;
        const juce::String unit = timed ? (ms ? " ms" : " s") : " ln";
        const juce::Font font { juce::FontOptions(Sp3ctraTheme::kFontTiny) };

        g.setColour(accent.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(geo.x0, geo.rulerY, geo.xR - geo.x0, 1.0f));
        g.setFont(font);
        for (double v = 0.0; v <= spanU + step * 1e-6; v += step)
        {
            const float x = (float) (geo.x0 + v * pxPerU);
            g.setColour(accent.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(x - 0.5f, geo.rulerY - 3.0f, 1.0f, 4.0f));
            const juce::String label = (v <= 0.0) ? juce::String("0")
                                     : (dec > 0 ? juce::String(v, dec) : juce::String(juce::roundToInt(v))) + unit;
            const int w = juce::GlyphArrangement::getStringWidthInt(font, label) + 2;
            int lx = (int) x + 3;
            if (lx + w > (int) geo.xR) lx = (int) x - 3 - w;
            g.setColour(accent.withAlpha(0.5f));
            g.drawText(label, lx, (int) geo.rulerY + 1, w, (int) kRulerH - 1,
                       juce::Justification::centredLeft, false);
        }
    }

    /** Handle painter — Repeat-1 is a filled node, Repeat-2 a hollow ring
     *  (Sp3ctraHandles: Idle / Hover / Drag are distinct states). */
    void drawNode(juce::Graphics& g, juce::Point<float> pt, Handle h, bool hollow) const
    {
        const auto s = Sp3ctraHandles::stateOf(h == dragging,
                                               dragging == Handle::None && h == hovered,
                                               false, handleHeat(h));
        if (hollow) Sp3ctraHandles::drawRing(g, pt, s);
        else        Sp3ctraHandles::drawNode(g, pt, s);
    }

    //==========================================================================
    void timerCallback() override
    {
        if (dragging == Handle::Repeat1 && dragMoved_ && !hold_.fired())
            applyRepeat1(lastDragPos_);   // edge zoom while the pointer holds still
        updateView();
        if (isShowing()) repaint();
    }

    //==========================================================================
    /** Remote-edit heat of the parameter(s) a handle drives — a change from
     *  the box below, a MIDI CC or automation lights the handle exactly like
     *  a drag (ui/Sp3ctraControls.h). */
    float handleHeat(Handle h) const noexcept
    {
        switch (h)
        {
            case Handle::Repeat1: return juce::jmax(del.heat(), mix.heat());
            case Handle::Repeat2: return fbk.heat();
            default:              return 0.0f;
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

    Bound del, fbk, mix;
    Sp3ctraBarSlider boxD, boxF, boxM;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxDAtt, boxFAtt, boxMAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnD_, learnF_, learnM_;
    std::function<float()> lineRate_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // graph area inside the frame (ModuleChrome::graphOf)
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    float  viewW_      { 0.0f };   // window shown (lines) — glides to viewTarget_
    float  viewTarget_ { 0.0f };
    bool   viewFrozen_ { false };  // a Repeat-1 drag keeps the node under the pointer
    bool   dragMoved_  { false };  // the pointer travelled since mouseDown (a real drag)
    double lastEdgeZoomMs_ { 0.0 };
    juce::Point<float> lastDragPos_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoEditorComponent)
};
