/**
 * @file VideoScrollViewportEditor.h
 * @brief The VIEWPORT pad of the VIDEO SCROLL page — the output window drawn
 *        as a live thumbnail, with the generation frame (the "vignette")
 *        laid on it as a grabbable object.
 *
 * What is drawn (window space, the output's real aspect):
 *   • the window rect, filled with the latest VIDEO MIX composite (or the
 *     frame colour through the Invert / Color law while nothing is rendered);
 *   • the sweep band — the strip the waterfall actually covers (Zoom wide,
 *     running edge to edge along the scroll axis) — bright, the rest veiled;
 *   • the vignette — the zoom frame (Zoom × the view itself, W × H, turned
 *                    by Rotation — rotation-invariant like a camera viewfinder,
 *     centred by Center X / Y, turned by Rotation) hosting the birth line;
 *   • the birth line inside it (Line Pos);
 *   • the parameters without a handle, made visible ON the axis (see
 *     drawFlowAndAging): the stamped bar (Thickness) as a band centred on
 *     the line; on each side of the line that is still inside the window, a
 *     time ruler whose ticks bunch toward the edge with Compression, go out
 *     with Fade and widen with Blur; and 1–3 flow chevrons for |Speed|,
 *     pointing away from the line (toward it when Speed < 0). The renderer
 *     pushes history away from the line on BOTH sides, so a single outward
 *     arrow was wrong — and vanished when the line sat on an edge;
 *   • the handles: centre node + outline (move), four corner rings (zoom), a
 *     lever on the frame's leading edge (rotation). Hovering a handle names
 *     it with its value; a key of the drawing sits in the free zone left of
 *     the window when there is room.
 *
 * Gestures (VideoScrollRenderCore::drawWarp / birthLine01 are the model —
 * the pad reproduces their geometry exactly, so what you grab is what the
 * output does):
 *   • drag the vignette          Center X / Center Y (±1 = centre on the edge)
 *   • drag a corner ring         Zoom (uniform, about the vignette centre)
 *   • drag the lever, or drag    Rotation (degrees clockwise, wraps at 360)
 *     anywhere outside the vignette (dial around its centre)
 *   • drag the birth line        Line Pos (along the scroll axis, inside the
 *                                zoom frame; clamped to the window like the
 *                                renderer)
 *   • pinch (trackpad) / ⌘ or Ctrl + wheel   Zoom — a plain wheel keeps
 *                                scrolling the page (editor rule)
 *   • double-click               reset what is under the pointer: vignette →
 *                                centre 0/0, corner → 1.0×, lever/outside →
 *                                0°, birth line → its default
 *   • right-click                MIDI Learn of the parameter(s) under the
 *                                pointer (both centres on the vignette; all
 *                                five outside)
 *
 * Colour split (charter): the window, band, axis, arrow and captions are
 * DISPLAY (module colour); everything grabbable is lime (Sp3ctraHandles).
 * The numeric boxes live in the page's row BELOW the frame, never inside.
 *
 * Hosted once per output page (the chain tab AND every section of the ALL
 * view): no timer — the editor's 20 Hz tick calls previewTick(), which keeps
 * the mixer's solo render of THIS output alive (the thumbnail is the output
 * being edited, alone — not the VIDEO MIX composite) and repaints only when
 * a new frame was published or the view size changed.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../ui/Sp3ctraHandles.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/ModuleParamManifest.h"   // vsParam()
#include "VideoScrollMode.h"             // VideoScrollLimits
#include "VideoScrollPreviewSource.h"
#include "VideoScrollRenderCore.h"       // applyDisplayColour (frame colour law)

class VideoScrollViewportEditor : public juce::Component,
                                  public juce::SettableTooltipClient
{
    struct Geo;   // screen geometry — defined below (private), used by member signatures above it
public:
    static constexpr int kGraphH     = 240;      // the frame alone (boxes live in the page)
    static constexpr int kPreferredH = kGraphH;

    VideoScrollViewportEditor(juce::AudioProcessorValueTreeState& apvtsIn,
                              juce::Colour accentColour)
        : apvts_(apvtsIn), accent_(accentColour)
    {
        setRepaintsOnMouseActivity(true);
        setTooltip(juce::String::fromUTF8(
            "VIEWPORT \xE2\x80\x94 the output window\n"
            "Drag the frame: Center X / Y \xC2\xB7 corner: Zoom \xC2\xB7 lever or outside: Rotation\n"
            "Birth line: Line Pos \xC2\xB7 pinch or \xE2\x8C\x98+wheel: Zoom \xC2\xB7 double-click: reset"));
    }

    /** Optional MIDI-learn wiring — right-click menus on the handles. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** Where the live thumbnail and the view aspect come from (the mixer).
     *  Null = schematic pad on the frame colour, square window. */
    void setPreviewSource(VideoScrollPreviewSource* src) noexcept
    {
        source_ = src;
        lastFrame_ = 0;
        repaint();
    }

    /** Bind the handles to the videoScroll{slot}_* bank; slot < 0 unbinds. */
    void setSlot(int slot)
    {
        slot_ = slot;
        for (auto* b : { &rot_, &zoom_, &cx_, &cy_, &line_ }) b->attach.reset();
        if (slot_ < 0) { repaint(); return; }
        bind(rot_,  vsParam(slot_, "rotation"));
        bind(zoom_, vsParam(slot_, "zoom"));
        bind(cx_,   vsParam(slot_, "centerX"));
        bind(cy_,   vsParam(slot_, "centerY"));
        bind(line_, vsParam(slot_, "linePos"));
        repaint();
    }

    int slot() const noexcept { return slot_; }

    /** Editor timer (20 Hz): keep this output's solo render alive and repaint
     *  when the frame or the view changed. Cheap when hidden (the ALL view
     *  keeps the single page alive) — a hidden pad requests nothing, so the
     *  mixer stops soloing its output. */
    void previewTick()
    {
        if (slot_ < 0 || source_ == nullptr || ! isShowing()) return;
        source_->requestOutputPreview(slot_);
        const uint32_t fc = source_->frameCounter();
        const auto vs = source_->viewSize();
        if (fc != lastFrame_ || vs != lastView_)
        {
            lastFrame_ = fc;
            lastView_  = vs;
            repaint();
        }
    }

    //==========================================================================
    void resized() override
    {
        frame_ = getLocalBounds().toFloat();
        plot_  = ModuleChrome::plotOf(frame_);
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frame_, accent_);
        ModuleChrome::drawCaption(g, frame_, accent_, "VIEWPORT");

        if (slot_ < 0)
        {
            ModuleChrome::drawReadout(g, frame_, accent_, "select a VIDEO SCROLL block");
            return;
        }

        const Geo geo = computeGeometry();
        if (! geo.valid) return;

        ModuleChrome::drawReadout(g, frame_, accent_, readoutText());

        // ── Window: thumbnail (this output alone), veiled outside the band ──
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(geo.win.getSmallestIntegerContainer());
            const juce::Image img = source_ != nullptr ? source_->outputFrame(slot_) : juce::Image();
            auto paintWindow = [&]
            {
                g.setColour(paperColour());
                g.fillRect(geo.win);
                if (img.isValid())
                {
                    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
                    g.drawImage(img, geo.win, juce::RectanglePlacement::stretchToFit);
                }
            };
            paintWindow();
            g.setColour(juce::Colours::black.withAlpha(0.5f));   // veil = outside the band
            g.fillRect(geo.win);
            g.reduceClipRegion(bandPath(geo));
            paintWindow();
        }

        // ── Display layer (module colour): window edge, band edges, axis ────
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(plot_.getSmallestIntegerContainer());

            g.setColour(accent_.withAlpha(0.55f));
            g.drawRect(geo.win, 1.0f);

            const float L = geo.k * std::hypot(geo.W, geo.H);   // covers the window at any angle
            g.setColour(accent_.withAlpha(0.35f));
            for (float sgn : { -1.0f, 1.0f })
            {
                const auto a = geo.C + geo.u * (sgn * geo.hx) - geo.v * L;
                const auto b = geo.C + geo.u * (sgn * geo.hx) + geo.v * L;
                g.drawLine({ a, b }, 1.0f);
            }
            const float dash[2] = { 4.0f, 4.0f };
            g.setColour(accent_.withAlpha(0.3f));
            g.drawDashedLine({ geo.C - geo.v * L, geo.C + geo.v * L }, dash, 2, 1.0f);

            // Thickness bar, time ruler (compression / fade / blur), flow
            // chevrons (speed) — inside the window, on the axis.
            drawFlowAndAging(g, geo);

            // ── Controls (lime): vignette, birth line, corners, lever, centre ──
            const auto sBody   = stateOf(Handle::Body);
            const auto sLine   = stateOf(Handle::Birth);
            const auto sLever  = stateOf(Handle::Lever);
            const auto sCorner = stateOf(Handle::Corner);

            juce::Path outline = vignettePath(geo);
            if (Sp3ctraHandles::isHot(sBody))
            {
                g.setColour(Sp3ctraHandles::colour().withAlpha(0.08f));
                g.fillPath(outline);
            }
            g.setColour(Sp3ctraHandles::ringColour(sBody)
                            .withMultipliedAlpha(Sp3ctraHandles::isHot(sBody) ? 1.0f : 0.85f));
            g.strokePath(outline, juce::PathStrokeType(Sp3ctraHandles::isHot(sBody) ? 2.0f : 1.2f));

            Sp3ctraHandles::drawGrabLine(g, { geo.birth - geo.u * geo.hx, geo.birth + geo.u * geo.hx }, sLine);

            Sp3ctraHandles::drawLink(g, geo.C - geo.v * geo.hy, geo.lever, sLever);
            Sp3ctraHandles::drawNode(g, geo.lever, sLever, 4.0f);

            for (const auto& c : geo.corner)
                Sp3ctraHandles::drawRing(g, c, sCorner);

            Sp3ctraHandles::drawNode(g, geo.C, sBody);

            // Name + value of the handle under the pointer (or being dragged).
            // The dial (anywhere outside the vignette) is not named on hover —
            // it would label the whole pad.
            const Handle named = dragging_ != Handle::None ? dragging_
                               : (hovered_ != Handle::Dial ? hovered_ : Handle::None);
            if (named != Handle::None)
                Sp3ctraHandles::drawReadout(g, handleReadout(named), anchorOf(named, geo), plot_);
        }

        drawKey(g, geo);
    }

    /** The parameters without a handle, made visible on the axis. Mirrors
     *  VideoScrollRenderCore (scrollStep: the stamp is centred on the birth
     *  line and both zones move AWAY from it, toward it in reverse; buildWarp:
     *  kCompMax / kFadeRate, aging 0 at the line → 1 at the nearest window
     *  edge on each side):
     *   • Thickness — the stamped bar, 1 + t·(z·H − 1) px, as a translucent
     *     band centred on the birth line;
     *   • per side with room in the window: N ticks at EQUAL history
     *     intervals — the compression map d + c·d²/S is inverted so they
     *     bunch toward the edge as Compression rises — dimmed by Fade
     *     (exp(−10·fade·age)) and widened by Blur;
     *   • 1–3 flow chevrons for |Speed| next to the line, pointing away from
     *     it (toward it when Speed < 0), dim when Speed = 0.
     *  Everything stays INSIDE the window: a line pushed to an edge keeps its
     *  indicators on the side that is still visible. */
    void drawFlowAndAging(juce::Graphics& g, const Geo& geo) const
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(geo.win.getSmallestIntegerContainer());

        const float speed  = juce::jlimit(-1.f, 1.f, readRaw("speed",     0.33f));
        const float thick  = juce::jlimit( 0.f, 1.f, readRaw("thickness", 0.f));
        const float comp01 = juce::jlimit( 0.f, 1.f, (readRaw("compress", 1.f) - 1.0f) / 63.0f);
        const float fade01 = juce::jlimit( 0.f, 1.f, readRaw("fade",      0.f));
        const float blur01 = juce::jlimit( 0.f, 1.f, readRaw("blur",      0.f));
        constexpr float kCompMax  = 2.5f;    // = VideoScrollRenderCore::buildWarp
        constexpr float kFadeRate = 10.0f;

        // Thickness: the stamped bar, centred on the birth line.
        const float hb = 0.5f * (1.0f + thick * (geo.z * geo.H - 1.0f)) * geo.k;
        if (hb > 1.5f)
        {
            juce::Path bar;
            bar.startNewSubPath(geo.birth - geo.u * geo.hx - geo.v * hb);
            bar.lineTo         (geo.birth + geo.u * geo.hx - geo.v * hb);
            bar.lineTo         (geo.birth + geo.u * geo.hx + geo.v * hb);
            bar.lineTo         (geo.birth - geo.u * geo.hx + geo.v * hb);
            bar.closeSubPath();
            g.setColour(accent_.withAlpha(0.22f));
            g.fillPath(bar);
            g.setColour(accent_.withAlpha(0.5f));
            g.strokePath(bar, juce::PathStrokeType(1.0f));
        }

        for (float sgn : { -1.0f, 1.0f })
        {
            const auto  dir  = geo.v * sgn;
            const float room = rayExit(geo.birth, dir, geo.win);   // = the aging span (screen px)
            if (room < 8.0f) continue;

            // Time ruler: N equal history intervals mapped to the screen
            // through the compression law (closed-form inverse).
            constexpr int N = 8;
            const float c = comp01 * kCompMax;
            for (int i = 1; i < N; ++i)
            {
                const float gi = (float) i / (float) N * room * (1.0f + c);
                const float d  = c > 1.0e-4f
                    ? (std::sqrt(1.0f + 4.0f * c * gi / room) - 1.0f) * room / (2.0f * c)
                    : gi;
                const float age   = d / room;
                const float alpha = 0.6f * std::exp(-kFadeRate * fade01 * age);
                if (alpha < 0.03f) break;
                const float w = 3.0f + 5.0f * blur01 * age;
                const auto  p = geo.birth + dir * d;
                g.setColour(accent_.withAlpha(alpha));
                g.drawLine({ p - geo.u * w, p + geo.u * w }, 1.0f);
            }

            // Flow chevrons.
            int n = speed == 0.0f ? 1 : 1 + (int) std::lround(2.0f * std::abs(speed));
            n = juce::jmin(n, (int) ((room - 6.0f) / 6.0f));
            if (n < 1) continue;
            const auto head = speed < 0.0f ? -dir : dir;
            g.setColour(accent_.withAlpha(speed == 0.0f ? 0.35f : 0.85f));
            for (int i = 0; i < n; ++i)
            {
                const auto tip = geo.birth + dir * (8.0f + 6.0f * (float) i) + head * 2.5f;
                juce::Path ch;
                ch.startNewSubPath(tip - head * 5.0f - geo.u * 4.0f);
                ch.lineTo(tip);
                ch.lineTo(tip - head * 5.0f + geo.u * 4.0f);
                g.strokePath(ch, juce::PathStrokeType(1.5f));
            }
        }
    }

    /** Distance from `o` along the unit vector `d` to where the ray leaves
     *  the axis-aligned `r` (0 when `o` is outside or on the facing edge). */
    static float rayExit(juce::Point<float> o, juce::Point<float> d, juce::Rectangle<float> r)
    {
        if (! r.expanded(1.0f).contains(o)) return 0.0f;
        float t = 1.0e9f;
        if (d.x >  1.0e-6f) t = juce::jmin(t, (r.getRight()  - o.x) / d.x);
        if (d.x < -1.0e-6f) t = juce::jmin(t, (r.getX()      - o.x) / d.x);
        if (d.y >  1.0e-6f) t = juce::jmin(t, (r.getBottom() - o.y) / d.y);
        if (d.y < -1.0e-6f) t = juce::jmin(t, (r.getY()      - o.y) / d.y);
        return juce::jmax(0.0f, t);
    }

    /** Key of the drawing, in the free zone left of the window (only when
     *  it fits — a narrow page keeps the hover names alone). */
    void drawKey(juce::Graphics& g, const Geo& geo) const
    {
        constexpr int   kLineH = 12;
        constexpr float kColW  = 58.0f;
        static const char* const rows[][2] = {
            { "frame",    "center x / y" },
            { "corner",   "zoom" },
            { "lever",    "rotation" },
            { "line",     "line pos" },
            { "bar",      "thickness" },
            { "ticks",    "age: compression / fade / blur" },
            { "chevrons", "speed" },
        };
        constexpr int nRows = (int) (sizeof(rows) / sizeof(rows[0]));
        const float zoneW = geo.win.getX() - plot_.getX() - 8.0f;
        if (zoneW < 210.0f || plot_.getHeight() < (float) (nRows * kLineH + 8)) return;

        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        float y = plot_.getCentreY() - 0.5f * (float) (nRows * kLineH);
        const float x = plot_.getX() + 4.0f;
        for (const auto& r : rows)
        {
            const juce::Rectangle<int> a((int) x, (int) y, (int) kColW, kLineH);
            const juce::Rectangle<int> b((int) (x + kColW), (int) y, (int) (zoneW - kColW), kLineH);
            g.setColour(accent_.withAlpha(0.8f));
            g.drawText(r[0], a, juce::Justification::centredLeft, false);
            g.setColour(accent_.withAlpha(0.55f));
            g.drawText(r[1], b, juce::Justification::centredLeft, false);
            y += (float) kLineH;
        }
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging_ != Handle::None) return;
        const Handle h = handleAt(e.position);
        if (h != hovered_) { hovered_ = h; repaint(); }
        setMouseCursor(cursorFor(h));
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging_ == Handle::None && hovered_ != Handle::None) { hovered_ = Handle::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (slot_ < 0) return;
        const Geo geo = computeGeometry();
        const Handle h = handleAt(e.position);

        if (e.mods.isPopupMenu())
        {
            showLearnMenu(h);
            return;
        }
        // Second click of a double-click: no gesture — mouseDoubleClick resets.
        if (e.getNumberOfClicks() > 1 || ! geo.valid) return;

        dragging_ = h;
        hovered_  = h;
        start_    = { geo.C, e.position, rot_.value, zoom_.value, cx_.value, cy_.value, line_.value };
        switch (h)
        {
            case Handle::Body:   cx_.begin(); cy_.begin(); break;
            case Handle::Corner: zoom_.begin();            break;
            case Handle::Lever:
            case Handle::Dial:   rot_.begin();             break;
            case Handle::Birth:  line_.begin();            break;
            case Handle::None:   break;
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging_ == Handle::None || e.mods.isPopupMenu()) return;
        const Geo geo = computeGeometry();
        if (! geo.valid) return;
        const auto p  = e.position;
        const auto d0 = start_.mouse - start_.centre;
        const auto d  = p - start_.centre;

        switch (dragging_)
        {
            case Handle::Body:
            {
                // Window-space offsets: ±1 = the centre on the window edge.
                const float dx = (p.x - start_.mouse.x) / juce::jmax(1.0f, geo.k * geo.W * 0.5f);
                const float dy = (p.y - start_.mouse.y) / juce::jmax(1.0f, geo.k * geo.H * 0.5f);
                cx_.setGesture(juce::jlimit(-1.0f, 1.0f, start_.cx + dx));
                cy_.setGesture(juce::jlimit(-1.0f, 1.0f, start_.cy + dy));
                break;
            }
            case Handle::Corner:
            {
                // Uniform zoom about the vignette centre: the corner follows
                // the pointer's distance from it.
                const float r0 = juce::jmax(4.0f, d0.getDistanceFromOrigin());
                const float r  = d.getDistanceFromOrigin();
                zoom_.setGesture(clampZoom(start_.zoom * r / r0));
                break;
            }
            case Handle::Lever:
            case Handle::Dial:
            {
                // Dial: the angle swept around the vignette centre.
                if (d.getDistanceFromOrigin() < 2.0f) break;
                const float a0 = std::atan2(d0.y, d0.x), a1 = std::atan2(d.y, d.x);
                rot_.setGesture(wrapDeg(start_.rot + juce::radiansToDegrees(a1 - a0)));
                break;
            }
            case Handle::Birth:
            {
                // Position along the scroll axis, relative to the vignette
                // centre, mapped through the zoom frame (birthLine01).
                const auto  rel = p - geo.C;
                const float t   = (rel.x * geo.v.x + rel.y * geo.v.y) / geo.k;   // view px
                const float pos = 0.5f + t / juce::jmax(1.0f, geo.z * geo.H);
                line_.setGesture(juce::jlimit(-1.0f, 1.0f, pos * 2.0f - 1.0f));
                break;
            }
            case Handle::None: break;
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        switch (dragging_)
        {
            case Handle::Body:   cx_.end(); cy_.end(); break;
            case Handle::Corner: zoom_.end();          break;
            case Handle::Lever:
            case Handle::Dial:   rot_.end();           break;
            case Handle::Birth:  line_.end();          break;
            case Handle::None:   break;
        }
        dragging_ = Handle::None;
        hovered_  = handleAt(e.position);
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (slot_ < 0 || e.mods.isPopupMenu()) return;
        switch (handleAt(e.position))
        {
            case Handle::Body:   cx_.reset(); cy_.reset(); break;
            case Handle::Corner: zoom_.reset();            break;
            case Handle::Lever:
            case Handle::Dial:   rot_.reset();             break;
            case Handle::Birth:  line_.reset();            break;
            case Handle::None:   break;
        }
    }

    /** ⌘ / Ctrl + wheel = zoom; a plain wheel is left to the page viewport. */
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (slot_ < 0 || ! (e.mods.isCommandDown() || e.mods.isCtrlDown()))
        {
            juce::Component::mouseWheelMove(e, w);   // propagate: page scroll
            return;
        }
        if (w.deltaY == 0.0f || dragging_ != Handle::None) return;
        zoom_.set(clampZoom(zoom_.value * std::exp2(w.deltaY * 2.0f)));
    }

    /** Trackpad pinch = zoom. */
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override
    {
        if (slot_ < 0 || dragging_ != Handle::None || scaleFactor <= 0.0f) return;
        zoom_.set(clampZoom(zoom_.value * scaleFactor));
    }

