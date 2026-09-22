/**
 * @file VideoMixRadar.h
 * @brief The VIDEO MIX toile — every patched VIDEO SCROLL output on its own
 *        SPOKE with its own handle (the polygon IS the mix), the blend mode
 *        as a chip beside the spoke's label, and the PROJECTOR at the
 *        centre: the one-hand sweep that dims whatever it points away from
 *        (law in video/VideoMixFocus.h, shared with the compositor).
 *
 * Replaces the row-per-output fader strip (2026-09-04, user choice among
 * four mixer models): same parameters — videoMix{slot}_level / _blend —
 * plus the two global projector params (videoMixFocusX / Y). Nothing to
 * migrate.
 *
 * Reading the toile:
 *   • spoke i (rack order, clockwise from the top): its handle sits at
 *     level × R from the centre; the SOLID polygon joins the EFFECTIVE
 *     levels (level × projector weight) — what the compositor renders;
 *     when the projector is off-centre the BASE polygon stays as a dashed
 *     outline so the user sees what the sweep is masking;
 *   • the LINK chip, top-left corner — the VIDEO MIX ⇄ AUDIO MIX link
 *     (video/VideoMixFollow.h; everything but on/off lives in its menu).
 *     Toward the SOUND (default) the projector IS the audio mix of the
 *     chains on the toile: centre = every chain at 50 %, a spoke = that
 *     chain alone, in between a crossfade (VideoMixFocus::audioWeight) —
 *     the AUDIO MIX faders show it as a ceiling line. Toward the PICTURE
 *     it is the reverse: every output whose chain is not part of what is
 *     currently heard is dimmed, a second projector, automatic instead of
 *     hand-held, same contract (it can only mask), same reading (the
 *     solid polygon is what the compositor renders, the dashed one is the
 *     levels it is masking);
 *   • label + chip at the tip, outside the rim: the label is the output's
 *     chain — its numbered pastille + name, the one chain identity of the
 *     rack header and the MIDI MAP (ChainIdentity.h) — and a link to its
 *     chain tab, like the old strip rows; the chip cycles MIX / ADD / SCR;
 *   • a disabled output (videoScroll{slot}_enabled off) is ghosted;
 *   • the handle BLINKS with the output's incoming stream (the render
 *     core's flow envelopes, pushed by the mixer's presenter) — "who is
 *     talking" at a glance. It is a blinker, not a glow, and it stays in
 *     the handle's CORE alone — the beam is never part of it: a line
 *     strobing the width of the toile reads as an alarm, while the lamp
 *     tells the same story quietly. BOTH the rate and the depth follow the
 *     content, so a trickle winks slowly and dim while a dense stream
 *     strobes fast and bright. The unfed white sweep stays dark.
 *     Beside the blend chip the same reading is PRINTED as a percentage —
 *     the tap's power — so activity can be compared between outputs and
 *     read on an output that is momentarily masked or turned down;
 *
 * Gestures (the charter of ui/Sp3ctraGestureSlider.h, canvas flavour):
 *   • drag a handle, or click / drag anywhere along its spoke → that level
 *     (absolute, projected on the spoke);
 *   • drag the projector → focus (unit disc, dead zone at the centre);
 *   • double-click a handle → default level; the projector → centre;
 *   • click the chip → next blend mode; click the label → the chain tab;
 *   • right-click → MIDI learn of the thing under the pointer (level,
 *     blend, or the two focus axes as sub-menus off any handle).
 *
 * Colours: the polygon, rim and spokes are DISPLAY (module colour); each
 * spoke's handle and label wear the chain's identity tint like the AUDIO
 * MIX strips; the projector — the one universal control — is THE control
 * colour. Every handle climbs the shared ladder (ui/Sp3ctraControls.h):
 * desaturated at rest, full under the pointer, white ring while edited —
 * by the mouse or by a MIDI CC / automation (Bound heat).
 *
 * Message thread only.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"   // MidiLearnPopup
#include "../video/VideoMixFocus.h"
#include "../video/VideoMixFollow.h"
#include "ChainIdentity.h"
#include "ModuleCatalog.h"
#include "ModuleParamManifest.h"           // vsParam / vsMixParam
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class VideoMixRadar : public juce::Component,
                      public  juce::TooltipClient,
                      private juce::Timer
{
public:
    /** One patched output, in RACK order (the spoke index). */
    struct Output
    {
        int          slot     { -1 };
        int          chainIdx { 0 };
        juce::String label;      ///< "CHAIN n" / user chain name (+ a/b…)
    };

    explicit VideoMixRadar(Sp3ctraAudioProcessor& p)
        : apvts_(p.getAPVTS()), midiMap_(p.getMidiMap())
    {
        setOpaque(false);
        bindFocus();
    }

    ~VideoMixRadar() override { stopTimer(); }

    /** A click on a spoke label → the mixer forwards it to the editor,
     *  which opens that output's chain tab. */
    std::function<void(int slot)> onOutputClicked;

    /** (Re)build the spokes. Same slot list → labels / colours refreshed in
     *  place, bindings kept (a chain rename must not rebind anything). */
    void setOutputs(const std::vector<Output>& outs)
    {
        bool same = outs.size() == spokes_.size();
        for (size_t i = 0; same && i < outs.size(); ++i)
            same = outs[i].slot == spokes_[i]->slot;

        if (! same)
        {
            spokes_.clear();
            for (const auto& o : outs)
            {
                auto s = std::make_unique<Spoke>();
                s->slot = o.slot;
                bindParam(s->level,   vsMixParam(o.slot, "level"));
                bindParam(s->blend,   vsMixParam(o.slot, "blend"));
                bindParam(s->enabled, vsParam   (o.slot, "enabled"));
                spokes_.push_back(std::move(s));
            }
            drag_ = press_ = hover_ = {};
        }
        for (size_t i = 0; i < outs.size(); ++i)
        {
            spokes_[i]->chainIdx = outs[i].chainIdx;
            spokes_[i]->label    = outs[i].label;
            spokes_[i]->colour   = ChainIdentity::colour(outs[i].chainIdx);
        }
        repaint();
    }

    int numSpokes() const noexcept { return (int) spokes_.size(); }

    /** Live stream of spoke `idx` — VideoScrollRenderCore::flowNow / flowPeak
     *  (0…1), pushed by the mixer's presenter. Repaints that handle only
     *  when the reading actually moved. */
    void setFlow(int idx, float now, float peak, float power)
    {
        if (idx < 0 || idx >= (int) spokes_.size()) return;
        auto& s = *spokes_[(size_t) idx];
        s.flowNow  = juce::jlimit(0.0f, 1.0f, now);
        s.flowPeak = juce::jlimit(0.0f, 1.0f, peak);
        const float pw = juce::jlimit(0.0f, 1.0f, power);
        if (std::abs(pw - s.flowPower) >= 0.005f)   // ≥ 1 printed point
        {
            s.flowPower = pw;
            repaint(powerBoxOf(idx).toNearestInt());
        }
        // Painting is the blink timer's business — a flowing output must
        // repaint on the BLINK cadence, not on the envelope's own steps.
        if (s.flowPeak > kFlowMin && ! isTimerRunning())
            startTimerHz(kAnimHz);
    }

    /** AUDIO-FOLLOW mask of spoke `idx` (0…1, 1 = untouched) — computed and
     *  smoothed ONCE by the mixer's presenter (VideoMixerComponent::
     *  updateFollow) and handed to the compositor in the same breath, so the
     *  polygon drawn here IS the one rendered. `active` = this spoke takes
     *  part (armed, and its chain feeds at least one engine). */
    void setFollow(int idx, float w, bool active)
    {
        if (idx < 0 || idx >= (int) spokes_.size()) return;
        auto& s = *spokes_[(size_t) idx];
        const float v = juce::jlimit(0.0f, 1.0f, w);
        if (std::abs(v - s.follow) < 0.004f && active == s.followOn) return;
        s.follow   = v;
        s.followOn = active;
        repaint();
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto geo   = geometry();
        const auto now   = Sp3ctraControls::nowMs();
        const auto disp  = moduleColour(ModuleType::VideoScroll);
        const auto frame = getLocalBounds().toFloat();

        // Frame — the graphic-editor window every module page draws in.
        g.setColour(juce::Colour(Sp3ctraTheme::kColFrameBg));
        g.fillRoundedRectangle(frame, 3.0f);
        g.setColour(disp.withAlpha(0.25f));
        g.drawRoundedRectangle(frame.reduced(0.5f), 3.0f, 1.0f);

        if (spokes_.empty())
            return;

        const int n = (int) spokes_.size();

        // Guides: rim (level 1), half ring, spokes.
        {
            const float dash[2] = { 3.0f, 4.0f };
            juce::Path rim, rimDashed;
            rim.addEllipse(geo.c.x - geo.r, geo.c.y - geo.r, 2.0f * geo.r, 2.0f * geo.r);
            juce::PathStrokeType(1.0f).createDashedStroke(rimDashed, rim, dash, 2);
            g.setColour(disp.withAlpha(0.28f));
            g.fillPath(rimDashed);

            const float dot[2] = { 1.5f, 4.5f };
            juce::Path half, halfDotted;
            const float hr = geo.r * 0.5f;
            half.addEllipse(geo.c.x - hr, geo.c.y - hr, 2.0f * hr, 2.0f * hr);
            juce::PathStrokeType(1.0f).createDashedStroke(halfDotted, half, dot, 2);
            g.setColour(disp.withAlpha(0.16f));
            g.fillPath(halfDotted);

            g.setColour(disp.withAlpha(0.20f));
            for (int i = 0; i < n; ++i)
                g.drawLine({ geo.c, geo.tip[(size_t) i] }, 1.0f);
        }

        const bool focusOn = geo.focus.x != 0.0f || geo.focus.y != 0.0f;
        bool followMasks = false;
        for (const auto& sp : spokes_)
            followMasks = followMasks || (sp->followOn && sp->follow < 0.995f);

        // Base polygon (dashed) — only while something masks: the projector,
        // the audio-follow, or both. It is what the mix WOULD be.
        if (focusOn || followMasks)
        {
            const juce::Path base = polygon(geo, false);
            juce::Path baseDashed;
            const float dash[2] = { 3.0f, 3.0f };
            juce::PathStrokeType(1.0f).createDashedStroke(baseDashed, base, dash, 2);
            g.setColour(disp.withAlpha(0.45f));
            g.fillPath(baseDashed);
        }

        // Effective polygon — the mix as the compositor sees it.
        {
            juce::Path eff = polygon(geo, true);
            g.setColour(disp.withAlpha(0.13f));
            g.fillPath(eff);
            g.setColour(disp.withAlpha(0.75f));
            g.strokePath(eff, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered,
                                                   juce::PathStrokeType::rounded));
        }

        const float focusHeat = Sp3ctraControls::heatOfAny({ &focusX_, &focusY_ }, now);
        const bool  focusEdit = drag_.kind == Hit::Focus || focusHeat > 0.0f;

        // Spokes: handle (identity tint), label (link), chip (blend).
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        for (int i = 0; i < n; ++i)
        {
            auto&       s  = *spokes_[(size_t) i];
            const auto& sg = geo.spoke[(size_t) i];
            const bool  on = s.enabled.value >= 0.5f;
            const float a  = on ? 1.0f : Sp3ctraTheme::kCtlAlphaOff;

            // Label — the chain's numbered pastille + name (ChainIdentity),
            // a link to its tab: the name brightens + underlines on hover.
            {
                const bool labHov = hover_.kind == Hit::Label && hover_.idx == i;
                auto box = sg.label;
                const auto dot = box.removeFromLeft(kPastilleD).withSizeKeepingCentre(kPastilleD, kPastilleD);
                box.removeFromLeft(kPastilleGap);
                ChainIdentity::drawPastille(g, dot, s.chainIdx + 1, s.colour, a);
                g.setColour((labHov ? s.colour.brighter(0.5f) : s.colour).withMultipliedAlpha(a));
                g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
                g.drawText(s.label, box.toNearestInt(), juce::Justification::centredLeft, true);
                if (labHov)
                    g.fillRect(box.getX(), box.getBottom() - 2.0f, box.getWidth(), 1.0f);
            }

            // Chip — MIX / ADD / SCR (a control: the shared chip recipe).
            {
                const bool chipHov = (hover_.kind == Hit::Chip && hover_.idx == i)
                                   || s.blend.heat(now) > 0.0f;
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                Sp3ctraHandles::drawChip(g, sg.chip, blendName((int) std::lround(s.blend.value)),
                                         false, chipHov, 2.5f);
            }

            // Tap power — the measured drive, printed. A silent tap prints a
            // faint "0%" rather than nothing: an output that is NOT receiving
            // is exactly what one comes here to find out. (No box in compact
            // mode: that width goes to the rim instead.)
            if (! sg.power.isEmpty())
            {
                const float pw = s.flowPower;
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(s.colour.withMultipliedAlpha((0.30f + 0.70f * pw) * a));
                g.drawText(juce::String((int) std::lround(pw * 100.0f)) + "%",
                           sg.power.toNearestInt(),
                           sg.dir.x < -0.35f ? juce::Justification::centredRight
                                             : juce::Justification::centredLeft, false);
            }

            // Activity BLINKER — the handle's core carries it alone (below).
            // The beam stays quiet: a 2 px line switching on and off from the
            // centre to the rim swept the whole toile and read as an alarm.
            const float blink = blinkOf(s);

            // Handle — the level, in the chain's identity tint.
            {
                const bool dragging = drag_.kind == Hit::Handle && drag_.idx == i;
                const bool hovered  = (hover_.kind == Hit::Handle || hover_.kind == Hit::Spoke)
                                    && hover_.idx == i;
                const auto look = Sp3ctraControls::stateOf(dragging, hovered, false, s.level.heat(now));
                drawTintedNode(g, sg.handle, look, s.colour, kHandleR, a);
                // …whose core is the blinker's lamp: it flashes with the
                // stream, staying inside the handle (same footprint at rest
                // and at full flow).
                if (blink > 0.01f)
                {
                    const float rr = kHandleR - 1.2f;
                    g.setColour(s.colour.brighter(1.0f).withMultipliedAlpha(blink * a));
                    g.fillEllipse(sg.handle.x - rr, sg.handle.y - rr, 2.0f * rr, 2.0f * rr);
                }

                // AUDIO tick — where the follow put this output. Drawn in the
                // AUDIO MIX blue, on the spoke, with the stretch it removed
                // faded behind it: the ONE thing that makes the link legible
                // ("the picture moved because the audio moved").
                if (s.followOn && sg.follow < 0.995f)
                {
                    const float lv = juce::jlimit(0.0f, 1.0f, s.level.value);
                    const auto  v  = geo.c + sg.dir * (geo.r * lv * sg.weight * sg.follow);
                    const juce::Point<float> perp { -sg.dir.y, sg.dir.x };
                    const auto blue = juce::Colour(kAudioTint);
                    g.setColour(blue.withAlpha(0.30f * a));
                    g.drawLine({ v, sg.handle }, 1.0f);
                    g.setColour(blue.withMultipliedAlpha(a));
                    g.drawLine(v.x - perp.x * 4.5f, v.y - perp.y * 4.5f,
                               v.x + perp.x * 4.5f, v.y + perp.y * 4.5f, 2.0f);
                }

                // Readouts stay INSIDE the rim (toward the centre), never on
                // the labels outside it.
                if (dragging || s.level.heat(now) > 0.0f)
                    drawReadoutInside(g, geo, sg, percent(s.level.value));
                else if (focusEdit && sg.weight * sg.follow < 0.995f)
                    drawReadoutInside(g, geo, sg,
                                      percent(s.level.value * sg.weight * sg.follow));
            }
        }

        // Projector — THE control colour. Link to the centre while off it.
        {
            const bool dragging = drag_.kind == Hit::Focus;
            const bool hovered  = hover_.kind == Hit::Focus;
            const auto look = Sp3ctraControls::stateOf(dragging, hovered, false, focusHeat);
            if (focusOn)
                Sp3ctraHandles::drawLink(g, geo.c, geo.focusPt, look);
            // Centre cross — where "neutral" is.
            g.setColour(Sp3ctraHandles::rest().withAlpha(0.55f));
            g.drawLine(geo.c.x - 4.0f, geo.c.y, geo.c.x + 4.0f, geo.c.y, 1.0f);
            g.drawLine(geo.c.x, geo.c.y - 4.0f, geo.c.x, geo.c.y + 4.0f, 1.0f);
            Sp3ctraHandles::drawRing(g, geo.focusPt, look, kFocusR);
            g.setColour(Sp3ctraHandles::ink(look).line);
            g.fillEllipse(geo.focusPt.x - 1.5f, geo.focusPt.y - 1.5f, 3.0f, 3.0f);
        }

        drawFollowBlock(g, geo, now);
    }

    void resized() override { repaint(); }

    /** The toile carries no written legend — it says what is under the
     *  pointer instead (the editor owns a TooltipWindow). */
    juce::String getTooltip() override
    {
        switch (hover_.kind)
        {
            case Hit::FollowArm:
                return "LINK: this toile drives the sound - each chain is heard at the "
                       "level its output has here (handle x projector). No fader is ever "
                       "written, so switching it off gives both mixes back untouched. "
                       "Right-click for the direction, the amount and the LIVE flavour.";
            case Hit::Focus:
                return "Projector: drag it toward an output to dim the others "
                       "(double-click: neutral \xC2\xB7 hold: type).";
            case Hit::Chip:
                return "Blend mode of this output: MIX / ADD / SCREEN.";
            case Hit::Label:
                return "Open this chain's page.";
            case Hit::Handle:
            case Hit::Spoke:
                return "Level of this output in the video mix (double-click: default \xC2\xB7 hold: type).";
            default: break;
        }
        return {};
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (drag_.kind != Hit::None) return;
        setHover(hitAt(e.position));
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (drag_.kind == Hit::None) setHover({});
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;          // learn menu lands on mouseUp
        if (e.getNumberOfClicks() != 1) return;    // 2nd click → mouseDoubleClick
        press_ = hitAt(e.position);
        drag_  = {};
        switch (press_.kind)
        {
            case Hit::Focus:
                focusX_.begin(); focusY_.begin();
                drag_ = press_;
                break;
            case Hit::Handle:
                spokes_[(size_t) press_.idx]->level.begin();
                drag_ = press_;
                break;
            case Hit::Spoke:
                // Click / drag along the spoke = go to the pointed level.
                spokes_[(size_t) press_.idx]->level.begin();
                drag_ = { Hit::Handle, press_.idx };
                applyLevelAt(press_.idx, e.position);
                break;
            default: break;
        }
        if (drag_.kind != Hit::None)
            hold_.arm(e, [this] { holdToType(); });   // long press = type
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (hold_.fired()) return;                 // the entry bubble owns the rest
        hold_.moved(e);
        if (drag_.kind == Hit::Focus)       applyFocusAt(e.position);
        else if (drag_.kind == Hit::Handle) applyLevelAt(drag_.idx, e.position);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        hold_.release();
        if (e.mods.isPopupMenu())
        {
            drag_ = press_ = {};
            showLearnMenu(hitAt(e.position));
            return;
        }
        if (drag_.kind == Hit::Focus)       { focusX_.end(); focusY_.end(); }
        else if (drag_.kind == Hit::Handle) spokes_[(size_t) drag_.idx]->level.end();
        else if (e.mouseWasClicked() && press_.kind == Hit::FollowArm)
            follow_.setComplete(follow_.value >= 0.5f ? 0.0f : 1.0f);
        else if (e.mouseWasClicked() && press_.idx >= 0 && press_.idx < (int) spokes_.size())
        {
            auto& s = *spokes_[(size_t) press_.idx];
            if (press_.kind == Hit::Chip)
                s.blend.setComplete((float) (((int) std::lround(s.blend.value) + 1) % 3));
            else if (press_.kind == Hit::Label && onOutputClicked)
                onOutputClicked(s.slot);
        }
        drag_ = press_ = {};
        setHover(hitAt(e.position));
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        const auto h = hitAt(e.position);
        if (h.kind == Hit::Focus)
        {
            focusX_.setComplete(0.0f);
            focusY_.setComplete(0.0f);
        }
        else if ((h.kind == Hit::Handle || h.kind == Hit::Spoke) && h.idx >= 0)
        {
            auto& b = spokes_[(size_t) h.idx]->level;
            if (b.param != nullptr)
                b.setComplete(b.param->convertFrom0to1(b.param->getDefaultValue()));
        }
        drag_ = press_ = {};
        repaint();
    }

