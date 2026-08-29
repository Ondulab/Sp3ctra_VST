/**
 * @file ShapeEqComponent.h
 * @brief THE EQ editor — typed-handle curve canvas, shared by every EQ
 *        surface (EQUALIZER / CENTROID / LEVELS pages, sampler slot EQ,
 *        SCORE / VOICE / MIDI SCORE generator tabs).
 *
 * An EQ is a stack of up to SHAPE_EQ_MAX_HANDLES typed handles (Bell /
 * Low-pass / High-pass / DJ Filter / Tilt — shape_eq.h), each with three
 * musical settings: Freq (position on the log-f axis), Gain (height) and
 * Width (bandwidth / resonance). The drawn curve is sampled from the SAME
 * evaluator the engines' LUTs use (shape_eq_db), so what is drawn is what
 * is applied.
 *
 * Two binding modes:
 *  - APVTS mode (chain modules): construct with the APVTS, then
 *    setInstance() rebinds the canvas to one instance's Sh{h}* param bank
 *    (ParameterAttachment gestures — host-automatable). The three MIDI
 *    targets are the VIRTUAL "selected handle" ids (EqHandleMidiTargets);
 *    the selection is pushed to the processor through the selection sink.
 *  - String mode (sampler + generators): the handles live in the component;
 *    the host persists them through encodeState()/decodeState()
 *    (ShapeEqCodec) and listens on onChange.
 *
 * Interactions:
 *  - type chips           BELL · LP · HP · DJ · TILT, always visible in the
 *                         header — click to retype the selected handle, or to
 *                         create one when the slot is empty
 *  - LEVEL fader          left margin — the whole-curve gain (its own Level
 *                         param; right-click = MIDI Learn, double-click = 0)
 *  - click empty canvas   add a Bell at the click point (first free slot)
 *  - click a handle       select it (the 3 mapped CCs follow the selection)
 *  - drag                 Freq (x) for every type; ONLY the Bell adds a
 *                         RELATIVE vertical = Gain (the handle stays ON
 *                         the curve). DJ reso = its vertical arrow, Tilt
 *                         amount = its lever, LP/HP slope = the chevron —
 *                         never hidden on the handle itself
 *  - chevrons             linked to the handle; the spread maps the WHOLE
 *                         setting range onto kGripMin/MaxPx (drag follows
 *                         the cursor): Bell = bandwidth, LP/HP/DJ = SLOPE
 *                         (3–192 dB/oct); DJ adds a VERTICAL arrow above
 *                         the handle = the resonance
 *  - Tilt lever           ONE arrow on the pivot: raise / lower it = the
 *                         tilt (its swing maps the whole ±range onto ±60°,
 *                         not the on-screen slope — the plot is too flat
 *                         for that), pull it in / out = the rounding
 *                         (short = hard S, long = straight)
 *  - mouse wheel          same second setting, on the hovered handle
 *  - shift + drag         Width (Bell/Tilt — shortcut for chevrons/wheel)
 *  - double-click         DELETE the handle
 *  - right-click          MIDI Learn + Type + Delete menu; the learn labels
 *                         follow the type (TILT/RESO/SLOPE… — same CC ids)
 *
 * No controls inside the plot (editor guidelines) — everything hangs off the
 * right-click menu. Chrome (frame / caption / readout / box row) is the
 * shared ModuleEditorChrome; everything grabbable (handles, chevrons, fader
 * thumb, type chips) is painted lime by Sp3ctraHandles — the module colour
 * stays on the curve, grid, labels and captions.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <functional>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../midi/EqHandleMidiTargets.h"
#include "../image/ShapeEqCodec.h"
#include "../processing/shape_eq.h"
#include "../processing/lux_eq.h"   // default live glow reads the LuxEq pool
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraHandles.h"
#include "ModuleEditorChrome.h"

class ShapeEqComponent : public juce::Component,
                         private juce::Timer
{
public:
    static constexpr int   kGraphH     = 150;   // the curve frame
    static constexpr int   kPreferredH = kGraphH + ModuleChrome::kBelowFrameH;   // + box row
    static constexpr float kGainRange  = SHAPE_EQ_DB_MAX;   // ± dB (plot grid)

    /** String mode — the curve lives here, persisted by the host through
     *  encodeState()/decodeState(). */
    explicit ShapeEqComponent(juce::Colour accentColour)
        : accent(accentColour)
    {
        for (auto& h : cur_) shape_eq_default(&h);
        // Value boxes below the frame (LEVELS-page idiom) — bound to the
        // SELECTED handle, labels/formats follow its type.
        for (auto* b : { &freqBox_, &gainBox_, &widthBox_ })
            addAndMakeVisible(*b);   // lime bars (control colour) — no re-tint
        rebindBoxes();
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    /** APVTS mode — unbound until the owning tab calls setInstance(). */
    ShapeEqComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                     juce::Colour accentColour)
        : ShapeEqComponent(accentColour)
    {
        apvts_ = &apvtsIn;
    }

    ~ShapeEqComponent() override { stopTimer(); }

    //==========================================================================
    // Host hooks
    //==========================================================================

    /** String mode: called whenever the curve changes (drag / menu / reset). */
    std::function<void()> onChange;

    /** Right-click MIDI-Learn engine (APVTS pages + sampler; null = no learn
     *  section in the menu — the generators leave it off). */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** MIDI target id per `which` (0 = Freq, 1 = Gain, 2 = Width). APVTS mode
     *  fills it from EqHandleMidiTargets in setInstance(); the sampler slot
     *  editor points it at its smp:…:eqfreq/eqgain/eqwidth ids. */
    std::function<juce::String(int which)> midiTargetIdFn;

    /** Selection plumbing — sink pushes a click-selection out (processor /
     *  LuxSampler atomic), provider seeds the selection on rebind so the UI
     *  agrees with whatever the CCs are currently steering. */
    std::function<void(int handle)> selectionSink;
    std::function<int()>            selectionProvider;

    /** Live-glow source: true while the bound engine instance is actually
     *  applying this curve. Defaults to the LuxEq pool — hosts embedding the
     *  editor for another engine's bank (CENTROID / LEVELS) repoint it. */
    std::function<bool(int slot)> liveProvider = [](int slot)
    {
        const LuxEqState& st = *lux_eq_instance(slot);
        return st.config.enabled != 0 && st.eq_active != 0;
    };

    //==========================================================================
    // APVTS mode binding
    //==========================================================================

    /** (Re)bind the canvas to one instance's Sh{h}* bank. `family` is the
     *  EqHandleMidiTargets family (0 = EQUALIZER, 1 = CENTROID, 2 = LEVELS),
     *  `paramIdFn` maps a suffix ("Sh0Freq"…) to the bank's param id. The
     *  selection sink/provider default to nothing — hosts wire them to
     *  processor.set/getEqSelectedHandle so the mapped CCs follow. */
    void setInstance(int family, int slot,
                     std::function<juce::String(const juce::String&)> paramIdFn)
    {
        jassert(apvts_ != nullptr);
        family_ = juce::jlimit(0, EqHandleMidiTargets::FamilyCount - 1, family);
        slot_   = juce::jlimit(0, EqHandleMidiTargets::kSlots - 1, slot);
        for (int w = 0; w < 3; ++w)
            midiIds_[w] = EqHandleMidiTargets::makeId(family_, slot_, w);
        midiTargetIdFn = [this](int w) { return midiIds_[juce::jlimit(0, 2, w)]; };

        paramIdFn_ = paramIdFn;
        bindLevel(paramIdFn("Level"));
        for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h)
        {
            const juce::String sh = "Sh" + juce::String(h);
            bindField(h, kFieldType,  paramIdFn(sh + "Type"));
            bindField(h, kFieldFreq,  paramIdFn(sh + "Freq"));
            bindField(h, kFieldGain,  paramIdFn(sh + "Gain"));
            bindField(h, kFieldWidth, paramIdFn(sh + "Width"));
        }
        selected_ = selectionProvider ? juce::jlimit(0, SHAPE_EQ_MAX_HANDLES - 1,
                                                     selectionProvider())
                                      : 0;
        rebindBoxes();
        repaint();
    }

    int slot() const noexcept { return slot_; }
    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    // Shared surface (both modes)
    //==========================================================================

    void setAccent(juce::Colour accentColour)
    {
        if (accentColour == accent) return;
        accent = accentColour;   // display only — the boxes stay lime
        repaint();
    }

    void setTitle(const juce::String& t)
    {
        if (t == title_) return;
        title_ = t;
        repaint();
    }

    /** Frequency span of the axis (labels + gainDbAtFreq). String mode only
     *  moves the labels — the handles are positional (freq01). */
    void setRange(double minHz, double maxHz)
    {
        if (minHz <= 0.0 || maxHz <= minHz) return;
        if (std::abs(minHz - minF_) < 1e-9 && std::abs(maxHz - maxF_) < 1e-9)
            return;
        minF_ = minHz; maxF_ = maxHz;
        repaint();
    }

    bool isFlat() const noexcept
    {
        return shape_eq_is_flat_level(cur_, SHAPE_EQ_MAX_HANDLES,
                                      levelDb_) != 0;
    }

    /** Whole-curve gain fader value (dB) — generators bake it into the
     *  rendered image exactly like the handles. */
    float getLevelDb() const noexcept { return levelDb_; }

    /** True while a handle is being dragged — hosts defer heavy reapplies. */
    bool isDragging() const noexcept { return dragging_ != -1; }

    /** Gain (dB) at an arbitrary frequency — the exact applied curve. */
    float gainDbAtFreq(double hz) const noexcept
    {
        if (hz <= minF_) hz = minF_;
        if (hz >= maxF_) hz = maxF_;
        const float x01 = (float) (std::log(hz / minF_) / std::log(maxF_ / minF_));
        return shape_eq_db_level(cur_, SHAPE_EQ_MAX_HANDLES, levelDb_,
                                 x01, spanOct());
    }

    /** Curve snapshot — lets hosts capture the handles for a background
     *  thread (MidiScoreGenRenderer). */
    void getHandles(ShapeEqHandle out[SHAPE_EQ_MAX_HANDLES]) const noexcept
    { for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h) out[h] = cur_[h]; }
    double getMinFreq() const noexcept { return minF_; }
    double getMaxFreq() const noexcept { return maxF_; }

    //==========================================================================
    // String mode persistence
    //==========================================================================

    juce::String encodeState() const
    { return ShapeEqCodec::encode(cur_, levelDb_, minF_, maxF_); }

    /** Restore a curve written by encodeState(). Anything else — including
     *  every legacy 9-band spline string — resets to FLAT (the migration
     *  policy) and returns false. */
    bool decodeState(const juce::String& s)
    {
        // decodeState IS the string-mode rebind moment — re-seed the selection
        // from the provider so the UI agrees with what the CCs are steering.
        if (selectionProvider)
            selected_ = juce::jlimit(0, SHAPE_EQ_MAX_HANDLES - 1,
                                     selectionProvider());
        ShapeEqHandle parsed[SHAPE_EQ_MAX_HANDLES];
        float lvl = 0.0f;
        double lo = minF_, hi = maxF_;
        if (ShapeEqCodec::decode(s, parsed, lvl, lo, hi))
        {
            for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h) cur_[h] = parsed[h];
            levelDb_ = lvl;
            minF_ = lo; maxF_ = hi;
            repaint();
            return true;
        }
        if (s.isNotEmpty())
            DBG("ShapeEq: legacy EQ payload ignored — curve reset to flat");
        for (auto& h : cur_) shape_eq_default(&h);
        levelDb_ = 0.0f;
        repaint();
        return false;
    }

    /** Reset every handle to Off (flat) + the level fader to 0 dB. */
    void reset()
    {
        for (int h = 0; h < SHAPE_EQ_MAX_HANDLES; ++h)
        {
            ShapeEqHandle v; shape_eq_default(&v);
            writeType(h, SHAPE_EQ_OFF);
            writeHandleComplete(h, v);
        }
        writeLevelComplete(0.0f);
        repaint();
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto bf = frameArea();
        ModuleChrome::drawFrame(g, bf, accent);

        const auto plot = plotArea();
        const bool live = liveProvider && liveProvider(slot_);

        // Horizontal gain grid: 0 dB centre (brighter) + ±12 / ±24.
        for (int db = -24; db <= 24; db += 12)
        {
            const float y = gainToY((float) db, plot);
            g.setColour(db == 0 ? juce::Colour(0x22ffffff) : juce::Colour(0x10ffffff));
            g.drawHorizontalLine((int) y, plot.getX(), plot.getRight());
            g.setColour(accent.withAlpha(0.35f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawText((db > 0 ? "+" : "") + juce::String(db),
                       (int) plot.getX() + 2, (int) y - 6, 26, 11,
                       juce::Justification::left, false);
        }

        // Vertical octave grid + Hz labels (informative — the curve is
        // positional over the configured span).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        const int nOct = juce::jmax(1, (int) std::floor(spanOct() + 0.001f));
        for (int k = 0; k <= nOct; ++k)
        {
            const float x = plot.getX()
                          + ((float) k / spanOct()) * plot.getWidth();
            if (x > plot.getRight() + 0.5f) break;
            g.setColour(juce::Colour(0x0cffffff));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            if (k % 2 == 0 || k == nOct)
            {
                const double f = minF_ * std::pow(2.0, (double) k);
                const juce::String lbl = (f >= 1000.0)
                    ? juce::String(f / 1000.0, f >= 10000.0 ? 0 : 1) + "k"
                    : juce::String((int) std::lround(f));
                g.setColour(accent.withAlpha(0.4f));
                g.drawText(lbl, (int) x - 16, (int) plot.getBottom() + 1, 32, 10,
                           juce::Justification::centred, false);
            }
        }

        // Curve + filled area to the 0 dB line — sampled from the SAME
        // evaluator the engine LUTs use (shape_eq_db). Roll-off below the
        // ±24 dB grid draws clipped at the plot bottom (the applied value
        // keeps falling to SHAPE_EQ_DB_MIN).
        juce::Path curve, fill;
        const float y0 = gainToY(0.0f, plot);
        const int steps = juce::jmax(48, (int) plot.getWidth() / 3);
        for (int s = 0; s <= steps; ++s)
        {
            const float u  = (float) s / (float) steps;
            const float x  = plot.getX() + u * plot.getWidth();
            const float db = juce::jmax(-kGainRange,
                                 shape_eq_db_level(cur_, SHAPE_EQ_MAX_HANDLES,
                                                   levelDb_, u, spanOct()));
            const float y  = gainToY(db, plot);
            if (s == 0) { curve.startNewSubPath(x, y); fill.startNewSubPath(x, y0); fill.lineTo(x, y); }
            else        { curve.lineTo(x, y); fill.lineTo(x, y); }
        }
        fill.lineTo(plot.getRight(), y0);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(live ? 0.16f : 0.10f));
        g.fillPath(fill);
        g.setColour(accent.withAlpha(live ? 0.95f : 0.55f));
        g.strokePath(curve, juce::PathStrokeType(1.6f));

        // Handles (active slots only) + type glyph — lime, four states
        // (Sp3ctraHandles: idle / SELECTED / hover / drag). All of them RIDE
        // THE CURVE (handleY); the selected Tilt shows its lever (see the
        // chevron block below).
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES; ++i)
        {
            if (cur_[i].type == SHAPE_EQ_OFF) continue;
            const float x = plot.getX() + cur_[i].freq01 * plot.getWidth();
            const float y = handleY(cur_[i], plot);
            const bool sel = (i == selected_);
            const auto st  = Sp3ctraHandles::stateOf(i == dragging_,
                                                     dragging_ == -1 && i == hovered_,
                                                     sel);
            Sp3ctraHandles::drawNode(g, { x, y }, st);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.setColour(sel ? Sp3ctraHandles::colour() : accent.withAlpha(0.6f));
            g.drawText(typeGlyph(cur_[i].type),
                       (int) x + 6, (int) y - 12, 22, 10,
                       juce::Justification::left, false);
        }

        // Width / slope / rounding chevrons — the selected handle's second
        // setting made grabbable: linked to the handle by a segment. The
        // DISPLAYED spread mirrors the setting inside a small cosmetic
        // window (kGripMin/MaxPx — the whole range stays reachable). A
        // chevron that would leave the plot or sit on the handle itself is
        // hidden (the handle keeps priority at the plot edges) — except
        // the DJ reso arrow, laid out to FIT the headroom (gripHidden).
        {
            const auto& h = cur_[selected_];
            int sides[3];
            const int nSides = (h.type == SHAPE_EQ_OFF) ? 0
                                                        : gripSideList(h, sides);
            if (nSides > 0)
            {
                const float hx    = plot.getX() + h.freq01 * plot.getWidth();
                const float hy    = handleY(h, plot);
                const float ghost = gripGhosted(h) ? 0.45f : 1.0f;
                for (int si = 0; si < nSides; ++si)
                {
                    const int  side = sides[si];
                    const auto gp   = gripPos(h, side, plot);
                    if (gripHidden(side, gp, { hx, hy }, plot))
                        continue;
                    const auto st = Sp3ctraHandles::stateOf(
                        dragGrip_ == side, dragGrip_ == 0 && hoverGrip_ == side);
                    // Direction handle → chevron: horizontal for the width /
                    // slope chevrons, along the tilt line for the Tilt lever.
                    const float ddx = gp.x - hx, ddy = gp.y - hy;
                    const float len = juce::jmax(1.0f,
                                          std::sqrt(ddx * ddx + ddy * ddy));
                    const float ux = ddx / len, uy = ddy / len;
                    // Linking segment handle → chevron (the setting's span),
                    // then the lime arrowhead — both ghosted in the DJ dead zone.
                    Sp3ctraHandles::drawLink(g, { hx + ux * 7.0f, hy + uy * 7.0f },
                                             { gp.x - ux * 4.0f, gp.y - uy * 4.0f },
                                             st, ghost);
                    Sp3ctraHandles::drawChevron(g, gp, { ux, uy }, st, ghost);
                }
            }
        }

        // LEVEL fader — whole-curve gain in the left margin: neutral rail +
        // 0 dB notch (display), lime amount bar from 0 dB and a lime thumb
        // (control — Sp3ctraHandles states).
        {
            const auto fs = faderStrip();
            const float fx = fs.getCentreX();
            const auto st  = Sp3ctraHandles::stateOf(dragFader_, hoverFader_);
            const float ty = gainToY(levelDb_, plot);
            g.setColour(juce::Colour(0x2effffff));
            g.drawLine(fx, fs.getY(), fx, fs.getBottom(), 2.0f);
            g.setColour(accent.withAlpha(0.5f));
            g.drawLine(fx - 3.0f, fs.getCentreY(), fx + 3.0f,
                       fs.getCentreY(), 1.0f);
            g.setColour(Sp3ctraHandles::colour()
                            .withAlpha(Sp3ctraHandles::isHot(st) ? 0.6f : 0.35f));
            g.drawLine(fx, fs.getCentreY(), fx, ty, 2.0f);
            Sp3ctraHandles::drawThumb(g, { fx - 5.0f, ty - 2.5f, 10.0f, 5.0f }, st);
        }

        // Type chips — always-visible header row: click to retype the
        // selected handle (or to spawn one when the slot is empty).
        {
            const int curType = cur_[selected_].type;
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            for (int i = 0; i < SHAPE_EQ_NUM_TYPES - 1; ++i)
            {
                const int type = SHAPE_EQ_BELL + i;
                Sp3ctraHandles::drawChip(g, chipRect(i).toFloat(), kChipNames[i],
                                         curType == type, hoverChip_ == type);
            }
        }

        // Caption + selected-handle readout / hint (shared chrome; the
        // timed hint is amber, the idle hint muted grey).
        ModuleChrome::drawCaption(g, bf, accent, title_);
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (now < hintUntilMs_)
            ModuleChrome::drawReadout(g, bf, juce::Colour(0xffe0a24a), hint_, 1.0f);
        else if (dragFader_ || hoverFader_)
            ModuleChrome::drawReadout(g, bf, accent,
                                      "LEVEL " + juce::String(levelDb_ >= 0 ? "+" : "")
                                          + juce::String(levelDb_, 1) + " dB",
                                      0.8f);
        else if (cur_[selected_].type != SHAPE_EQ_OFF)
            ModuleChrome::drawReadout(g, bf, accent, selectedReadout());
        else if (isFlat())
            ModuleChrome::drawReadout(g, bf, juce::Colour(0xff55606f),
                                      juce::String::fromUTF8(
                                          "click to add  \xc2\xb7  right-click: MIDI"),
                                      1.0f);

        // Mapped badge — any of the three targets mapped → amber dot.
        if (midiMap_ != nullptr && midiTargetIdFn)
            for (int w = 0; w < 3; ++w)
            {
                int t, c, n;
                const juce::String id = midiTargetIdFn(w);
                if (id.isNotEmpty() && midiMap_->getMappingFor(id, t, c, n))
                {
                    g.setColour(juce::Colour(0xffe0a24a).withAlpha(0.9f));
                    g.fillEllipse((float) getWidth() - 8.0f, 1.0f, 7.0f, 7.0f);
                    break;
                }
            }

        // Value-box labels (shared chrome) — worded for the selected type.
        {
            const int mt = cur_[selected_].type;
            ModuleChrome::drawBoxLabel(g, freqBox_,  accent, boxLabel(mt, 0));
            ModuleChrome::drawBoxLabel(g, gainBox_,  accent, boxLabel(mt, 1));
            ModuleChrome::drawBoxLabel(g, widthBox_, accent, boxLabel(mt, 2));
        }
    }

    void resized() override
    {
        // Box row (label strip + boxes) below the frame — clear of the LEVEL
        // fader margin, aligned with the plot.
        auto row = getLocalBounds().removeFromBottom(ModuleChrome::kBoxRowH);
        row.removeFromLeft(6 + (int) kFaderStripW);
        row.removeFromRight(6);
        ModuleChrome::layoutBoxRow(row, { &freqBox_, &gainBox_, &widthBox_ });
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging_ != -1 || dragFader_) return;
        const bool fad = faderHit(e.position);
        const int chip = fad ? 0 : chipAt(e.position);
        const int gr   = (fad || chip != 0) ? 0 : gripAt(e.position);
        const int h    = (fad || chip != 0 || gr != 0) ? -1
                                                       : handleAt(e.position);
        if (h != hovered_ || gr != hoverGrip_ || chip != hoverChip_
            || fad != hoverFader_)
        { hovered_ = h; hoverGrip_ = gr; hoverChip_ = chip;
          hoverFader_ = fad; repaint(); }
        setMouseCursor(fad       ? juce::MouseCursor::UpDownResizeCursor
                     : chip != 0 ? juce::MouseCursor::PointingHandCursor
                     : gr == kGripUp ? juce::MouseCursor::UpDownResizeCursor
                     : gr != 0   ? (cur_[selected_].type == SHAPE_EQ_TILT
                                        ? juce::MouseCursor::CrosshairCursor
                                        : juce::MouseCursor::LeftRightResizeCursor)
                     : h == -1   ? juce::MouseCursor::NormalCursor
                     : cur_[h].type == SHAPE_EQ_BELL
                                 ? juce::MouseCursor::UpDownResizeCursor
                                 : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging_ == -1 && ! dragFader_
            && (hovered_ != -1 || hoverGrip_ != 0 || hoverChip_ != 0
                || hoverFader_))
        { hovered_ = -1; hoverGrip_ = 0; hoverChip_ = 0; hoverFader_ = false;
          repaint(); }
    }

    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override
    {
        // Wheel over a handle (or a width grip) sweeps its Width / resonance.
        int h = handleAt(e.position);
        if (h < 0 && gripAt(e.position) != 0) h = selected_;
        if (h < 0) return;
        ShapeEqHandle v = cur_[h];
        if (v.type == SHAPE_EQ_OFF) return;
        v.width01 = juce::jlimit(0.0f, 1.0f, v.width01 + wheel.deltaY * 0.6f);
        writeHandleComplete(h, v);
        select(h);
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int hit = handleAt(e.position);

        if (e.mods.isPopupMenu())
        {
            // Fader: learn menu for the Level param (APVTS pages).
            if (faderHit(e.position))
            {
                if (midiMap_ != nullptr && apvtsMode() && levelId_.isNotEmpty())
                    MidiLearnPopup::show(*midiMap_, levelId_, this);
                return;
            }
            if (hit >= 0) select(hit);
            showMenu(hit);
            return;
        }

        // LEVEL fader — drag sets the whole-curve gain; double-click = 0 dB.
        if (faderHit(e.position))
        {
            if (e.getNumberOfClicks() >= 2)
            {
                writeLevelComplete(0.0f);
                repaint();
                return;
            }
            dragFader_ = true;
            beginLevelGesture();
            writeLevelAsGesture(yToGain(e.position.y, plotArea()));
            repaint();
            return;
        }

        // Type chips (header row) — retype / spawn, no menu needed.
        if (const int chip = chipAt(e.position))
        {
            applyChip(chip);
            return;
        }

        // Width / slope / rounding chevrons — absolute drag (the chevron
        // sticks to the cursor); the DJ reso arrow drags RELATIVE to the
        // grab (its base moves with the value — see applyDrag).
        if (const int gr = gripAt(e.position))
        {
            dragGrip_   = gr;
            dragging_   = selected_;
            hovered_    = selected_;
            dragStartW_ = cur_[selected_].width01;
            dragStartG_ = cur_[selected_].gain_db;
            dragStartX_ = e.position.x;
            dragStartY_ = e.position.y;
            dragStartSpan_ = resoArrowSpanPx(handleY(cur_[selected_], plotArea()),
                                             plotArea()) - kGripMinPx;
            beginGestures(selected_);
            applyDrag(e);
            return;
        }

        if (hit >= 0)
        {
            select(hit);
            if (e.getNumberOfClicks() >= 2)
            {
                // Double-click = DELETE the handle.
                writeType(hit, SHAPE_EQ_OFF);
                repaint();
                return;
            }
            startDrag(hit, e);
            return;
        }

        // Empty canvas — add a Bell in the first free slot at the click
        // point (header / label strips don't spawn handles).
        if (! plotArea().contains(e.position))
            return;
        int free = -1;
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES && free < 0; ++i)
            if (cur_[i].type == SHAPE_EQ_OFF) free = i;
        if (free < 0)
        {
            flashHint(juce::String(SHAPE_EQ_MAX_HANDLES) + " handles max");
            return;
        }
        ShapeEqHandle v; shape_eq_default(&v);
        v.type = SHAPE_EQ_BELL;
        writeType(free, SHAPE_EQ_BELL);
        writeHandleComplete(free, v);   // fresh defaults (slot may be recycled)
        select(free);
        startDrag(free, e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragFader_)
        {
            writeLevelAsGesture(yToGain(e.position.y, plotArea()));
            repaint();
            return;
        }
        applyDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragFader_)
        {
            endLevelGesture();
            dragFader_  = false;
            hoverFader_ = faderHit(e.position);
            repaint();
            return;
        }
        if (dragging_ >= 0) endGestures(dragging_);
        dragging_  = -1;
        dragGrip_  = 0;
        hoverGrip_ = gripAt(e.position);
        hovered_   = (hoverGrip_ != 0) ? -1 : handleAt(e.position);
        repaint();
    }

