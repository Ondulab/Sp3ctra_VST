/**
 * @file VideoScrollViewportEditor.h
 * @brief The VIEWPORT pad of the VIDEO SCROLL grid — the output window drawn
 *        as a live thumbnail, with the generation frame laid on it.
 *
 * TWO POSITIONS, and nothing else (2026-09-04). The pad used to carry every
 * setting at once: corner rings for Zoom, a lever for Rotation, and on the
 * axis a thickness bar, a compression ruler, an aging ribbon, blur wedges,
 * flow chevrons, plus a column of value cards. Together they buried the one
 * thing only this view can show — where the generation sits IN the picture —
 * under a dozen marks nobody could tell apart. Each of those settings now
 * owns a cell in the grid below, which draws it properly and to scale.
 *
 * What is drawn (window space, the output's real aspect):
 *   • the window rect, filled with the latest solo render of THIS output (or
 *     the frame colour through the Invert / Color law before the first frame);
 *   • the sweep band it covers, bright, the rest veiled;
 *   • the generation window — Zoom × the view, turned by Rotation, centred by
 *     Center X / Y — as a plain outline: its POSITION, not its size or angle;
 *   • the birth line inside it (Line Pos).
 *
 * Edit glow: every parameter is bound (ParameterAttachment), so a change from
 * ANY source — a handle here, a grid cell, a MIDI controller, automation —
 * lights its element for kGlowMs with a name + value readout, and
 * onActiveParamChanged tells the page which setting is live.
 *
 * Gestures (VideoScrollRenderCore::drawWarp / birthLine01 are the model — the
 * pad reproduces their geometry exactly, so what you grab is what the output
 * does). The window has NO centre node: you push the frame itself rather than
 * aim at a dot sitting in the middle of the picture.
 *   • drag the frame             Center X / Center Y (±1 = centre on the edge)
 *   • drag the birth line        Line Pos — seized anywhere along its length,
 *                                then it follows the pointer along the scroll
 *                                axis, clamped to the window like the renderer
 *   • pinch (trackpad) / ⌘ or Ctrl + wheel   Zoom — a plain wheel keeps
 *                                scrolling the page (editor rule)
 *   • double-click               reset what is under the pointer
 *   • right-click                MIDI Learn — the parameter(s) under the
 *                                pointer, or all of them off the handles
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
#include <array>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../ui/Sp3ctraHandles.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/ModuleParamManifest.h"   // vsParam()
#include "VideoScrollMode.h"             // VideoScrollLimits
#include "VideoScrollPreviewSource.h"
#include "../ui/Sp3ctraGestures.h"
#include "VideoScrollRenderCore.h"       // applyDisplayColour (frame colour law)

class VideoScrollViewportEditor : public juce::Component,
                                  public juce::SettableTooltipClient
{
    struct Geo;     // screen geometry — defined below (private), used by member signatures above it
    struct Bound;   // a bound parameter — idem
public:
    // The frame alone (boxes live in the page). 240 → 420 → 640 px
    // (2026-08-30, "bigger, and let me grab the handles that are off frame"):
    // the page scrolls in zone 3 and the ALL view stacks one per output.
    static constexpr int kGraphH     = 640;
    static constexpr int kPreferredH = kGraphH;

    /** Every setting the pad visualises — one element / card each, lit in
     *  the control colour while it is being edited from any source. Paper =
     *  the display law group (Invert / Color / Background). */
    enum class Param { Rotation, Zoom, CenterX, CenterY, LinePos, Speed, Thickness,
                       Compress, Fade, Blur, Gamma, Paper, Count };

    /** The setting edited most recently (still glowing), else Count. */
    Param activeParam() const noexcept { return lastActive_; }

    /** Fired when activeParam() changes — the page tints that box's label. */
    std::function<void()> onActiveParamChanged;

    VideoScrollViewportEditor(juce::AudioProcessorValueTreeState& apvtsIn,
                              juce::Colour accentColour)
        : apvts_(apvtsIn), accent_(accentColour)
    {
        setRepaintsOnMouseActivity(true);
        setTooltip(juce::String::fromUTF8(
            "VIEWPORT \xE2\x80\x94 where the generation sits in the output\n"
            "Drag the frame: Center X / Y \xC2\xB7 drag the line: Line Pos\n"
            "pinch or \xE2\x8C\x98+wheel: Zoom \xC2\xB7 double-click: reset \xC2\xB7 hold: type"));
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
        for (auto* b : allBounds()) b->reset();
        lastActive_ = Param::Count;
        glowing_    = false;
        if (slot_ < 0) { repaint(); return; }
        bind(rot_,   Param::Rotation,  vsParam(slot_, "rotation"));
        bind(zoom_,  Param::Zoom,      vsParam(slot_, "zoom"));
        bind(cx_,    Param::CenterX,   vsParam(slot_, "centerX"));
        bind(cy_,    Param::CenterY,   vsParam(slot_, "centerY"));
        bind(line_,  Param::LinePos,   vsParam(slot_, "linePos"));
        bind(speed_, Param::Speed,     vsParam(slot_, "speed"));
        bind(thick_, Param::Thickness, vsParam(slot_, "thickness"));
        bind(comp_,  Param::Compress,  vsParam(slot_, "pack"));
        bind(fade_,  Param::Fade,      vsParam(slot_, "fade"));
        bind(blur_,  Param::Blur,      vsParam(slot_, "blur"));
        bind(gamma_, Param::Gamma,     vsParam(slot_, "gamma"));
        bind(fadeMidX_,    Param::Fade, vsParam(slot_, "fadeMidX"));
        bind(fadeMidY_,    Param::Fade, vsParam(slot_, "fadeMidY"));
        bind(fadeMidFree_, Param::Fade, vsParam(slot_, "fadeMidFree"));
        static const char* const paperIds[kPaperN] = { "invertMode", "colorMode", "bgR", "bgG", "bgB" };
        for (int i = 0; i < kPaperN; ++i)
            bind(paper_[i], Param::Paper, vsParam(slot_, paperIds[i]));
        repaint();
    }

    int slot() const noexcept { return slot_; }

    /** Editor timer (20 Hz): keep this output's solo render alive and repaint
     *  when the frame or the view changed. Cheap when hidden (the ALL view
     *  keeps the single page alive) — a hidden pad requests nothing, so the
     *  mixer stops soloing its output. */
    void previewTick()
    {
        if (slot_ < 0 || ! isShowing()) return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        nowMs_ = now;

        // Edit glow: keep repainting while an element is lit; when the last
        // one goes out, the active setting is cleared (page label follows).
        bool need = false;
        if (glowing_)
        {
            need = true;
            if (maxHeat() <= 0.0f)
            {
                glowing_    = false;
                lastActive_ = Param::Count;
                if (onActiveParamChanged) onActiveParamChanged();
            }
        }
        if (source_ != nullptr)
        {
            source_->requestOutputPreview(slot_);
            const uint32_t fc = source_->frameCounter();
            const auto vs = source_->viewSize();
            if (fc != lastFrame_ || vs != lastView_) need = true;
        }
        if (! need) return;
        // Throttle: the thumbnail is a multi-megapixel blit — every other
        // editor tick (10 Hz) is plenty for a waterfall and halves the
        // message-thread cost of an ALL view full of pads. A frame skipped
        // here is caught on the next tick (lastFrame_ only moves on repaint).
        if (now - lastRepaintMs_ < kMinRepaintMs) return;
        lastRepaintMs_ = now;
        if (source_ != nullptr) { lastFrame_ = source_->frameCounter(); lastView_ = source_->viewSize(); }
        repaint();
    }

    //==========================================================================
    void resized() override
    {
        frame_ = getLocalBounds().toFloat();
        plot_  = ModuleChrome::plotOf(frame_);
    }

    void paint(juce::Graphics& g) override
    {
        nowMs_ = juce::Time::getMillisecondCounterHiRes();
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
        // The image is blitted ONCE (it is multi-megapixel); the veil is a
        // single even-odd fill of "window minus band" on top — not a second
        // blit clipped to the band (that doubled the pad's cost).
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(geo.win.getSmallestIntegerContainer());
            g.setColour(paperColour());
            g.fillRect(geo.win);
            const juce::Image img = source_ != nullptr ? source_->outputFrame(slot_) : juce::Image();
            if (img.isValid())
            {
                g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
                g.drawImage(img, geo.win, juce::RectanglePlacement::stretchToFit);
            }
            juce::Path veil;
            veil.addRectangle(geo.win);
            veil.addPath(bandPath(geo));
            veil.setUsingNonZeroWinding(false);   // rect XOR band = outside the band
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillPath(veil);
        }

        // ── Display layer (module colour): window edge, band edges, axis ────
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(plot_.getSmallestIntegerContainer());

            // Window edge — lit while the display law (paper) is edited.
            { juce::Path w; w.addRectangle(geo.win); strokeMark(g, w, Param::Paper, 0.55f, 1.0f); }

            // ── Controls (lime): the generation window, and the birth line ──
            // TWO positions, and nothing else (2026-09-04). Rotation, zoom,
            // speed, thickness, compression, attenuation and blur all have a
            // cell of their own in the grid below, which draws each of them
            // far better than a mark crowded onto this axis could. What the
            // pad is FOR is the pair you can only judge against the picture:
            // where the generation window sits, and where the line is born.
            const auto sBody = stateOf(Handle::Body);
            const auto sLine = stateOf(Handle::Birth);

            juce::Path outline = vignettePath(geo);
            if (Sp3ctraHandles::isHot(sBody))
            {
                g.setColour(Sp3ctraHandles::colour().withAlpha(0.08f));
                g.fillPath(outline);
            }
            g.setColour(Sp3ctraHandles::ringColour(sBody)
                            .withMultipliedAlpha(Sp3ctraHandles::isHot(sBody) ? 1.0f : 0.85f));
            g.strokePath(outline, juce::PathStrokeType(Sp3ctraHandles::isHot(sBody) ? 2.0f : 1.2f));

            // No centre node: the window IS its own handle — you push the
            // frame, you do not aim at a dot in the middle of the picture.
            Sp3ctraHandles::drawGrabLine(g, { geo.birth - geo.u * geo.hx, geo.birth + geo.u * geo.hx }, sLine);

            // Readout: whichever of the two is under the hand or being dragged.
            const Handle named = dragging_ != Handle::None ? dragging_ : hovered_;
            if (named != Handle::None)
                Sp3ctraHandles::drawReadout(g, handleReadout(named), anchorOf(named, geo), plot_);
        }
    }

    //── Edit glow ─────────────────────────────────────────────────────────────
    /** 1 → 0 over Sp3ctraTheme::kCtlGlowMs after a parameter's last change
     *  (0 = idle) — the shared heat, read at the pad's own clock. */
    float heat(const Bound& b) const noexcept { return b.heat(nowMs_); }

    float heatOf(Param p) const noexcept
    {
        switch (p)
        {
            case Param::Rotation:  return heat(rot_);
            case Param::Zoom:      return heat(zoom_);
            case Param::CenterX:   return heat(cx_);
            case Param::CenterY:   return heat(cy_);
            case Param::LinePos:   return heat(line_);
            case Param::Speed:     return heat(speed_);
            case Param::Thickness: return heat(thick_);
            case Param::Compress:  return heat(comp_);
            case Param::Fade:      return heat(fade_);
            case Param::Blur:      return heat(blur_);
            case Param::Gamma:     return heat(gamma_);
            case Param::Paper:
            {
                float h = 0.0f;
                for (const auto& b : paper_) h = juce::jmax(h, heat(b));
                return h;
            }
            case Param::Count: break;
        }
        return 0.0f;
    }

    bool lit(Param p) const noexcept { return heatOf(p) > 0.0f; }

    float maxHeat() const noexcept
    {
        float h = 0.0f;
        for (const auto* b : allBounds()) h = juce::jmax(h, heat(*b));
        return h;
    }

    /** Display colour of an element: module colour, pulled toward the control
     *  colour (and brightened) by its parameter's edit heat. */
    juce::Colour ink(Param p, float alpha) const
    {
        const float h = heatOf(p);
        return accent_.interpolatedWith(Sp3ctraHandles::colour(), h)
                      .withAlpha(juce::jmin(1.0f, alpha + 0.35f * h));
    }

    /** Stroke a display element, with a control-colour halo while edited. */
    void strokeMark(juce::Graphics& g, const juce::Path& p, Param id, float alpha, float w) const
    {
        const float h = heatOf(id);
        if (h > 0.0f)
        {
            g.setColour(Sp3ctraHandles::colour().withAlpha(0.28f * h));
            g.strokePath(p, juce::PathStrokeType(w + 4.0f));
        }
        g.setColour(ink(id, alpha));
        g.strokePath(p, juce::PathStrokeType(w));
    }

    void noteEdit(Bound& b)
    {
        // The stamp itself lives in the binding; the pad only tracks WHICH
        // setting is lit (its readout, its card, the page's box label).
        nowMs_   = juce::Time::getMillisecondCounterHiRes();
        glowing_ = true;
        if (lastActive_ != b.id)
        {
            lastActive_ = b.id;
            if (onActiveParamChanged) onActiveParamChanged();
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
        if (h != Handle::None)
            setMouseCursor(Sp3ctraCursors::closedHand());   // now you hold it
        start_    = { geo.C, e.position, rot_.value, zoom_.value, cx_.value, cy_.value, line_.value };
        switch (h)
        {
            case Handle::Body:   cx_.begin(); cy_.begin(); break;
            case Handle::Birth:  line_.begin();            break;
            case Handle::None:   break;
        }
        if (h != Handle::None)
            hold_.arm(e, [this] { holdToType(); });   // long press = type
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hold_.fired()) return;            // the entry bubble owns the rest
        hold_.moved(e);
        if (dragging_ == Handle::None || e.mods.isPopupMenu()) return;
        const Geo geo = computeGeometry();
        if (! geo.valid) return;
        const auto p = e.position;

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
        hold_.release();
        switch (dragging_)
        {
            case Handle::Body:   cx_.end(); cy_.end(); break;
            case Handle::Birth:  line_.end();          break;
            case Handle::None:   break;
        }
        dragging_ = Handle::None;
        hovered_  = handleAt(e.position);
        setMouseCursor(cursorFor(hovered_));                // and it opens again
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (slot_ < 0 || e.mods.isPopupMenu()) return;
        switch (handleAt(e.position))
        {
            case Handle::Body:   cx_.toDefault(); cy_.toDefault(); break;
            case Handle::Birth:  line_.toDefault();               break;
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
    // 2026-09-04 — the pad answers for TWO positions and nothing else: the
    // generation window (Body) and the birth line. Rotation, zoom and the
    // flow settings each own a cell in the grid, which draws them properly;
    // crowding them back onto this axis is what made the view unreadable.
    enum class Handle { None, Body, Birth };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    // Double-click resets what is under the pointer (mouseDoubleClick above);
    // the long press types it.
    void holdToType()
    {
        const Handle h = dragging_;
        switch (h)
        {
            case Handle::Body:   cx_.end(); cy_.end(); break;
            case Handle::Birth:  line_.end();          break;
            case Handle::None:   break;
        }
        dragging_ = Handle::None;
        setMouseCursor(cursorFor(hovered_));
        repaint();
        Sp3ctraGestures::BoundList l;
        if (h == Handle::Body)  l = { { "Center X", &cx_ }, { "Center Y", &cy_ } };
        if (h == Handle::Birth) l = { { "Line", &line_ } };
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), l);
    }
    Sp3ctraGestures::Hold hold_;

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

        // The window does NOT fill the plot: kFitMargin leaves a border of
        // ~24 % of the window on every side, because the vignette's corners,
        // its lever and the sweep band live OUTSIDE the window as soon as it
        // is rotated (at 45°, zoom 1, a corner sits 0.207·side beyond the
        // edge) — without that border they fell outside plot_, which both
        // clips the drawing and makes handleAt() refuse them. Enlarging the
        // pad alone never helped: the window is fitted to the plot, so
        // everything scaled together and the handles stayed just as far out.
        g.k   = kFitMargin * juce::jmin(plot_.getWidth() / g.W, plot_.getHeight() / g.H);
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

        // Two positions, one priority: the birth line is seized along its whole
        // length, then the window by its body. Nothing else answers.
        juce::Point<float> onLine;
        if (juce::Line<float>(g.birth - g.u * g.hx, g.birth + g.u * g.hx)
                .getDistanceFromPoint(p, onLine) <= kLineHitR)
            return Handle::Birth;

        const auto  rel = p - g.C;
        const float a   = rel.x * g.u.x + rel.y * g.u.y;
        const float b   = rel.x * g.v.x + rel.y * g.v.y;
        if (std::abs(a) <= g.hx && std::abs(b) <= g.hy) return Handle::Body;
        return Handle::None;
    }

    /** Drag > hover > edit heat (its parameter is being changed from a box,
     *  a MIDI CC or automation: the handle fades back down over
     *  Sp3ctraTheme::kCtlGlowMs like every other control) > idle. */
    Sp3ctraHandles::Look stateOf(Handle h) const noexcept
    {
        const float editing = juce::jmax(heatOf(paramOf(h)),
                                         h == Handle::Body ? heatOf(Param::CenterY) : 0.0f);
        return Sp3ctraHandles::stateOf(dragging_ == h,
                                       dragging_ == Handle::None && hovered_ == h,
                                       false, editing);
    }

    static Param paramOf(Handle h) noexcept
    {
        switch (h)
        {
            case Handle::Body:   return Param::CenterX;
            case Handle::Birth:  return Param::LinePos;
            case Handle::None:   break;
        }
        return Param::Count;
    }

    static juce::MouseCursor cursorFor(Handle h)
    {
        switch (h)
        {
            // At rest the hand is OPEN over both of them (the charter's
            // grammar, ui/Sp3ctraControls.h); it closes on the press below.
            case Handle::Body:
            case Handle::Birth:  return Sp3ctraCursors::openHand();
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
        // "OFF" = the output is disabled in the mix: the mixer no longer
        // renders it (its thumbnail is the blank paper) — see renderFrame.
        return juce::String(wrapDeg(rot_.value), 1) + deg() + dot()
             + juce::String(clampZoom(zoom_.value), 2) + times() + dot()
             + "x " + juce::String(cx_.value, 2) + "  y " + juce::String(cy_.value, 2) + dot()
             + "line " + juce::String(line_.value, 2)
             + (readRaw("enabled", 1.f) < 0.5f ? dot() + "OFF" : juce::String());
    }

    /** Name + value of a handle (hover and drag readout). */
    juce::String handleReadout(Handle h) const
    {
        switch (h)
        {
            case Handle::Body:   return "Center  x " + juce::String(cx_.value, 2) + "  y " + juce::String(cy_.value, 2);
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
            case Handle::Birth:  return g.birth;
            case Handle::None:   break;
        }
        return g.C;
    }

    static const char* invertName(int mode) noexcept
    {
        return mode == 1 ? "Negative" : mode == 2 ? "Luminance" : "Off";
    }

    juce::String paperHex() const
    {
        return "#" + paperColour().toDisplayString(false).toUpperCase();
    }

    /** Value column of a card (short form). */
    juce::String cardValue(Param p) const
    {
        switch (p)
        {
            case Param::Rotation:  return juce::String(wrapDeg(rot_.value), 1) + deg();
            case Param::Zoom:      return juce::String(clampZoom(zoom_.value), 2) + times();
            case Param::CenterX:
            case Param::CenterY:   return juce::String(cx_.value, 2) + ", " + juce::String(cy_.value, 2);
            case Param::LinePos:   return juce::String(line_.value, 2);
            case Param::Speed:     return juce::String(speed_.value, 2);
            case Param::Thickness: return juce::String(thick_.value, 2);
            case Param::Compress:  return packText();
            case Param::Fade:      return juce::String(fade_.value, 2);
            case Param::Blur:      return juce::String(blur_.value, 2);
            case Param::Gamma:     return juce::String(gamma_.value, 2);
            case Param::Paper:     return paperHex();
            case Param::Count: break;
        }
        return {};
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
            case Handle::Birth:  single = "linePos";   break;
            // Off the two handles: every setting of the output, one sub-menu
            // each — the pad stays the place to learn them in one gesture.
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
            MidiLearnPopup::addItems(sub, *midiMap_, ids[i], MidiLearnPopup::subMenuBase(i));
            menu.addSubMenu(list[i].label, sub);
        }
        MidiMappingEngine& engine = *midiMap_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                           [&engine, ids](int choice)
                           {
                               for (int i = 0; i < ids.size(); ++i)
                                   if (MidiLearnPopup::handle(engine, ids[i], choice,
                                                              MidiLearnPopup::subMenuBase(i)))
                                       return;
                           });
    }

    //── Parameter binding ─────────────────────────────────────────────────────
    /** The shared binding (ui/Sp3ctraControls.h: attachment, mirrored value
     *  and the edit heat stamped by ANY source — handle drag, box, MIDI CC,
     *  automation, preset) plus which pad element it lights. */
    struct Bound : Sp3ctraControls::Bound
    {
        Param id { Param::Count };
        void set(float v)        { setComplete(v); }
    };

    void bind(Bound& b, Param id, const juce::String& paramId)
    {
        b.id = id;
        // Changes from ANY source land here on the message thread; the
        // initial update never counts as an edit (Sp3ctraControls::Bound).
        b.bind(apvts_, paramId, [this, &b](float) { noteEdit(b); repaint(); });
    }

    std::array<Bound*, 19> allBounds() noexcept
    {
        return { &rot_, &zoom_, &cx_, &cy_, &line_, &speed_, &thick_, &comp_, &fade_, &blur_, &gamma_,
                 &fadeMidX_, &fadeMidY_, &fadeMidFree_,
                 &paper_[0], &paper_[1], &paper_[2], &paper_[3], &paper_[4] };
    }
    std::array<const Bound*, 19> allBounds() const noexcept
    {
        return { &rot_, &zoom_, &cx_, &cy_, &line_, &speed_, &thick_, &comp_, &fade_, &blur_, &gamma_,
                 &fadeMidX_, &fadeMidY_, &fadeMidFree_,
                 &paper_[0], &paper_[1], &paper_[2], &paper_[3], &paper_[4] };
    }

    /** The bipolar packing, spoken the way the grid's cell speaks it. */
    juce::String packText() const
    {
        const float v = juce::jlimit(-1.0f, 1.0f, comp_.value);
        if (std::abs(v) < 0.02f) return "1 : 1";
        const float k = std::pow(2.0f, std::abs(v) * 2.0f);
        return (v > 0.0f ? "x" : "/") + juce::String(k, 2);
    }

    /** The attenuation level at normalised distance `a` — the SAME law the
     *  renderer dims with (VideoScrollMode.h). */
    float attenAt(float a) const noexcept
    {
        return videoScrollAttenLevel(a, fade_.value, fadeMidX_.value, fadeMidY_.value,
                                     fadeMidFree_.value > 0.5f);
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
    static constexpr float kFitMargin = 0.68f;   // window / plot — the handle border
    static constexpr float kHitR      = 9.0f;
    static constexpr float kLineHitR  = 6.0f;
    /** How much a ⇧-drag slows the hand down — the grid uses the same. */
    static constexpr float kFineFactor = 0.12f;

    juce::AudioProcessorValueTreeState& apvts_;
    juce::Colour accent_;
    MidiMappingEngine*        midiMap_ { nullptr };
    VideoScrollPreviewSource* source_  { nullptr };
    int slot_ { -1 };

    static constexpr int kPaperN = 5;   // invertMode, colorMode, bgR, bgG, bgB
    Bound rot_, zoom_, cx_, cy_, line_, speed_, thick_, comp_, fade_, blur_, gamma_;
    // The attenuation law's SHAPE (the grid cell's free middle handle). The
    // pad reproduces the renderer's geometry exactly — including this curve —
    // so the aging ribbon cannot drift from what the picture does.
    Bound fadeMidX_, fadeMidY_, fadeMidFree_;
    Bound paper_[kPaperN];

    // Edit glow
    Param  lastActive_ { Param::Count };
    bool   glowing_    { false };
    double nowMs_      { 0.0 };
    static constexpr double kGlowMs = Sp3ctraTheme::kCtlGlowMs;

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
    double           lastRepaintMs_ { 0.0 };
    static constexpr double kMinRepaintMs = 75.0;   // → every other 20 Hz tick

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollViewportEditor)
};
