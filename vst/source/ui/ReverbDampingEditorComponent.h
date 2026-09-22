/**
 * @file ReverbDampingEditorComponent.h
 * @brief DAMPING view for the REVERB tail — how long the tail rings along
 *        the pixel/frequency axis (bass left, treble right).
 *
 * The view draws the TAIL LENGTH at every axis position, relative to the
 * Decay setting: the top of the plot = the full Decay, and the scale is
 * logarithmic down to ×1/64 at the bottom (LUX_REVERB_DAMP_OCTAVES = the
 * strongest damping). The curve IS the applied law — lux_reverb_damp_rate,
 * sampled with the axis span the RT built its LUT with — so what is drawn
 * is what the tail does.
 *
 *   • TREBLE handle (filled node, right edge of the curve) → drag
 *     vertically → Damping: how much faster the treble edge fades than the
 *     bass (double-click: default · hold: type · right-click: MIDI learn).
 *   • LIN / AIR chips (top-right) → the law between the two ends:
 *     LINEAR = a straight line on this log scale, every step toward the
 *     treble fades the same factor faster; AIR = air absorption, nil over
 *     the bass and kneeing up over the top octaves — a hall
 *     (right-click a chip: MIDI learn).
 *
 * The resulting decay is printed in seconds at both ends of the axis. The
 * curve brightens while the bound pool instance still has a tail ringing.
 * Chrome (frame, caption) = ModuleChrome; handle and chips = Sp3ctraHandles.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include "ModuleEditorChrome.h"
#include "../processing/lux_reverb.h"   // self-manages extern "C" linkage

class ReverbDampingEditorComponent : public juce::Component,
                                     private juce::Timer
{
public:
    static constexpr int kPreferredH = 76;   // graph only — boxes live on the main editor

    ReverbDampingEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                                 juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning editor calls setInstance().
        setRepaintsOnMouseActivity(true);
        startTimerHz(15);   // live glow + the AIR knee follows the RT's axis span
    }

    ~ReverbDampingEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance). */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind to one instance's Decay (readouts) / Damping / law params
     *  and point the live glow at that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& decayId,
                     const juce::String& dampingId,
                     const juce::String& dampTypeId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        dcy.attach.reset(); amt.attach.reset(); typ.attach.reset();
        dampingId_  = dampingId;
        dampTypeId_ = dampTypeId;
        bind(dcy, decayId);
        bind(amt, dampingId);
        bind(typ, dampTypeId);
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
            const LuxReverbState& st = *lux_reverb_instance(slot_);
            const bool live = (st.config.enabled != 0 && st.tail_active != 0
                               && st.tail_peak > 0.0f);

            // Full-decay reference — the bass edge of every law sits here.
            g.setColour(accent.withAlpha(0.22f));
            g.drawHorizontalLine((int) geo.plot.getY(), geo.plot.getX(), geo.plot.getRight());

            // The tail-length curve, filled down to the shortest tail.
            juce::Path curve;
            const int W = juce::jmax(2, (int) geo.plot.getWidth());
            for (int px = 0; px <= W; ++px)
            {
                const float x = geo.plot.getX() + (float) px;
                const float y = yForU(geo, (float) px / (float) W);
                if (px == 0) curve.startNewSubPath(x, y);
                else         curve.lineTo(x, y);
            }
            {
                juce::Path fill = curve;
                fill.lineTo(geo.plot.getRight(), geo.plot.getBottom());
                fill.lineTo(geo.plot.getX(),     geo.plot.getBottom());
                fill.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.30f : 0.12f));
                g.fillPath(fill);
            }
            g.setColour(accent.withAlpha(live ? 0.95f : 0.70f));
            g.strokePath(curve, juce::PathStrokeType(1.5f));

            // Treble handle.
            const auto look = handleLook();
            Sp3ctraHandles::drawNode(g, handlePos(geo), look);

            // Resulting decay at both ends — the absolute meaning of the
            // normalised shape. The hot side shows "Damping" (lime) in
            // place of its readout, so the two never collide.
            const bool hot = Sp3ctraHandles::isHot(look);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            g.setColour(accent.withAlpha(0.55f));
            g.drawText(secondsText(geo.decayS / rateAt(geo, 0.0f)),
                       (int) geo.plot.getX() + 2, (int) geo.plot.getBottom() + 1,
                       64, 9, juce::Justification::centredLeft, false);
            g.setColour(hot ? Sp3ctraHandles::colour() : accent.withAlpha(0.55f));
            g.drawText(hot ? juce::String("Damping")
                           : secondsText(geo.decayS / rateAt(geo, 1.0f)),
                       (int) geo.plot.getRight() - 66, (int) geo.plot.getBottom() + 1,
                       64, 9, juce::Justification::centredRight, false);
        }

        ModuleChrome::drawCaption(g, frame, accent, "DAMPING");

        // Law chips, top-right in the caption strip.
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        for (int i = 0; i < kNumLaws; ++i)
            Sp3ctraHandles::drawChip(g, chipRect(i), kLawNames[i],
                                     lawIndex() == i,
                                     dragging == Hit::None && hovered == chipHit(i),
                                     2.5f);
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != Hit::None) return;
        const Hit h = hitAt(e.position, computeGeometry());
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == Hit::None   ? juce::MouseCursor::NormalCursor
                     : h == Hit::Handle ? juce::MouseCursor::UpDownResizeCursor
                                        : juce::MouseCursor::PointingHandCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == Hit::None && hovered != Hit::None) { hovered = Hit::None; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const Hit h = hitAt(e.position, computeGeometry());

        // Right-click → MIDI Learn for what sits under the pointer.
        if (e.mods.isPopupMenu())
        {
            if (midiMap_ != nullptr)
            {
                if (h == Hit::Handle)      MidiLearnPopup::show(*midiMap_, dampingId_,  this);
                else if (h != Hit::None)   MidiLearnPopup::show(*midiMap_, dampTypeId_, this);
            }
            return;
        }
        if (e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick

        pressed_ = h;
        if (h == Hit::Handle)
        {
            dragging = h;
            hovered  = h;
            amt.begin();
            hold_.arm(e, [this] { holdToType(); });   // long press = type
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hold_.fired()) return;            // the entry bubble owns the rest
        hold_.moved(e);
        if (dragging != Hit::Handle) return;
        const Geometry geo = computeGeometry();
        if (! geo.valid) return;
        // Down = shorter treble tail. The handle's height IS the amount:
        // both laws land on the same edge ratio (lux_reverb_damp_rate).
        const float a = (e.position.y - geo.plot.getY()) / juce::jmax(1.0f, geo.plot.getHeight());
        amt.setGesture(juce::jlimit(0.0f, 100.0f, a * 100.0f));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        hold_.release();
        if (dragging == Hit::Handle) amt.end();
        // A plain click on a chip selects its law (one complete host gesture).
        if (dragging == Hit::None && e.mouseWasClicked() && ! e.mods.isPopupMenu())
            for (int i = 0; i < kNumLaws; ++i)
                if (pressed_ == chipHit(i) && hitAt(e.position, computeGeometry()) == pressed_)
                    typ.setComplete((float) i);
        dragging = Hit::None;
        pressed_ = Hit::None;
        hovered  = hitAt(e.position, computeGeometry());
        repaint();
    }

    /** Double-click on the handle = Damping back to its default. */
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (hitAt(e.position, computeGeometry()) == Hit::Handle)
            Sp3ctraGestures::toDefault(boundsOf());
    }