private:
    static constexpr int kFieldType = 0, kFieldFreq = 1, kFieldGain = 2,
                         kFieldWidth = 3;

    //==========================================================================
    // Geometry
    //==========================================================================
    static constexpr float kFaderStripW = 16.0f;   // LEVEL fader margin

    /** The curve frame — the component minus the value-box row below. */
    juce::Rectangle<float> frameArea() const
    {
        return getLocalBounds().toFloat()
               .withTrimmedBottom((float) ModuleChrome::kBelowFrameH);
    }

    juce::Rectangle<float> plotArea() const
    {
        return frameArea().reduced(6.0f)
                          .withTrimmedTop(12.0f).withTrimmedBottom(12.0f)
                          .withTrimmedLeft(kFaderStripW);
    }

    juce::Rectangle<float> faderStrip() const
    {
        const auto p = plotArea();
        return { p.getX() - kFaderStripW, p.getY(),
                 kFaderStripW - 4.0f, p.getHeight() };
    }

    bool faderHit(juce::Point<float> pt) const
    { return faderStrip().expanded(3.0f, 2.0f).contains(pt); }
    float spanOct() const noexcept
    { return (float) (std::log(maxF_ / minF_) / std::log(2.0)); }
    float gainToY(float db, juce::Rectangle<float> p) const
    { return p.getCentreY() - (db / kGainRange) * (p.getHeight() * 0.5f); }
    float yToGain(float y, juce::Rectangle<float> p) const
    {
        const float g = (p.getCentreY() - y) / (p.getHeight() * 0.5f) * kGainRange;
        return juce::jlimit(-kGainRange, kGainRange, g);
    }
    float xToFreq01(float x, juce::Rectangle<float> p) const
    { return juce::jlimit(0.0f, 1.0f, (x - p.getX()) / p.getWidth()); }

    /** Full curve value (dB) at x01 — level + every handle, exactly the
     *  painted curve. */
    float curveDbAt(float x01) const noexcept
    {
        return shape_eq_db_level(cur_, SHAPE_EQ_MAX_HANDLES, levelDb_,
                                 x01, spanOct());
    }

    /** Handle Y — ALWAYS on the drawn curve at its own frequency: a handle
     *  never floats off the curve, whatever the fader or the other handles
     *  do. Vertical drags are therefore RELATIVE (delta from grab). */
    float handleY(const ShapeEqHandle& h, juce::Rectangle<float> p) const
    {
        return gainToY(juce::jmax(-kGainRange, curveDbAt(h.freq01)), p);
    }

    static const char* typeGlyph(int t) noexcept
    {
        switch (t)
        {
            case SHAPE_EQ_BELL: return "B";
            case SHAPE_EQ_LP:   return "LP";
            case SHAPE_EQ_HP:   return "HP";
            case SHAPE_EQ_DJ:   return "DJ";
            case SHAPE_EQ_TILT: return "T";
            default:            return "";
        }
    }

    static const char* typeName(int t) noexcept
    {
        switch (t)
        {
            case SHAPE_EQ_BELL: return "Bell";
            case SHAPE_EQ_LP:   return "Low-pass";
            case SHAPE_EQ_HP:   return "High-pass";
            case SHAPE_EQ_DJ:   return "DJ Filter";
            case SHAPE_EQ_TILT: return "Tilt";
            default:            return "Off";
        }
    }

    juce::String selectedReadout() const
    {
        const ShapeEqHandle& h = cur_[selected_];
        const double hz = minF_ * std::pow(maxF_ / minF_, (double) h.freq01);
        juce::String s(typeName(h.type));
        s << "  " << ((hz >= 1000.0)
                        ? juce::String(hz / 1000.0, 1) + " kHz"
                        : juce::String((int) std::lround(hz)) + " Hz");
        if (h.type == SHAPE_EQ_BELL || h.type == SHAPE_EQ_TILT)
            s << "  " << (h.gain_db >= 0 ? "+" : "")
              << juce::String(h.gain_db, 1) << " dB";
        if (h.type == SHAPE_EQ_BELL)
            s << "  " << juce::String(shape_eq_bell_octaves(h.width01), 2) << " oct";
        else if (h.type == SHAPE_EQ_LP || h.type == SHAPE_EQ_HP)
            s << "  " << (int) std::lround(shape_eq_filter_slope(h.width01))
              << " dB/oct";
        else if (h.type == SHAPE_EQ_DJ)
            s << "  " << (int) std::lround(shape_eq_filter_slope(h.width01))
              << " dB/oct  reso "
              << (int) std::lround(shape_eq_dj_reso(h.gain_db) * 100.0f)
              << "%";
        else if (h.width01 > 0.001f)   // Tilt rounding (S curve)
            s << "  S " << (int) std::lround(h.width01 * 100.0f) << "%";
        return s;
    }

    /** Grip sides the selected handle exposes (±1 = horizontal chevrons,
     *  kGripUp = the vertical arrow): Bell = both (half-width), LP/HP = the
     *  attenuated side (the SLOPE), Tilt = +1 (the lever), DJ = the
     *  attenuated side of the ACTIVE filter (both, ghosted, in the dead
     *  zone) + the vertical RESO arrow. Returns the count. */
    static constexpr int kGripUp = 2;
    int gripSideList(const ShapeEqHandle& h, int out[3]) const
    {
        switch (h.type)
        {
            case SHAPE_EQ_BELL: out[0] = -1; out[1] = +1; return 2;
            case SHAPE_EQ_TILT: out[0] = +1;              return 1;   // lever
            case SHAPE_EQ_LP:   out[0] = +1;              return 1;
            case SHAPE_EQ_HP:   out[0] = -1;              return 1;
            case SHAPE_EQ_DJ:
            {
                const float k = 2.0f * h.freq01 - 1.0f;
                int n = 0;
                if      (k <= -SHAPE_EQ_DJ_DEADZONE) out[n++] = +1;
                else if (k >=  SHAPE_EQ_DJ_DEADZONE) out[n++] = -1;
                else { out[n++] = -1; out[n++] = +1; }   // dead zone: ghosted preview
                out[n++] = kGripUp;                      // resonance
                return n;
            }
            default:            return 0;
        }
    }

    /** True while the selected DJ knob sits in its flat dead zone — the
     *  chevrons then paint ghosted (preview). */
    bool gripGhosted(const ShapeEqHandle& h) const noexcept
    {
        return h.type == SHAPE_EQ_DJ
            && std::abs(2.0f * h.freq01 - 1.0f) < SHAPE_EQ_DJ_DEADZONE;
    }

    static constexpr float kGripMinPx = 14.0f, kGripMaxPx = 64.0f;

    /** DISPLAY offset (px) of a chevron from its handle — a DIRECT map of
     *  the whole setting range onto the [kGripMinPx..kGripMaxPx] window, so
     *  the spread visibly tracks every change (and the drag inverts the
     *  same map: the chevron follows the cursor). Bell: wide = spread;
     *  filters: steep slope = tight; Tilt: hard S = tight. */
    float gripOffsetPx(const ShapeEqHandle& h,
                       juce::Rectangle<float>) const
    {
        const float t = (h.type == SHAPE_EQ_BELL) ? h.width01
                                                  : 1.0f - h.width01;
        return kGripMinPx + t * (kGripMaxPx - kGripMinPx);
    }

    /** DJ resonance arrow — its SPAN (px above the handle at 100 %). The
     *  nominal kGripMaxPx is compressed into the headroom left between
     *  the handle and the arrow ceiling (kGripCeilPx above the plot top),
     *  so the arrow ALWAYS fits: at full reso its tip touches the
     *  ceiling. Short plots, a raised fader or the handle riding its own
     *  bump (knob fully closed = the handle sits AT the cutoff) used to
     *  push the tip off-plot, where a chevron is culled — and the arrow
     *  is the only reso affordance on the plot. Floored so a handle
     *  parked near the ceiling keeps a usable (if tight) travel. */
    static constexpr float kGripCeilPx = 4.0f, kResoMinTravelPx = 8.0f;
    float resoArrowSpanPx(float hy, juce::Rectangle<float> p) const
    {
        return juce::jlimit(kGripMinPx + kResoMinTravelPx, kGripMaxPx,
                            hy - (p.getY() - kGripCeilPx));
    }

    /** A chevron that would leave the plot or sit on its handle is hidden
     *  (the handle keeps priority at the plot edges). Never the DJ reso
     *  arrow: it is laid out to fit (resoArrowSpanPx) and never closer
     *  than kGripMinPx to its handle — hiding it would strand the reso. */
    bool gripHidden(int side, juce::Point<float> gp, juce::Point<float> hc,
                    juce::Rectangle<float> p) const
    {
        if (side == kGripUp) return false;
        return gp.x < p.getX() - 4.0f || gp.x > p.getRight() + 4.0f
            || gp.y < p.getY() - 4.0f || gp.getDistanceFrom(hc) < 11.0f;
    }

    /** Tilt lever direction (screen y down). The whole ±gain range maps
     *  onto ±kTiltLeverDeg — NOT the on-screen slope of the line (the plot
     *  is ~8× wider than tall, that angle would never exceed a few
     *  degrees). Raise = brighter. */
    static constexpr float kTiltLeverDeg = 60.0f;
    juce::Point<float> tiltDir(const ShapeEqHandle& h,
                               juce::Rectangle<float>) const
    {
        const float th = juce::degreesToRadians(
            juce::jlimit(-1.0f, 1.0f, h.gain_db / kGainRange) * kTiltLeverDeg);
        return { std::cos(th), -std::sin(th) };
    }

    juce::Point<float> gripPos(const ShapeEqHandle& h, int side,
                               juce::Rectangle<float> p) const
    {
        const float hx = p.getX() + h.freq01 * p.getWidth();
        const float hy = handleY(h, p);
        if (h.type == SHAPE_EQ_TILT)
        {
            // Tangent LEVER: along the tilt line (angle = tilt), its
            // length = rounding (long = straight, short = hard S).
            const auto  d = tiltDir(h, p);
            const float L = gripOffsetPx(h, p);
            return { hx + d.x * L, hy + d.y * L };
        }
        if (side == kGripUp)   // DJ resonance: straight up, height = reso
        {
            const float t = shape_eq_dj_reso(h.gain_db);
            return { hx, hy - (kGripMinPx
                               + t * (resoArrowSpanPx(hy, p) - kGripMinPx)) };
        }
        return { hx + (float) side * gripOffsetPx(h, p), hy };
    }

    /** ±1 when @p pt sits on a chevron of the SELECTED handle, else 0.
     *  Hidden chevrons (off-plot / overlapping the handle) never hit, so a
     *  handle parked on a plot edge stays grabbable. */
    int gripAt(juce::Point<float> pt) const
    {
        const ShapeEqHandle& h = cur_[selected_];
        int sides[3];
        const int n = (h.type == SHAPE_EQ_OFF) ? 0 : gripSideList(h, sides);
        const auto p = plotArea();
        const juce::Point<float> hc(p.getX() + h.freq01 * p.getWidth(),
                                    handleY(h, p));
        for (int i = 0; i < n; ++i)
        {
            const int  side = sides[i];
            const auto gp   = gripPos(h, side, p);
            if (gripHidden(side, gp, hc, p))
                continue;
            if (pt.getDistanceFrom(gp) <= 8.0f)
                return side;
        }
        return 0;
    }

    //==========================================================================
    // Type chips (header row, after the title)
    //==========================================================================
    static constexpr const char* kChipNames[SHAPE_EQ_NUM_TYPES - 1] =
        { "BELL", "LP", "HP", "DJ", "TILT" };

    juce::Rectangle<int> chipRect(int i) const
    {
        constexpr int w = 32, h = 12, gap = 3;
        const int x0 = (int) plotArea().getX() + 94;
        return { x0 + i * (w + gap), 1, w, h };
    }

    /** SHAPE_EQ_BELL..TILT when @p pt sits on a chip, else 0. */
    int chipAt(juce::Point<float> pt) const
    {
        for (int i = 0; i < SHAPE_EQ_NUM_TYPES - 1; ++i)
            if (chipRect(i).toFloat().contains(pt))
                return SHAPE_EQ_BELL + i;
        return 0;
    }

    /** Chip click: retype the selected handle, or spawn one when its slot is
     *  empty — the visible, no-menu way to reach DJ / Tilt. */
    void applyChip(int type)
    {
        if (cur_[selected_].type == SHAPE_EQ_OFF)
        {
            ShapeEqHandle v; shape_eq_default(&v);
            v.type = type;
            if (type == SHAPE_EQ_TILT)
                v.width01 = 0.0f;   // straight tilt (default 0.5 elsewhere:
                                    // 1 oct bell / 24 dB/oct slope)
            writeType(selected_, type);
            writeHandleComplete(selected_, v);
            if (selectionSink) selectionSink(selected_);
            repaint();
            return;
        }
        changeType(selected_, type);
    }

    int handleAt(juce::Point<float> pt) const
    {
        const auto p = plotArea();
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES; ++i)
        {
            if (cur_[i].type == SHAPE_EQ_OFF) continue;
            const float x = p.getX() + cur_[i].freq01 * p.getWidth();
            const float y = handleY(cur_[i], p);
            const float d = pt.getDistanceFrom({ x, y });
            if (d <= 11.0f && d < bd) { bd = d; best = i; }
        }
        return best;
    }

    //==========================================================================
    // Selection / hint
    //==========================================================================
    void select(int h)
    {
        h = juce::jlimit(0, SHAPE_EQ_MAX_HANDLES - 1, h);
        if (h == selected_) return;
        selected_ = h;
        if (selectionSink) selectionSink(h);
        repaint();
    }

    void flashHint(const juce::String& msg)
    {
        hint_ = msg;
        hintUntilMs_ = juce::Time::getMillisecondCounter() + 1500;
        repaint();
    }

    //==========================================================================
    // Drag
    //==========================================================================
    void startDrag(int h, const juce::MouseEvent& e)
    {
        dragging_   = h;
        hovered_    = h;
        dragShift_  = e.mods.isShiftDown();
        dragStartW_ = cur_[h].width01;
        dragStartG_ = cur_[h].gain_db;
        dragStartY_ = e.position.y;
        beginGestures(h);
        applyDrag(e);
    }

    void applyDrag(const juce::MouseEvent& e)
    {
        if (dragging_ < 0) return;
        const auto p = plotArea();
        ShapeEqHandle v = cur_[dragging_];

        if (dragGrip_ != 0)
        {
            // Chevron drag — ABSOLUTE inverse of the display map: the
            // chevron sticks to the cursor inside its window. Outward:
            // widens a Bell, softens a slope, straightens a Tilt.
            const float hx = p.getX() + v.freq01 * p.getWidth();
            if (v.type == SHAPE_EQ_TILT)
            {
                // Lever: its ANGLE is the tilt (±kTiltLeverDeg = ±full
                // range, raise = brighter), its LENGTH the rounding
                // (pull out = straighter).
                const float hy  = handleY(v, p);
                const float ddx = e.position.x - hx;
                const float ddy = e.position.y - hy;
                const float deg = juce::jlimit(-kTiltLeverDeg, kTiltLeverDeg,
                    juce::radiansToDegrees(std::atan2(-ddy, juce::jmax(1.0f, ddx))));
                v.gain_db = deg / kTiltLeverDeg * kGainRange;
                const float L = std::sqrt(ddx * ddx + ddy * ddy);
                const float t = juce::jlimit(0.0f, 1.0f,
                    (L - kGripMinPx) / (kGripMaxPx - kGripMinPx));
                v.width01 = 1.0f - t;
                writeHandleAsGesture(dragging_, v);
                repaint();
                return;
            }
            if (dragGrip_ == kGripUp)   // DJ resonance arrow: height = reso
            {
                // RELATIVE — delta from the grab over the arrow's travel
                // AT GRAB TIME. The handle rides the curve, so with the
                // knob fully closed (handle AT its own resonance bump)
                // the arrow's base climbs with the value: an absolute
                // inverse re-solved against the moving base runs away
                // (flip-flops 0 / 100 %). A fixed origin has no feedback.
                const float t = juce::jlimit(0.0f, 1.0f,
                    shape_eq_dj_reso(dragStartG_)
                        + (dragStartY_ - e.position.y) / dragStartSpan_);
                v.gain_db = t * SHAPE_EQ_RESO_DB_MAX;
                writeHandleAsGesture(dragging_, v);
                repaint();
                return;
            }
            const float d  = (e.position.x - hx) * (float) dragGrip_;
            const float t  = juce::jlimit(0.0f, 1.0f,
                (d - kGripMinPx) / (kGripMaxPx - kGripMinPx));
            v.width01 = (v.type == SHAPE_EQ_BELL) ? t : 1.0f - t;
            writeHandleAsGesture(dragging_, v);
            repaint();
            return;
        }

        if (dragShift_
            && (v.type == SHAPE_EQ_BELL || v.type == SHAPE_EQ_TILT))
        {
            // Shift: vertical travel sweeps width / Tilt rounding.
            v.width01 = juce::jlimit(0.0f, 1.0f,
                dragStartW_ + (dragStartY_ - e.position.y) / p.getHeight());
        }
        else
        {
            v.freq01 = xToFreq01(e.position.x, p);
            // Only the Bell takes a vertical from its handle (RELATIVE —
            // delta from grab: the handle rides the CURVE, so its absolute
            // y is not the value). Every other type moves in x only: the
            // DJ resonance lives on its vertical arrow, the Tilt amount on
            // its lever, the LP/HP slope on the chevron — one gesture, one
            // affordance, nothing hidden on the handle.
            if (v.type == SHAPE_EQ_BELL)
            {
                const float dGain = (dragStartY_ - e.position.y)
                                  / (p.getHeight() * 0.5f) * kGainRange;
                v.gain_db = juce::jlimit(-kGainRange, kGainRange,
                                         dragStartG_ + dGain);
            }
        }
        writeHandleAsGesture(dragging_, v);
        hovered_ = dragging_;
        repaint();
    }

    //==========================================================================
    // Right-click menu — MIDI Learn (selected-handle trio) + Type + Delete.
    //==========================================================================
    /** MIDI-target display name per `which`, worded for the TYPE it steers
     *  right now (the underlying id never changes — same CC, new label):
     *  Tilt GAIN reads TILT, DJ GAIN reads RESO, filter WIDTH reads SLOPE… */
    static const char* whichName(int type, int w) noexcept
    {
        if (w == 0) return "FREQ";
        if (w == 1)
            return type == SHAPE_EQ_TILT ? "TILT"
                 : type == SHAPE_EQ_DJ   ? "RESO" : "GAIN";
        return type == SHAPE_EQ_BELL ? "WIDTH"
             : type == SHAPE_EQ_TILT ? "ROUND" : "SLOPE";
    }

    /** Title-case variant for the value-box labels below the frame. */
    static const char* boxLabel(int type, int w) noexcept
    {
        if (w == 0) return "Freq";
        if (w == 1)
            return type == SHAPE_EQ_TILT ? "Tilt"
                 : type == SHAPE_EQ_DJ   ? "Reso" : "Gain";
        return type == SHAPE_EQ_BELL ? "Width"
             : type == SHAPE_EQ_TILT ? "Round" : "Slope";
    }

    //==========================================================================
    // Value boxes (below the frame) — bound to the SELECTED handle
    //==========================================================================
    void rebindBoxes()
    {
        boundSel_  = selected_;
        boundType_ = cur_[selected_].type;
        const int t = boundType_;
        freqAtt_.reset();  gainAtt_.reset();  widthAtt_.reset();
        freqLearn_.reset(); gainLearn_.reset(); widthLearn_.reset();

        const bool active = (t != SHAPE_EQ_OFF);
        freqBox_ .setEnabled(active);
        gainBox_ .setEnabled(active && t != SHAPE_EQ_LP && t != SHAPE_EQ_HP);
        widthBox_.setEnabled(active);

        // Display formats — the boxes talk the selected type's language.
        freqBox_.textFromValueFunction = [this](double v)
        {
            const double hz = minF_
                * std::pow(maxF_ / minF_, juce::jlimit(0.0, 1.0, v));
            return hz >= 1000.0
                ? juce::String(hz / 1000.0, 2) + " kHz"
                : juce::String((int) std::lround(hz)) + " Hz";
        };
        gainBox_.textFromValueFunction = [t](double v)
        {
            if (t == SHAPE_EQ_DJ)
                return juce::String((int) std::lround(
                           shape_eq_dj_reso((float) v) * 100.0f)) + " %";
            return juce::String(v, 1) + " dB";
        };
        widthBox_.textFromValueFunction = [t](double v)
        {
            if (t == SHAPE_EQ_BELL)
                return juce::String(shape_eq_bell_octaves((float) v), 2) + " oct";
            if (t == SHAPE_EQ_TILT)
                return juce::String((int) std::lround(v * 100.0)) + " %";
            return juce::String((int) std::lround(
                       shape_eq_filter_slope((float) v))) + " dB/oct";
        };

        if (apvtsMode() && active && paramIdFn_)
        {
            // The attachments own the round-trip — no direct model writes.
            freqBox_.onValueChange  = nullptr;
            gainBox_.onValueChange  = nullptr;
            widthBox_.onValueChange = nullptr;
            const juce::String sh = "Sh" + juce::String(selected_);
            freqAtt_  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                            *apvts_, paramIdFn_(sh + "Freq"),  freqBox_);
            gainAtt_  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                            *apvts_, paramIdFn_(sh + "Gain"),  gainBox_);
            widthAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                            *apvts_, paramIdFn_(sh + "Width"), widthBox_);
        }
        else if (! apvtsMode())
        {
            freqBox_ .setRange(0.0, 1.0, 0.001);
            gainBox_ .setRange(-SHAPE_EQ_DB_MAX, SHAPE_EQ_DB_MAX, 0.1);
            widthBox_.setRange(0.0, 1.0, 0.001);
            freqBox_.onValueChange = [this]
            { cur_[selected_].freq01 = (float) freqBox_.getValue();
              if (onChange) onChange(); repaint(); };
            gainBox_.onValueChange = [this]
            { cur_[selected_].gain_db = (float) gainBox_.getValue();
              if (onChange) onChange(); repaint(); };
            widthBox_.onValueChange = [this]
            { cur_[selected_].width01 = (float) widthBox_.getValue();
              if (onChange) onChange(); repaint(); };
            syncBoxValues();
        }

        // Right-click on a box = the same virtual "selected handle" targets
        // as the canvas menu.
        if (midiMap_ != nullptr && midiTargetIdFn && active)
        {
            auto mkLearn = [this](juce::Component& c, int w)
                -> std::unique_ptr<MidiLearnAttachment>
            {
                const juce::String id = midiTargetIdFn(w);
                if (id.isEmpty()) return nullptr;
                return std::make_unique<MidiLearnAttachment>(*midiMap_, c, id);
            };
            freqLearn_  = mkLearn(freqBox_,  0);
            gainLearn_  = mkLearn(gainBox_,  1);
            widthLearn_ = mkLearn(widthBox_, 2);
        }
        repaint();   // labels follow the type
    }

    /** String mode: mirror the model into the boxes (attachments own it in
     *  APVTS mode). */
    void syncBoxValues()
    {
        if (apvtsMode()) return;
        const auto& h = cur_[selected_];
        if (! freqBox_.isMouseButtonDown())
            freqBox_.setValue(h.freq01, juce::dontSendNotification);
        if (! gainBox_.isMouseButtonDown())
            gainBox_.setValue(h.gain_db, juce::dontSendNotification);
        if (! widthBox_.isMouseButtonDown())
            widthBox_.setValue(h.width01, juce::dontSendNotification);
    }

    void showMenu(int hit)
    {
        juce::PopupMenu menu;
        const int menuType = cur_[hit >= 0 ? hit : selected_].type;

        if (midiMap_ != nullptr && midiTargetIdFn)
        {
            for (int w = 0; w < 3; ++w)
            {
                const juce::String id = midiTargetIdFn(w);
                if (id.isEmpty()) continue;
                // GAIN is inert on LP/HP — no learn bait for a dead CC.
                const bool inert = (w == 1 && (menuType == SHAPE_EQ_LP
                                            || menuType == SHAPE_EQ_HP));
                const juce::String name(whichName(menuType, w));
                const bool learningThis = midiMap_->isLearning()
                                       && midiMap_->learningParamId() == id;
                if (learningThis)
                    menu.addItem(1 + w,
                        juce::String::fromUTF8("Learning\xE2\x80\xA6 (cancel) ")
                            + name, true, true);
                else if (! inert)
                    menu.addItem(1 + w, "MIDI Learn " + name);
                const juce::String mapped = midiMap_->mappingDescription(id);
                if (mapped.isNotEmpty())
                    menu.addItem(4 + w, "Remove " + name
                                        + " mapping (" + mapped + ")");
            }
        }

        if (hit >= 0)
        {
            if (menu.getNumItems() > 0) menu.addSeparator();
            juce::PopupMenu types;
            for (int t = SHAPE_EQ_BELL; t < SHAPE_EQ_NUM_TYPES; ++t)
                types.addItem(10 + t, typeName(t), true, cur_[hit].type == t);
            menu.addSubMenu("Type", types);
            menu.addItem(20, "Delete handle");
        }

        if (menu.getNumItems() == 0) return;
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this)
                                      .withMousePosition(),
            [this, hit](int choice)
            {
                if (choice >= 1 && choice <= 3 && midiMap_ && midiTargetIdFn)
                {
                    const juce::String id = midiTargetIdFn(choice - 1);
                    const bool learningThis = midiMap_->isLearning()
                                           && midiMap_->learningParamId() == id;
                    if (learningThis) midiMap_->cancelLearn();
                    else              midiMap_->startLearn(id);
                }
                else if (choice >= 4 && choice <= 6 && midiMap_ && midiTargetIdFn)
                    midiMap_->removeMappingFor(midiTargetIdFn(choice - 4));
                else if (choice >= 10 + SHAPE_EQ_BELL
                      && choice < 10 + SHAPE_EQ_NUM_TYPES && hit >= 0)
                    changeType(hit, choice - 10);
                else if (choice == 20 && hit >= 0)
                {
                    writeType(hit, SHAPE_EQ_OFF);
                    repaint();
                }
            });
    }

    void changeType(int h, int newType)
    {
        if (cur_[h].type == newType) return;
        ShapeEqHandle v = cur_[h];
        v.type = newType;
        // Sensible per-type re-seed so a switch never lands on a screaming
        // preset: DJ centres (flat, no reso), LP/HP/Bell take the centred
        // width (24 dB/oct / 1 oct), Tilt starts as a straight line.
        switch (newType)
        {
            case SHAPE_EQ_DJ:   v.freq01 = 0.5f; v.width01 = 0.5f;
                                v.gain_db = 0.0f;                  break;
            case SHAPE_EQ_LP:
            case SHAPE_EQ_HP:
            case SHAPE_EQ_BELL: v.width01 = 0.5f;                  break;
            case SHAPE_EQ_TILT: v.width01 = 0.0f;                  break;
            default: break;
        }
        writeType(h, newType);
        writeHandleComplete(h, v);
        repaint();
    }

    //==========================================================================
    // Model write-through — APVTS attachments or the local (string) handles.
    //==========================================================================
    struct Field
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
    };

    void bindField(int h, int field, const juce::String& id)
    {
        auto& f = fields_[h][field];
        f.attach.reset();
        f.param = apvts_->getParameter(id);
        jassert(f.param != nullptr);
        if (f.param == nullptr) return;
        f.attach = std::make_unique<juce::ParameterAttachment>(
            *f.param, [this, h, field](float v)
            {
                switch (field)
                {
                    case kFieldType:  cur_[h].type    = (int) std::lround(v); break;
                    case kFieldFreq:  cur_[h].freq01  = v; break;
                    case kFieldGain:  cur_[h].gain_db = v; break;
                    default:          cur_[h].width01 = v; break;
                }
                repaint();
            });
        f.attach->sendInitialUpdate();
    }

    bool apvtsMode() const noexcept { return apvts_ != nullptr; }

    void beginGestures(int h)
    {
        if (! apvtsMode()) return;
        for (int f = kFieldFreq; f <= kFieldWidth; ++f)
            if (fields_[h][f].attach) fields_[h][f].attach->beginGesture();
    }

    void endGestures(int h)
    {
        if (! apvtsMode()) return;
        for (int f = kFieldFreq; f <= kFieldWidth; ++f)
            if (fields_[h][f].attach) fields_[h][f].attach->endGesture();
    }

    /** Freq/Gain/Width mid-drag write (inside an open gesture). */
    void writeHandleAsGesture(int h, const ShapeEqHandle& v)
    {
        if (apvtsMode())
        {
            if (auto& a = fields_[h][kFieldFreq].attach)  a->setValueAsPartOfGesture(v.freq01);
            if (auto& a = fields_[h][kFieldGain].attach)  a->setValueAsPartOfGesture(v.gain_db);
            if (auto& a = fields_[h][kFieldWidth].attach) a->setValueAsPartOfGesture(v.width01);
        }
        else
        {
            cur_[h].freq01 = v.freq01; cur_[h].gain_db = v.gain_db;
            cur_[h].width01 = v.width01;
            if (onChange) onChange();
        }
    }

    /** Freq/Gain/Width one-shot write (menu / reset — complete gestures). */
    void writeHandleComplete(int h, const ShapeEqHandle& v)
    {
        if (apvtsMode())
        {
            if (auto& a = fields_[h][kFieldFreq].attach)  a->setValueAsCompleteGesture(v.freq01);
            if (auto& a = fields_[h][kFieldGain].attach)  a->setValueAsCompleteGesture(v.gain_db);
            if (auto& a = fields_[h][kFieldWidth].attach) a->setValueAsCompleteGesture(v.width01);
        }
        else
        {
            cur_[h].freq01 = v.freq01; cur_[h].gain_db = v.gain_db;
            cur_[h].width01 = v.width01;
            if (onChange) onChange();
        }
    }

    void writeType(int h, int type)
    {
        if (apvtsMode())
        {
            if (auto& a = fields_[h][kFieldType].attach)
                a->setValueAsCompleteGesture((float) type);
        }
        else
        {
            cur_[h].type = type;
            if (onChange) onChange();
        }
    }

    //==========================================================================
    // LEVEL fader write-through
    //==========================================================================
    void bindLevel(const juce::String& id)
    {
        levelId_ = id;
        levelField_.attach.reset();
        levelField_.param = apvts_->getParameter(id);
        jassert(levelField_.param != nullptr);
        if (levelField_.param == nullptr) return;
        levelField_.attach = std::make_unique<juce::ParameterAttachment>(
            *levelField_.param, [this](float v) { levelDb_ = v; repaint(); });
        levelField_.attach->sendInitialUpdate();
    }

    void beginLevelGesture()
    { if (apvtsMode() && levelField_.attach) levelField_.attach->beginGesture(); }
    void endLevelGesture()
    { if (apvtsMode() && levelField_.attach) levelField_.attach->endGesture(); }

    void writeLevelAsGesture(float v)
    {
        if (apvtsMode())
        { if (auto& a = levelField_.attach) a->setValueAsPartOfGesture(v); }
        else { levelDb_ = v; if (onChange) onChange(); }
    }

    void writeLevelComplete(float v)
    {
        if (apvtsMode())
        { if (auto& a = levelField_.attach) a->setValueAsCompleteGesture(v); }
        else { levelDb_ = v; if (onChange) onChange(); }
    }

    //==========================================================================
    void timerCallback() override
    {
        // One watcher for every selection/type change path (click, chips,
        // menu, CC-driven, decodeState): rebind the value boxes when the
        // bound target drifts, mirror the model into them otherwise.
        if (boundSel_ != selected_ || boundType_ != cur_[selected_].type)
            rebindBoxes();
        else
            syncBoxValues();
        if (isShowing()) repaint();
    }

    //==========================================================================
    juce::AudioProcessorValueTreeState* apvts_ = nullptr;
    juce::Colour accent;
    juce::String title_ { "EQ" };
    double minF_ = 65.41, maxF_ = 16744.04;   // label span (informative)

    int family_ { 0 };   // EqHandleMidiTargets family of the bound bank
    int slot_   { 0 };   // pool slot of the bound instance (live overlay)
    juce::String midiIds_[3];
    MidiMappingEngine* midiMap_ = nullptr;

    ShapeEqHandle cur_[SHAPE_EQ_MAX_HANDLES];   // painted model (both modes)
    float levelDb_ = 0.0f;                      // whole-curve gain (fader)
    Field fields_[SHAPE_EQ_MAX_HANDLES][4];     // APVTS mode attachments
    Field levelField_;                          // APVTS mode Level attachment
    juce::String levelId_;                      // Level param id (learn menu)
    std::function<juce::String(const juce::String&)> paramIdFn_;   // bank ids

    // Value boxes below the frame — rebound to the selected handle.
    Sp3ctraBarSlider freqBox_, gainBox_, widthBox_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        freqAtt_, gainAtt_, widthAtt_;
    std::unique_ptr<MidiLearnAttachment> freqLearn_, gainLearn_, widthLearn_;
    int boundSel_ = -1, boundType_ = -1;        // what the boxes are bound to

    int selected_ = 0, hovered_ = -1, dragging_ = -1;
    int dragGrip_ = 0, hoverGrip_ = 0;   // ±1 = a chevron of the selection
    int hoverChip_ = 0;                  // SHAPE_EQ_* under the cursor (chips)
    bool dragFader_ = false, hoverFader_ = false;
    bool  dragShift_  = false;
    float dragStartW_ = 0.5f, dragStartG_ = 0.0f;
    float dragStartY_ = 0.0f, dragStartX_ = 0.0f;
    float dragStartSpan_ = kGripMaxPx - kGripMinPx;   // reso arrow travel at grab

    juce::String hint_;
    juce::uint32 hintUntilMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShapeEqComponent)
};