private:
    //==========================================================================
    enum class Hit { None, Focus, Handle, Spoke, Chip, Label, FollowArm };
    struct HitInfo
    {
        Hit kind { Hit::None };
        int idx  { -1 };
        bool operator== (const HitInfo& o) const noexcept { return kind == o.kind && idx == o.idx; }
        bool operator!= (const HitInfo& o) const noexcept { return ! (*this == o); }
    };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    // Double-click resets the projector / a level (mouseDoubleClick above);
    // the long press types it.
    void holdToType()
    {
        const HitInfo h = drag_;
        if (h.kind == Hit::Focus)       { focusX_.end(); focusY_.end(); }
        else if (h.kind == Hit::Handle) spokes_[(size_t) h.idx]->level.end();
        drag_ = press_ = {};
        repaint();
        Sp3ctraGestures::BoundList l;
        if (h.kind == Hit::Focus)
            l = { { "Focus X", &focusX_ }, { "Focus Y", &focusY_ } };
        else if (h.kind == Hit::Handle && h.idx >= 0 && h.idx < (int) spokes_.size())
            l = { { "Level", &spokes_[(size_t) h.idx]->level } };
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), l);
    }
    Sp3ctraGestures::Hold hold_;

    struct Spoke
    {
        int          slot     { -1 };
        int          chainIdx { 0 };
        juce::String label;
        juce::Colour colour;
        Sp3ctraControls::Bound level, blend, enabled;
        float follow     { 1.0f };   ///< audio-follow mask (1 = not masked)
        bool  followOn   { false };  ///< armed AND this chain feeds an engine
        float flowNow    { 0.0f };   ///< live stream, fast envelope (the lamp)
        float flowPeak   { 0.0f };   ///< its rémanence, slow envelope (the rate)
        float flowPower  { 0.0f };   ///< the printed reading (symmetric smoothing)
        float blinkPhase { 0.0f };   ///< blinker oscillator, radians
    };

    /** Screen geometry of the toile for the current bounds + values. */
    struct SpokeGeo
    {
        juce::Point<float>     dir;        ///< screen unit vector (y down)
        juce::Point<float>     tipAt1;     ///< rim point (level 1) — alias tip
        juce::Point<float>     handle;     ///< base level position
        float                  weight { 1.0f };   ///< projector mask
        float                  follow { 1.0f };   ///< audio-follow mask
        juce::Rectangle<float> label, chip;   ///< label = pastille + gap + name
        juce::Rectangle<float> power;         ///< printed tap power, beside the chip
    };
    struct Geo
    {
        juce::Point<float>              c;
        float                           r { 0.0f };
        juce::Point<float>              focus;     ///< (right, up), snapped
        juce::Point<float>              focusPt;   ///< screen
        std::vector<juce::Point<float>> tip;
        std::vector<SpokeGeo>           spoke;
        // FOLLOW block (top-left corner): plate, its two chips, and the
        // depth arch — an arc whose x maps monotonically to the value, so
        // "drag along the arch" is literally what the gesture does.
        juce::Rectangle<float>          fBlock, fArm;
        bool                            compact { false };
    };

    static constexpr float kSideBand  = 92.0f;   ///< room for a side label (pastille + name) + gap
    // COMPACT: in a narrow column the side labels ate the whole width and the
    // rim collapsed to a few pixels (the toile became unreadable while its
    // band kept its full height). Below kCompactW the names and the printed
    // tap power step aside — the numbered pastille and the blend chip carry
    // the identity — and the rim gets the width back.
    static constexpr float kSideBandC = 34.0f;
    static constexpr float kCompactW  = 294.0f;
    static constexpr float kTopBand   = 26.0f;   ///< label line above / below the rim
    static constexpr float kLabelMaxW = 58.0f;   ///< the NAME part; ellipsis past it
    static constexpr float kLabelH    = 14.0f;
    static constexpr float kPastilleD = 14.0f;   ///< chain pastille, the label line height
    static constexpr float kPastilleGap = 3.0f;
    static constexpr float kChipW     = 28.0f;
    static constexpr float kChipH     = 12.0f;
    static constexpr float kPowerW    = 30.0f;   ///< "100%" at kFontMicro + air
    static constexpr float kPowerGap  = 4.0f;
    static constexpr float kHandleR   = 5.5f;
    // FOLLOW block — corner plate, its chips and the depth arch.
    static constexpr float kFolPad    = 4.0f;    ///< margin from the frame
    static constexpr float kFolArmW   = 40.0f;
    static constexpr float kFolModeW  = 30.0f;
    static constexpr float kFolGap    = 4.0f;
    static constexpr float kFolInset  = 4.0f;    ///< plate padding
    // The AUDIO MIX badge blue — the follow speaks with the audio's voice,
    // not with the video module's colour nor with the control lime.
    static constexpr uint32_t kAudioTint = 0xff5a9de0;
    static constexpr float kFocusR    = 7.0f;
    static constexpr float kHitR      = 10.0f;
    static constexpr float kSpokeHit  = 7.0f;
    // Activity blinker: BOTH ends follow the content — the rate (a trickle
    // winks, a dense stream strobes) and the depth (dim vs. bright).
    static constexpr float kFlowMin    = 0.02f;   ///< below: no stream, no blink
    static constexpr int   kAnimHz     = 30;      ///< blink repaint cadence
    static constexpr float kBlinkHzMin = 2.5f;    ///< rate at the faintest stream
    static constexpr float kBlinkHzMax = 10.0f;   ///< rate at full stream
    static constexpr float kBlinkFloor = 0.20f;   ///< how far "off" the lamp goes

    Geo geometry() const
    {
        Geo geo;
        const auto b = getLocalBounds().toFloat();
        geo.c = b.getCentre();
        geo.compact = b.getWidth() < kCompactW;
        const float side = geo.compact ? kSideBandC : kSideBand;
        geo.r = juce::jmax(20.0f, juce::jmin((b.getHeight() - 2.0f * kTopBand) * 0.5f,
                                             (b.getWidth()  - 2.0f * side) * 0.5f));
        geo.focus   = VideoMixFocus::focusVector(focusX_.value, focusY_.value);
        geo.focusPt = { geo.c.x + geo.focus.x * geo.r, geo.c.y - geo.focus.y * geo.r };

        const int n = (int) spokes_.size();
        geo.tip.reserve((size_t) n);
        geo.spoke.reserve((size_t) n);
        const juce::Font labelFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        for (int i = 0; i < n; ++i)
        {
            const auto& s = *spokes_[(size_t) i];
            SpokeGeo sg;
            const auto d = VideoMixFocus::spokeDir(i, n);
            sg.dir    = { d.x, -d.y };
            sg.tipAt1 = geo.c + sg.dir * geo.r;
            sg.weight = VideoMixFocus::weight(i, n, focusX_.value, focusY_.value);
            sg.follow = juce::jlimit(0.0f, 1.0f, s.follow);
            const float lv = juce::jlimit(0.0f, 1.0f, s.level.value);
            sg.handle = geo.c + sg.dir * (geo.r * lv);

            // Label (pastille + name, exactly sized) + chip outside the rim,
            // on the spoke's side. Content always reads left → right.
            const float nameW = geo.compact
                ? 0.0f
                : juce::jmin(kLabelMaxW,
                             std::ceil(juce::GlyphArrangement::getStringWidth(labelFont, s.label)) + 2.0f);
            const float tw = geo.compact ? kPastilleD : kPastilleD + kPastilleGap + nameW;
            const auto anchor = sg.tipAt1 + sg.dir * 9.0f;
            if (std::abs(sg.dir.x) >= 0.35f)
            {
                // Side: label beside the tip, chip + power under it. The whole
                // block hugs the tip's side, so it reads outward-in on the left
                // and inward-out on the right without ever crossing the rim.
                const float x = sg.dir.x > 0.0f ? anchor.x : anchor.x - tw;
                sg.label = { x, anchor.y - kLabelH * 0.5f - (kChipH + 2.0f) * 0.5f, tw, kLabelH };
                const float rowY = sg.label.getBottom() + 2.0f;
                if (geo.compact)       // pastille over its chip, hugging the tip
                {
                    sg.chip  = { sg.dir.x > 0.0f ? x : x + tw - kChipW, rowY, kChipW, kChipH };
                    sg.power = {};      // the printed power steps aside first
                }
                else if (sg.dir.x > 0.0f)   // [chip][power], left-aligned on the block
                {
                    sg.chip  = { x, rowY, kChipW, kChipH };
                    sg.power = { sg.chip.getRight() + kPowerGap, rowY, kPowerW, kChipH };
                }
                else                   // [power][chip], right-aligned on the block
                {
                    sg.chip  = { x + tw - kChipW, rowY, kChipW, kChipH };
                    sg.power = { sg.chip.getX() - kPowerGap - kPowerW, rowY, kPowerW, kChipH };
                }
            }
            else
            {
                // Top / bottom: label centred on the spoke, chip then power to
                // its right.
                const float y = sg.dir.y < 0.0f ? anchor.y - kLabelH : anchor.y;
                const float rowW = geo.compact ? tw + 4.0f + kChipW
                                               : tw + 4.0f + kChipW + kPowerGap + kPowerW;
                sg.label = { anchor.x - rowW * 0.5f, y, tw, kLabelH };
                sg.chip  = { sg.label.getRight() + 4.0f, y + (kLabelH - kChipH) * 0.5f, kChipW, kChipH };
                sg.power = geo.compact
                             ? juce::Rectangle<float>()
                             : juce::Rectangle<float>(sg.chip.getRight() + kPowerGap,
                                                      sg.chip.getY(), kPowerW, kChipH);
            }
            geo.tip.push_back(sg.tipAt1);
            geo.spoke.push_back(sg);
        }

        // LINK chip, top-left. Drawn LAST over its own plate: a spoke label
        // pointing up-left may pass under it, and a control must stay
        // readable whatever the toile does behind it. ONE chip and no more:
        // the direction, the dosing and the LIVE flavour live in its
        // right-click menu — on a toile squeezed into its minimum band, four
        // controls for one switch ate a quarter of the picture.
        {
            geo.fBlock = { b.getX() + kFolPad, b.getY() + kFolPad,
                           kFolArmW + 2.0f * kFolInset, kChipH + 2.0f * kFolInset };
            geo.fArm = { geo.fBlock.getX() + kFolInset, geo.fBlock.getY() + kFolInset,
                         kFolArmW, kChipH };
        }
        return geo;
    }

    juce::Path polygon(const Geo& geo, bool effective) const
    {
        juce::Path p;
        const int n = (int) spokes_.size();
        for (int i = 0; i < n; ++i)
        {
            const auto& sg = geo.spoke[(size_t) i];
            const float lv = juce::jlimit(0.0f, 1.0f, spokes_[(size_t) i]->level.value)
                           * (effective ? sg.weight * sg.follow : 1.0f);
            const auto pt = geo.c + sg.dir * (geo.r * lv);
            if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
        }
        if (n == 1)                       // one output: a ray reads better than a point
            p.lineTo(geo.c);
        p.closeSubPath();
        return p;
    }

    HitInfo hitAt(juce::Point<float> p) const
    {
        const auto geo = geometry();
        if (spokes_.empty()) return {};
        // The FOLLOW block is painted over the toile — it is hit first too.
        if (geo.fArm.expanded(3.0f).contains(p)) return { Hit::FollowArm, -1 };
        if (p.getDistanceFrom(geo.focusPt) <= kHitR) return { Hit::Focus, -1 };

        const int n = (int) spokes_.size();
        int best = -1; float bestD = kHitR;
        for (int i = 0; i < n; ++i)
        {
            const float d = p.getDistanceFrom(geo.spoke[(size_t) i].handle);
            if (d <= bestD) { bestD = d; best = i; }
        }
        if (best >= 0) return { Hit::Handle, best };

        for (int i = 0; i < n; ++i)
        {
            const auto& sg = geo.spoke[(size_t) i];
            if (sg.chip.expanded(2.0f).contains(p))  return { Hit::Chip,  i };
            if (sg.label.expanded(2.0f).contains(p)) return { Hit::Label, i };
        }
        for (int i = 0; i < n; ++i)
        {
            const auto& sg = geo.spoke[(size_t) i];
            const auto  v  = p - geo.c;
            const float along = v.x * sg.dir.x + v.y * sg.dir.y;
            if (along < -kSpokeHit || along > geo.r + kSpokeHit) continue;
            const float across = std::abs(v.x * sg.dir.y - v.y * sg.dir.x);
            if (across <= kSpokeHit) return { Hit::Spoke, i };
        }
        return {};
    }

    void setHover(HitInfo h)
    {
        if (h == hover_) return;
        hover_ = h;
        setMouseCursor(h.kind == Hit::Label || h.kind == Hit::Chip
                           || h.kind == Hit::FollowArm
                           ? juce::MouseCursor::PointingHandCursor
                           : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void applyLevelAt(int idx, juce::Point<float> p)
    {
        if (idx < 0 || idx >= (int) spokes_.size()) return;
        const auto geo = geometry();
        const auto& sg = geo.spoke[(size_t) idx];
        const auto  v  = p - geo.c;
        const float along = (v.x * sg.dir.x + v.y * sg.dir.y) / geo.r;
        spokes_[(size_t) idx]->level.setGesture(juce::jlimit(0.0f, 1.0f, along));
    }

    void applyFocusAt(juce::Point<float> p)
    {
        const auto geo = geometry();
        const auto f = VideoMixFocus::focusVector((p.x - geo.c.x) / geo.r,
                                                  -(p.y - geo.c.y) / geo.r);
        focusX_.setGesture(f.x);
        focusY_.setGesture(f.y);
    }

    //── Right-click ───────────────────────────────────────────────────────────
    void showLearnMenu(HitInfo h)
    {
        if (h.kind == Hit::FollowArm) { showLinkMenu(); return; }
        if (h.idx >= 0 && h.idx < (int) spokes_.size())
        {
            const int slot = spokes_[(size_t) h.idx]->slot;
            if (h.kind == Hit::Chip)
            { MidiLearnPopup::show(midiMap_, vsMixParam(slot, "blend"), this); return; }
            if (h.kind == Hit::Handle || h.kind == Hit::Spoke || h.kind == Hit::Label)
            { MidiLearnPopup::show(midiMap_, vsMixParam(slot, "level"), this); return; }
        }
        // The projector (or the empty toile): one sub-menu per axis.
        juce::PopupMenu menu;
        const juce::StringArray ids { VideoMixFocus::kXId, VideoMixFocus::kYId };
        const char* names[] = { "Focus X", "Focus Y" };
        for (int i = 0; i < 2; ++i)
        {
            juce::PopupMenu sub;
            MidiLearnPopup::addItems(sub, midiMap_, ids[i], MidiLearnPopup::subMenuBase(i));
            menu.addSubMenu(names[i], sub);
        }
        MidiMappingEngine& engine = midiMap_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                           [&engine, ids](int choice)
                           {
                               for (int i = 0; i < ids.size(); ++i)
                                   if (MidiLearnPopup::handle(engine, ids[i], choice,
                                                              MidiLearnPopup::subMenuBase(i)))
                                       return;
                           });
    }

    /** EVERYTHING about the link except its on/off — deliberately off the
     *  toile. The chip is the option that was asked for; these are the ones
     *  that came with it, and a mixer squeezed into its minimum band has no
     *  room to spend on them. They stay real parameters (automatable, MIDI
     *  mappable through the sub-menus at the bottom). */
    void showLinkMenu()
    {
        const bool toAudio = dir_.value >= 0.5f;
        const int  depthPc = juce::roundToInt(juce::jlimit(0.0f, 1.0f, depth_.value) * 100.0f);
        // Ids 1..99 are this menu's own; every learn entry lives at
        // MidiLearnPopup::subMenuBase(i) >= 1000, so a step added here can
        // never quietly become someone's "MIDI Learn".
        juce::PopupMenu menu;
        menu.addItem(1, "Link the two mixes", true, follow_.value >= 0.5f);
        menu.addSeparator();
        menu.addSectionHeader("Which mix drives which");
        menu.addItem(2, "This toile drives the sound", true, toAudio);
        menu.addItem(3, "The audio mix drives the picture", true, ! toAudio);
        menu.addItem(4, "Follow what is playing, not the faders (LIVE)",
                     ! toAudio, mode_.value >= 0.5f);
        menu.addSeparator();
        juce::PopupMenu amount;
        const int steps[] = { 25, 50, 75, 100 };
        for (int i = 0; i < 4; ++i)
            amount.addItem(10 + i, juce::String(steps[i]) + " %", true, depthPc == steps[i]);
        // The depth doses the → VIDEO mask only: toward the sound the projector
        // is the whole law (centre = 50 %, spoke = solo).
        menu.addSubMenu("Amount (" + juce::String(depthPc) + " %)", amount, ! toAudio);
        menu.addSeparator();
        const juce::StringArray ids { VideoMixFollow::kArmId, VideoMixFollow::kDepthId,
                                      VideoMixFollow::kDirId, VideoMixFollow::kModeId };
        const char* names[] = { "MIDI: Link", "MIDI: Amount",
                                "MIDI: Direction", "MIDI: Live" };
        for (int i = 0; i < ids.size(); ++i)
        {
            juce::PopupMenu sub;
            MidiLearnPopup::addItems(sub, midiMap_, ids[i], MidiLearnPopup::subMenuBase(i));
            menu.addSubMenu(names[i], sub);
        }
        MidiMappingEngine& engine = midiMap_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                           [this, &engine, ids](int choice)
                           {
                               switch (choice)
                               {
                                   case 1: follow_.setComplete(follow_.value >= 0.5f ? 0.0f : 1.0f); return;
                                   case 2: dir_ .setComplete(1.0f); return;
                                   case 3: dir_ .setComplete(0.0f); return;
                                   case 4: mode_.setComplete(mode_.value >= 0.5f ? 0.0f : 1.0f); return;
                                   case 10: case 11: case 12: case 13:
                                       depth_.setComplete(0.25f * (float) (choice - 9)); return;
                                   default: break;
                               }
                               for (int i = 0; i < ids.size(); ++i)
                                   if (MidiLearnPopup::handle(engine, ids[i], choice,
                                                              MidiLearnPopup::subMenuBase(i)))
                                       return;
                           });
    }

    //── Bindings + heat ───────────────────────────────────────────────────────
    void bindParam(Sp3ctraControls::Bound& b, const juce::String& id)
    {
        // Any source (drag, MIDI CC, automation, preset) lands here; the
        // Bound stamps its heat, the toile repaints while anything glows.
        b.bind(apvts_, id, [this](float) { noteEdit(); });
    }

    void bindFocus()
    {
        bindParam(focusX_, VideoMixFocus::kXId);
        bindParam(focusY_, VideoMixFocus::kYId);
        bindParam(follow_, VideoMixFollow::kArmId);
        bindParam(depth_,  VideoMixFollow::kDepthId);
        bindParam(mode_,   VideoMixFollow::kModeId);
        bindParam(dir_,    VideoMixFollow::kDirId);
    }

    void noteEdit()
    {
        repaint();
        if (! isTimerRunning()) startTimerHz(kAnimHz);
    }

    /** The blinker's lamp, 0…1: a squared-off oscillation whose DEPTH is the
     *  fast envelope (bright content = bright flash) around a floor, gated
     *  by the stream itself so a dead output is simply dark. */
    float blinkOf(const Spoke& s) const noexcept
    {
        if (s.flowNow <= kFlowMin) return 0.0f;
        const float p  = 0.5f + 0.5f * std::sin(s.blinkPhase);
        const float sq = p * p * (3.0f - 2.0f * p);   // smoothstep: squarer than a sine
        return juce::jlimit(0.0f, 1.0f, s.flowNow * (kBlinkFloor + (1.0f - kBlinkFloor) * sq));
    }

    /** The screen box of spoke `idx`'s printed power (its repaint region). */
    juce::Rectangle<float> powerBoxOf(int idx) const
    {
        const auto geo = geometry();
        if (idx < 0 || idx >= (int) geo.spoke.size()) return {};
        return geo.spoke[(size_t) idx].power.expanded(1.0f);
    }

    /** Advance every blinker; returns true while at least one is running. */
    bool advanceBlink(double nowMs)
    {
        const float dt = (float) juce::jlimit(1.0, 200.0, nowMs - lastAnimMs_);
        lastAnimMs_ = nowMs;
        bool running = false;
        const auto geo = geometry();
        for (size_t i = 0; i < spokes_.size(); ++i)
        {
            auto& s = *spokes_[i];
            if (s.flowPeak <= kFlowMin) { s.blinkPhase = 0.0f; continue; }
            running = true;
            // Rate follows the rémanence (steadier than the lamp itself, so
            // the strobe does not stutter between two bursts of lines).
            const float hz = kBlinkHzMin + (kBlinkHzMax - kBlinkHzMin) * s.flowPeak;
            s.blinkPhase = std::fmod(s.blinkPhase
                                       + juce::MathConstants<float>::twoPi * hz * dt * 0.001f,
                                     juce::MathConstants<float>::twoPi);
            // Repaint the beam's box only — never the whole toile at 30 Hz.
            const auto h = geo.spoke[i].handle;
            const float pad = kHandleR + 4.0f;
            repaint(juce::Rectangle<float>::leftTopRightBottom(
                        juce::jmin(geo.c.x, h.x) - pad, juce::jmin(geo.c.y, h.y) - pad,
                        juce::jmax(geo.c.x, h.x) + pad, juce::jmax(geo.c.y, h.y) + pad)
                        .toNearestInt());
        }
        return running;
    }

    bool anyHeat(double now = Sp3ctraControls::nowMs()) const noexcept
    {
        if (focusX_.lit(now) || focusY_.lit(now)) return true;
        if (follow_.lit(now) || depth_.lit(now) || mode_.lit(now) || dir_.lit(now))
            return true;
        for (const auto& s : spokes_)
            if (s->level.lit(now) || s->blend.lit(now)) return true;
        return false;
    }

    void timerCallback() override
    {
        const double now = Sp3ctraControls::nowMs();
        const bool blinking = advanceBlink(now);
        if (anyHeat(now) || drag_.kind != Hit::None)
            repaint();
        else if (! blinking)
            stopTimer();
    }

    //── Painting helpers ──────────────────────────────────────────────────────
    /** The LINK corner: one chip on its own plate. Everything else about
     *  the link (which mix drives which, how hard, SET or LIVE) is in its
     *  right-click menu — see showLinkMenu. */
    void drawFollowBlock(juce::Graphics& g, const Geo& geo, double now) const
    {
        const bool armed = follow_.value >= 0.5f;

        g.setColour(juce::Colour(Sp3ctraTheme::kColFrameBg).brighter(0.10f).withAlpha(0.92f));
        g.fillRoundedRectangle(geo.fBlock, 4.0f);
        g.setColour(moduleColour(ModuleType::VideoScroll).withAlpha(0.22f));
        g.drawRoundedRectangle(geo.fBlock.reduced(0.5f), 4.0f, 1.0f);

        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        Sp3ctraHandles::drawChip(g, geo.fArm, "LINK", armed,
                                 hover_.kind == Hit::FollowArm
                                     || Sp3ctraControls::heatOfAny({ &follow_, &depth_,
                                                                     &dir_, &mode_ }, now) > 0.0f,
                                 2.5f);
    }

    /** The readout pill of a spoke handle, INSIDE the rim: pushed toward the
     *  centre from the handle, then clamped to the disc's inscribed square so
     *  it never crosses the rim into the labels (the right-hand handle used
     *  to land on its own label). */
    static void drawReadoutInside(juce::Graphics& g, const Geo& geo, const SpokeGeo& sg,
                                  const juce::String& text)
    {
        auto pill = Sp3ctraHandles::readoutSize(text);
        const float w = pill.getWidth(), h = pill.getHeight();
        const auto  c = sg.handle - sg.dir * (kHandleR + 8.0f + juce::jmax(w, h) * 0.5f);
        pill.setPosition(c.x - w * 0.5f, c.y - h * 0.5f);
        const float half = geo.r * 0.70f;   // inscribed square (≈ r/√2)
        if (2.0f * half > w) pill.setX(juce::jlimit(geo.c.x - half, geo.c.x + half - w, pill.getX()));
        else                 pill.setX(geo.c.x - w * 0.5f);
        if (2.0f * half > h) pill.setY(juce::jlimit(geo.c.y - half, geo.c.y + half - h, pill.getY()));
        else                 pill.setY(geo.c.y - h * 0.5f);
        Sp3ctraHandles::drawReadoutPill(g, text, pill);
    }

    /** drawNode with an IDENTITY accent (chain tint) instead of THE control
     *  colour — the recipe of Sp3ctraHandles::drawNode, ink resolved on the
     *  same ladder through Sp3ctraControls::inkOf(accent). */
    static void drawTintedNode(juce::Graphics& g, juce::Point<float> c,
                               Sp3ctraControls::Look look, juce::Colour accent,
                               float r, float alpha)
    {
        const auto ink = Sp3ctraControls::inkOf(look, accent, alpha);
        const float rad = look.hot() ? r + 1.0f : r;
        Sp3ctraHandles::drawHalo(g, c, rad, ink.halo);
        g.setColour(ink.fill);
        g.fillEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad);
        g.setColour(ink.line);
        g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad, ink.lineW);
    }

    static const char* blendName(int mode) noexcept
    {
        switch (mode) { case 1: return "ADD"; case 2: return "SCR"; default: return "MIX"; }
    }

    static juce::String percent(float v)
    {
        return juce::String((int) std::lround(juce::jlimit(0.0f, 1.0f, v) * 100.0f)) + " %";
    }

    //==========================================================================
    juce::AudioProcessorValueTreeState&   apvts_;
    MidiMappingEngine&                    midiMap_;
    std::vector<std::unique_ptr<Spoke>>   spokes_;   // stable addresses: Bound captures itself
    Sp3ctraControls::Bound                focusX_, focusY_;
    // FOLLOW block: arm, depth, mode (video/VideoMixFollow.h).
    Sp3ctraControls::Bound                follow_, depth_, mode_, dir_;
    HitInfo hover_, press_, drag_;
    double  lastAnimMs_ { Sp3ctraControls::nowMs() };   // blinker clock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixRadar)
};