private:
    enum class Hit { None, Handle, ChipLinear, ChipAir };

    static constexpr int         kNumLaws = 2;   // = LUX_REVERB_DAMP_* count
    static constexpr const char* kLawNames[kNumLaws] = { "LIN", "AIR" };
    static Hit chipHit(int i) noexcept { return i == 0 ? Hit::ChipLinear : Hit::ChipAir; }

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    Sp3ctraGestures::BoundList boundsOf() { return { { "Damping", &amt } }; }

    /** Long press on the handle: the drag gesture closes, the bubble opens. */
    void holdToType()
    {
        if (dragging == Hit::Handle) amt.end();
        dragging = Hit::None;
        repaint();
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), boundsOf());
    }

    Sp3ctraGestures::Hold hold_;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float decayS = 3.0f;      // s
        float amount = 0.0f;      // 0..1
        int   law    = LUX_REVERB_DAMP_AIR;
        float span   = 8.0f;      // axis span in octaves (the RT's LUT key)
        bool  valid  = false;
    };

    Geometry computeGeometry() const
    {
        Geometry geo;
        const auto frame = getLocalBounds().toFloat();
        if (frame.getWidth() < 40.0f || frame.getHeight() < 30.0f) return geo;
        geo.plot   = ModuleChrome::plotOf(frame);   // end readouts sit in the bottom inset
        geo.decayS = juce::jlimit(0.1f, 20.0f, dcy.value);
        geo.amount = juce::jlimit(0.0f, 1.0f, amt.value / 100.0f);
        geo.law    = lawIndex();
        // The span the RT built its LUT with (0 before the first line).
        const float span = lux_reverb_instance(slot_)->damp_key_span;
        geo.span   = (span > 0.0f) ? span : 8.0f;
        geo.valid  = geo.plot.getWidth() > 10.0f && geo.plot.getHeight() > 8.0f;
        return geo;
    }

    int lawIndex() const noexcept
    { return juce::jlimit(0, kNumLaws - 1, (int) std::lround(typ.value)); }

    /** Fade-rate multiplier at axis position u — THE shared law. */
    static float rateAt(const Geometry& geo, float u) noexcept
    { return lux_reverb_damp_rate(geo.law, geo.amount, u, geo.span); }

    /** Plot y of the tail length at u: top = ×1, bottom = ×2^-OCTAVES. */
    static float yForU(const Geometry& geo, float u) noexcept
    {
        const float oct = std::log2(juce::jmax(1.0f, rateAt(geo, u)));
        return geo.plot.getY() + juce::jlimit(0.0f, 1.0f, oct / LUX_REVERB_DAMP_OCTAVES)
                                 * geo.plot.getHeight();
    }

    juce::Point<float> handlePos(const Geometry& geo) const noexcept
    {
        return { geo.plot.getRight() - kNodeR,
                 juce::jlimit(geo.plot.getY() + kNodeR, geo.plot.getBottom() - kNodeR,
                              yForU(geo, 1.0f)) };
    }

    juce::Rectangle<float> chipRect(int i) const noexcept
    {
        const auto frame = getLocalBounds().toFloat();
        const float right = frame.getRight() - 8.0f - (float) (kNumLaws - 1 - i) * (kChipW + 3.0f);
        return { right - kChipW, frame.getY() + 2.0f, kChipW, kChipH };
    }

    Hit hitAt(juce::Point<float> p, const Geometry& geo) const
    {
        for (int i = 0; i < kNumLaws; ++i)
            if (chipRect(i).expanded(1.0f).contains(p)) return chipHit(i);
        if (geo.valid && p.getDistanceFrom(handlePos(geo)) < kHitR) return Hit::Handle;
        return Hit::None;
    }

    static juce::String secondsText(float s)
    {
        return (s >= 1.0f ? juce::String(s, 1) : juce::String(s, 2)) + " s";
    }

    /** Handle look — Idle / Hover / Drag, lit by any edit of Damping
     *  (a box, a CC, automation — ui/Sp3ctraControls.h). */
    Sp3ctraHandles::Look handleLook() const noexcept
    {
        return Sp3ctraHandles::stateOf(dragging == Hit::Handle,
                                       dragging == Hit::None && hovered == Hit::Handle,
                                       false, amt.heat());
    }

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    using Bound = Sp3ctraControls::Bound;

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.bind(apvts, id, [this](float) { repaint(); });
    }

    static constexpr float kNodeR       = Sp3ctraHandles::kNodeR;
    static constexpr float kHitR        = 12.0f;
    static constexpr float kChipW       = 28.0f;
    static constexpr float kChipH       = 12.0f;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (span + live glow)

    Bound dcy, amt, typ;
    juce::String dampingId_, dampTypeId_;
    MidiMappingEngine* midiMap_ = nullptr;

    Hit hovered  { Hit::None };
    Hit dragging { Hit::None };
    Hit pressed_ { Hit::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbDampingEditorComponent)
};