private:
    //==========================================================================
    enum class Handle { None, Body, Corner, Lever, Dial, Birth };

    /** Everything in SCREEN px, derived from the params + the view aspect,
     *  with the same maths as VideoScrollRenderCore (visibleSpans /
     *  centreOffset / birthLine01 / drawWarp). */
    struct Geo
    {
        bool  valid = false;
        float W = 1, H = 1;                    // view (window) size, view px
        float k = 1;                           // screen px per view px
        juce::Rectangle<float> win;            // the window on screen
        juce::Point<float> O;                  // window centre
        juce::Point<float> u, v;               // unit transverse / scroll-axis vectors
        float sx = 1, sy = 1;                  // visible spans (view px) — clamp only
        float z  = 1;
        juce::Point<float> C;                  // vignette centre
        float hx = 0, hy = 0;                  // vignette half sizes (screen px)
        juce::Point<float> corner[4];
        juce::Point<float> birth;              // birth line centre
        juce::Point<float> lever;              // rotation handle
    };

    Geo computeGeometry() const
    {
        Geo g;
        if (plot_.getWidth() < 20.0f || plot_.getHeight() < 20.0f) return g;

        // View aspect from the mixer (square column preview by default).
        juce::Point<int> vs;
        if (source_ != nullptr) vs = source_->viewSize();
        g.W = (float) (vs.x > 0 && vs.y > 0 ? vs.x : 1);
        g.H = (float) (vs.x > 0 && vs.y > 0 ? vs.y : 1);

        g.k   = juce::jmin(plot_.getWidth() / g.W, plot_.getHeight() / g.H);
        g.win = juce::Rectangle<float>(g.W * g.k, g.H * g.k).withCentre(plot_.getCentre());
        g.O   = g.win.getCentre();

        const float th = juce::degreesToRadians(wrapDeg(rot_.value));
        const float c  = std::cos(th), s = std::sin(th);
        g.u = { c, s };          // canvas x → screen (JUCE clockwise rotation)
        g.v = { -s, c };         // canvas y → screen
        g.sx = g.W * std::abs(c) + g.H * std::abs(s);
        g.sy = g.W * std::abs(s) + g.H * std::abs(c);
        g.z  = clampZoom(zoom_.value);

        // Centre offsets are WINDOW offsets (X → right, Y → down, any angle).
        const float vx = juce::jlimit(-1.f, 1.f, cx_.value) * 0.5f * g.W;
        const float vy = juce::jlimit(-1.f, 1.f, cy_.value) * 0.5f * g.H;
        const float oy = -vx * s + vy * c;   // along the scroll axis (canvas frame)
        g.C  = g.O + juce::Point<float>(vx, vy) * g.k;
        // Zoom frame = z × the view's own dims, whatever the angle (frameSpans):
        // rotating the vignette never resizes it.
        g.hx = 0.5f * g.z * g.W * g.k;
        g.hy = 0.5f * g.z * g.H * g.k;

        g.corner[0] = g.C - g.u * g.hx - g.v * g.hy;
        g.corner[1] = g.C + g.u * g.hx - g.v * g.hy;
        g.corner[2] = g.C + g.u * g.hx + g.v * g.hy;
        g.corner[3] = g.C - g.u * g.hx + g.v * g.hy;

        // Birth line: Line Pos inside the zoom frame, clamped to the visible
        // span about the WINDOW centre (birthLine01) — measured along the axis.
        const float posNorm = (juce::jlimit(-1.f, 1.f, line_.value) + 1.f) * 0.5f;
        const float tb = juce::jlimit(-0.5f * g.sy, 0.5f * g.sy,
                                      oy + (posNorm - 0.5f) * g.z * g.H);
        g.birth = g.C + g.v * ((tb - oy) * g.k);

        g.lever = g.C - g.v * (g.hy + kLeverLen);
        g.valid = true;
        return g;
    }

    static juce::Path vignettePath(const Geo& g)
    {
        juce::Path p;
        p.startNewSubPath(g.corner[0]);
        for (int i = 1; i < 4; ++i) p.lineTo(g.corner[i]);
        p.closeSubPath();
        return p;
    }

    /** The sweep band: vignette width, edge to edge along the scroll axis. */
    static juce::Path bandPath(const Geo& g)
    {
        const float L = g.k * std::hypot(g.W, g.H);
        juce::Path p;
        p.startNewSubPath(g.C - g.u * g.hx - g.v * L);
        p.lineTo         (g.C + g.u * g.hx - g.v * L);
        p.lineTo         (g.C + g.u * g.hx + g.v * L);
        p.lineTo         (g.C - g.u * g.hx + g.v * L);
        p.closeSubPath();
        return p;
    }

    Handle handleAt(juce::Point<float> p) const
    {
        if (slot_ < 0) return Handle::None;
        const Geo g = computeGeometry();
        if (! g.valid || ! plot_.contains(p)) return Handle::None;

        if (p.getDistanceFrom(g.lever) <= kHitR) return Handle::Lever;
        for (const auto& c : g.corner)
            if (p.getDistanceFrom(c) <= kHitR) return Handle::Corner;
        juce::Point<float> onLine;
        if (juce::Line<float>(g.birth - g.u * g.hx, g.birth + g.u * g.hx)
                .getDistanceFromPoint(p, onLine) <= kLineHitR)
            return Handle::Birth;

        const auto  rel = p - g.C;
        const float a   = rel.x * g.u.x + rel.y * g.u.y;
        const float b   = rel.x * g.v.x + rel.y * g.v.y;
        if (std::abs(a) <= g.hx && std::abs(b) <= g.hy) return Handle::Body;
        return Handle::Dial;
    }

    Sp3ctraHandles::State stateOf(Handle h) const noexcept
    {
        return Sp3ctraHandles::stateOf(dragging_ == h,
                                       dragging_ == Handle::None && hovered_ == h);
    }

    static juce::MouseCursor cursorFor(Handle h)
    {
        switch (h)
        {
            case Handle::Body:   return juce::MouseCursor::DraggingHandCursor;
            case Handle::Corner: return juce::MouseCursor::CrosshairCursor;
            case Handle::Lever:  return juce::MouseCursor::PointingHandCursor;
            case Handle::Birth:  return juce::MouseCursor::CrosshairCursor;
            case Handle::Dial:
            case Handle::None:   break;
        }
        return juce::MouseCursor::NormalCursor;
    }

    //── Readouts ──────────────────────────────────────────────────────────────
    static juce::String deg()   { return juce::String::fromUTF8("\xC2\xB0"); }
    static juce::String times() { return juce::String::fromUTF8("\xC3\x97"); }
    static juce::String dot()   { return juce::String::fromUTF8(" \xC2\xB7 "); }

    juce::String readoutText() const
    {
        return juce::String(wrapDeg(rot_.value), 1) + deg() + dot()
             + juce::String(clampZoom(zoom_.value), 2) + times() + dot()
             + "x " + juce::String(cx_.value, 2) + "  y " + juce::String(cy_.value, 2) + dot()
             + "line " + juce::String(line_.value, 2);
    }

    /** Name + value of a handle (hover and drag readout). */
    juce::String handleReadout(Handle h) const
    {
        switch (h)
        {
            case Handle::Body:   return "Center  x " + juce::String(cx_.value, 2) + "  y " + juce::String(cy_.value, 2);
            case Handle::Corner: return "Zoom  " + juce::String(clampZoom(zoom_.value), 2) + times();
            case Handle::Lever:
            case Handle::Dial:   return "Rotation  " + juce::String(wrapDeg(rot_.value), 1) + deg();
            case Handle::Birth:  return "Line Pos  " + juce::String(line_.value, 2);
            case Handle::None:   break;
        }
        return {};
    }

    juce::Point<float> anchorOf(Handle h, const Geo& g) const
    {
        switch (h)
        {
            case Handle::Body:   return g.C;
            case Handle::Corner: return g.corner[2];
            case Handle::Lever:
            case Handle::Dial:   return g.lever;
            case Handle::Birth:  return g.birth;
            case Handle::None:   break;
        }
        return g.C;
    }

    //── MIDI learn (right-click) ──────────────────────────────────────────────
    void showLearnMenu(Handle h)
    {
        if (midiMap_ == nullptr || slot_ < 0) return;
        struct Entry { const char* label; const char* suffix; };
        const Entry all[]  = { { "Rotation", "rotation" }, { "Zoom", "zoom" }, { "Center X", "centerX" },
                               { "Center Y", "centerY" }, { "Line Pos", "linePos" } };
        const Entry body[] = { { "Center X", "centerX" }, { "Center Y", "centerY" } };

        const Entry* list = all;
        int n = 5;
        const char* single = nullptr;
        switch (h)
        {
            case Handle::Body:   list = body; n = 2;   break;
            case Handle::Corner: single = "zoom";      break;
            case Handle::Lever:  single = "rotation";  break;
            case Handle::Birth:  single = "linePos";   break;
            case Handle::Dial:
            case Handle::None:   break;
        }
        if (single != nullptr)
        {
            MidiLearnPopup::show(*midiMap_, vsParam(slot_, single), this);
            return;
        }

        // Several parameters under the pointer: one sub-menu each.
        juce::PopupMenu menu;
        juce::StringArray ids;
        for (int i = 0; i < n; ++i)
        {
            ids.add(vsParam(slot_, list[i].suffix));
            juce::PopupMenu sub;
            MidiLearnPopup::addItems(sub, *midiMap_, ids[i], (i + 1) * MidiLearnPopup::kItemCount);
            menu.addSubMenu(list[i].label, sub);
        }
        MidiMappingEngine& engine = *midiMap_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                           [&engine, ids](int choice)
                           {
                               for (int i = 0; i < ids.size(); ++i)
                                   if (MidiLearnPopup::handle(engine, ids[i], choice,
                                                              (i + 1) * MidiLearnPopup::kItemCount))
                                       return;
                           });
    }

    //── Parameter binding ─────────────────────────────────────────────────────
    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float value = 0.0f;
        void begin()             { if (attach) attach->beginGesture(); }
        void end()               { if (attach) attach->endGesture(); }
        void setGesture(float v) { if (attach) attach->setValueAsPartOfGesture(v); }
        void set(float v)        { if (attach) attach->setValueAsCompleteGesture(v); }
        void reset()
        {
            if (attach && param)
                attach->setValueAsCompleteGesture(param->convertFrom0to1(param->getDefaultValue()));
        }
    };

    void bind(Bound& b, const juce::String& id)
    {
        b.param = apvts_.getParameter(id);
        jassert(b.param != nullptr);
        if (b.param == nullptr) return;
        b.attach = std::make_unique<juce::ParameterAttachment>(
            *b.param, [this, &b](float v) { b.value = v; repaint(); });
        b.attach->sendInitialUpdate();
    }

    float readRaw(const char* suffix, float def) const
    {
        if (slot_ < 0) return def;
        if (auto* v = apvts_.getRawParameterValue(vsParam(slot_, suffix))) return v->load();
        return def;
    }

    /** The frame colour through the Invert / Color law (= the output's
     *  border) — the "nothing rendered yet" paper of the window. */
    juce::Colour paperColour() const
    {
        int r = (int) std::lround(juce::jlimit(0.f, 1.f, readRaw("bgR", 1.f)) * 255.f);
        int g = (int) std::lround(juce::jlimit(0.f, 1.f, readRaw("bgG", 1.f)) * 255.f);
        int b = (int) std::lround(juce::jlimit(0.f, 1.f, readRaw("bgB", 1.f)) * 255.f);
        int inv = (int) readRaw("invertMode", 0.f);
        if (inv == 0 && readRaw("invert", 0.f) > 0.5f) inv = 1;   // legacy bool
        VideoScrollRenderCore::applyDisplayColour(r, g, b, readRaw("colorMode", 1.f) > 0.5f, inv);
        return juce::Colour((juce::uint8) r, (juce::uint8) g, (juce::uint8) b);
    }

    static float clampZoom(float z) noexcept
    {
        return juce::jlimit(VideoScrollLimits::kZoomMin, VideoScrollLimits::kZoomMax, z);
    }

    static float wrapDeg(float d) noexcept
    {
        d = std::fmod(d, VideoScrollLimits::kRotationMax);
        return d < 0.f ? d + VideoScrollLimits::kRotationMax : d;
    }

    //==========================================================================
    static constexpr float kHitR      = 9.0f;
    static constexpr float kLineHitR  = 6.0f;
    static constexpr float kLeverLen  = 22.0f;

    juce::AudioProcessorValueTreeState& apvts_;
    juce::Colour accent_;
    MidiMappingEngine*        midiMap_ { nullptr };
    VideoScrollPreviewSource* source_  { nullptr };
    int slot_ { -1 };

    Bound rot_, zoom_, cx_, cy_, line_;

    juce::Rectangle<float> frame_, plot_;
    Handle dragging_ { Handle::None };
    Handle hovered_  { Handle::None };

    struct DragStart
    {
        juce::Point<float> centre, mouse;
        float rot = 0, zoom = 1, cx = 0, cy = 0, line = 1;
    } start_;

    uint32_t         lastFrame_ { 0 };
    juce::Point<int> lastView_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollViewportEditor)
};
