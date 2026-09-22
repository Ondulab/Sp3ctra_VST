/**
 * @file Sp3ctraControls.h
 * @brief THE interaction charter of the Sp3ctra UI — the ONE place that says
 *        what "you are not touching this" and "this is being edited" look
 *        like, for every clickable thing in the interface.
 *
 * Two colour roles (unchanged charter):
 *   • DISPLAY  — curves, fills, frames, captions, labels, rack, mixer:
 *                the module's CATEGORY colour (ModuleCatalog::moduleColour).
 *   • CONTROL  — anything you can grab: handles, chevrons, thumbs, grabbable
 *                lines, bar sliders, toggles, combos, knobs, chips:
 *                Sp3ctraTheme::kColHandle (acid lime), or an identity accent
 *                on the mixer strips.
 *
 * What this file adds on top of that split is the INTERACTION LADDER — four
 * rungs, one grammar, used by the graphic-editor handles (Sp3ctraHandles),
 * by every widget (Sp3ctraLookAndFeel) and by the canvases:
 *
 *   Idle      resting: the accent DESATURATED (restOf) at kCtlAlphaIdle —
 *             at rest a page reads calm, controls do not shout;
 *   Selected  persistent selection (the EQ handle the boxes / CCs talk to):
 *             full accent, filled, outer ring;
 *   Hover     full accent, filled, halo — "you can grab this";
 *   Edit      being changed RIGHT NOW: full accent, WHITE ring, bigger halo.
 *
 * Edit is not only the mouse. A control moved by a MIDI CC, by host
 * automation or by a preset is being edited exactly the same way, so it
 * lights the same — and stays lit for kCtlGlowMs after the last move (the
 * HEAT, 1 → 0). Two sources feed it, and nothing else has to know:
 *   • Bound          — a parameter binding for canvas editors: any change,
 *                      from any source, stamps the handle's heat;
 *   • touch(Component) — a widget's "a controller just moved you" stamp,
 *                      raised by MidiLearnAttachment — the one object that
 *                      knows (component, paramId) for every mappable control
 *                      — when the editor drains the engine's last touched
 *                      parameter (MidiTouch bus below).
 *
 * Nothing outside this file restates an alpha, a ring width or a state
 * colour: change a rung here (or its token in UITheme.h) and the whole
 * interface follows.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <utility>
#include <vector>
#include "../UITheme.h"

namespace Sp3ctraControls
{
    //══════════════════════════════════════════════════════════════════════════
    // 1. The ladder
    //══════════════════════════════════════════════════════════════════════════

    /** The four rungs — see the file header. `Edit` covers a mouse drag AND a
     *  remote move (MIDI / automation); the difference is only how long it
     *  lasts, which is what `Look::heat` carries. */
    enum class State { Idle, Selected, Hover, Edit };

    /** A control's full interaction reading: its rung + the fading heat of a
     *  remote edit (0 = none). Implicitly built from a State, so every call
     *  site that only knows Idle/Hover/Edit keeps working unchanged. */
    struct Look
    {
        State state { State::Idle };
        float heat  { 0.0f };          ///< 0…1, remote-edit glow

        Look() = default;
        Look(State s, float h = 0.0f) noexcept
            : state(s), heat(juce::jlimit(0.0f, 1.0f, h)) {}

        bool operator== (State s) const noexcept { return state == s; }
        bool operator!= (State s) const noexcept { return state != s; }

        /** Hot = the user is on it (hover) or it is moving (drag / remote). */
        bool hot() const noexcept
        { return state == State::Hover || state == State::Edit || heat > 0.0f; }

        /** Filled = anything but a resting, un-glowing control. */
        bool filled() const noexcept { return state != State::Idle || heat > 0.0f; }

        /** How far along the "being edited" rung this control sits. */
        float edit() const noexcept
        { return state == State::Edit ? 1.0f : heat; }
    };

    /** Fold the usual editor booleans (+ optional remote heat) into a Look. */
    inline Look stateOf(bool dragging, bool hovered, bool selected = false,
                        float heat = 0.0f) noexcept
    {
        if (dragging) return { State::Edit, 1.0f };
        if (hovered)  return { State::Hover, heat };
        return { selected ? State::Selected : State::Idle, heat };
    }

    //══════════════════════════════════════════════════════════════════════════
    // 2. Colours
    //══════════════════════════════════════════════════════════════════════════

    /** THE control colour — a control that is hot. */
    inline juce::Colour active() noexcept { return juce::Colour(Sp3ctraTheme::kColHandle); }

    /** The ring of a control being edited (mouse or MIDI). */
    inline juce::Colour hot()    noexcept { return juce::Colour(Sp3ctraTheme::kColHandleHot); }

    /** Dark core of a resting handle / bar interior. */
    inline juce::Colour core()   noexcept { return juce::Colour(Sp3ctraTheme::kColHandleCore); }

    /** True when `accent` is THE control colour, as opposed to an IDENTITY
     *  tint (a module category colour, a mixer strip hue). */
    inline bool isControlColour(juce::Colour c) noexcept { return c == active(); }

    /** The resting version of an accent.
     *
     *  THE control colour desaturates: it says nothing but "you can touch
     *  this", so at rest it steps back and the page reads calm. An IDENTITY
     *  tint keeps its hue — on a catalogue chip, a rack card or a mixer
     *  strip the hue IS the information; only its intensity climbs the
     *  ladder (kCtlAlphaIdle → kCtlAlphaHot). */
    inline juce::Colour restOf(juce::Colour accent) noexcept
    {
        return isControlColour(accent)
                 ? accent.interpolatedWith(juce::Colour(Sp3ctraTheme::kColCtlNeutral),
                                           Sp3ctraTheme::kCtlRestMix)
                 : accent;
    }

    /** The resting version of THE control colour. */
    inline juce::Colour rest() noexcept { return restOf(active()); }

    //══════════════════════════════════════════════════════════════════════════
    // 3. The ink resolver — the only place a rung becomes pixels
    //══════════════════════════════════════════════════════════════════════════

    /** What to paint a control with, once its Look is known. */
    struct Ink
    {
        juce::Colour line;    ///< ring / outline / stroke / arrow
        juce::Colour fill;    ///< filled core, knob, pointer
        juce::Colour halo;    ///< soft glow under a hot control (may be transparent)
        float lineW  { Sp3ctraTheme::kCtlLineIdle };
        bool  filled { false };

        bool hasHalo() const noexcept { return halo.getFloatAlpha() > 0.004f; }
    };

    /** Role weights — how loud a role must be to READ the same. A 1 px
     *  outline running around a wide rectangle carries far more ink than a
     *  small ring, so bars, combos and toggle tracks pass kRoleOutline as
     *  their `alpha`; handles, thumbs, cursors and glyphs stay at 1.
     *  These are the only two weights: no branch invents a third. */
    constexpr float kRoleHandle  = 1.00f;
    constexpr float kRoleOutline = 0.70f;

    /** Resolve a Look into ink for `accent` (the control colour by default;
     *  an identity tint on the mixer strips). `alpha` ghosts the whole thing
     *  (a grip in a dead zone), `enabled` mutes an inert control. */
    inline Ink inkOf(Look lk, juce::Colour accent = active(),
                     float alpha = 1.0f, bool enabled = true)
    {
        Ink ink;
        if (! enabled)
        {
            ink.line = restOf(accent).withMultipliedAlpha(Sp3ctraTheme::kCtlAlphaOff * alpha);
            ink.fill = core().withMultipliedAlpha(alpha);
            ink.halo = juce::Colours::transparentBlack;
            ink.lineW = Sp3ctraTheme::kCtlLineIdle;
            return ink;
        }

        // Base rung.
        juce::Colour lineC;
        float lineA = Sp3ctraTheme::kCtlAlphaIdle, haloA = 0.0f;
        switch (lk.state)
        {
            case State::Idle:
                lineC = restOf(accent); lineA = Sp3ctraTheme::kCtlAlphaIdle;
                ink.lineW = Sp3ctraTheme::kCtlLineIdle; break;
            case State::Selected:
                lineC = accent;         lineA = Sp3ctraTheme::kCtlAlphaSel;
                ink.lineW = Sp3ctraTheme::kCtlLineSel;  break;
            case State::Hover:
                lineC = accent;         lineA = Sp3ctraTheme::kCtlAlphaHot;
                haloA = Sp3ctraTheme::kCtlHaloHover;
                ink.lineW = Sp3ctraTheme::kCtlLineHot;  break;
            case State::Edit:
                lineC = hot();          lineA = Sp3ctraTheme::kCtlAlphaHot;
                haloA = Sp3ctraTheme::kCtlHaloEdit;
                ink.lineW = Sp3ctraTheme::kCtlLineEdit; break;
        }

        // Remote heat pulls the rung toward Edit (white ring, wider, haloed).
        const float h = lk.state == State::Edit ? 0.0f : lk.heat;
        if (h > 0.0f)
        {
            lineC     = lineC.interpolatedWith(hot(), h);
            lineA     = juce::jmax(lineA, Sp3ctraTheme::kCtlAlphaHot);
            haloA     = juce::jmax(haloA, Sp3ctraTheme::kCtlHaloEdit * h);
            ink.lineW = ink.lineW + (Sp3ctraTheme::kCtlLineEdit - ink.lineW) * h;
        }

        ink.filled = lk.filled();
        ink.line   = lineC.withMultipliedAlpha(lineA * alpha);
        ink.fill   = (ink.filled ? accent.withMultipliedAlpha(alpha) : core());
        ink.halo   = haloA > 0.0f ? accent.withMultipliedAlpha(haloA * alpha)
                                  : juce::Colours::transparentBlack;
        return ink;
    }

    /** Value-fill alpha of a bar / toggle track on the same ladder (the
     *  "how much" surface, not the chrome). */
    inline float fillAlpha(Look lk, bool enabled = true) noexcept
    {
        if (! enabled) return Sp3ctraTheme::kCtlAlphaOff * Sp3ctraTheme::kCtlFillIdle;
        const float e = lk.edit();
        const float base = (lk.state == State::Hover || lk.state == State::Selected)
                             ? Sp3ctraTheme::kCtlFillHot : Sp3ctraTheme::kCtlFillIdle;
        return base + (Sp3ctraTheme::kCtlFillEdit - base) * e;
    }

    /** Text / glyph alpha on the ladder (chip labels, combo arrows, captions
     *  that belong to a control rather than to the display). */
    inline float inkAlpha(Look lk, bool enabled = true) noexcept
    {
        if (! enabled) return Sp3ctraTheme::kCtlAlphaOff;
        switch (lk.state)
        {
            case State::Idle:     break;
            case State::Selected: return Sp3ctraTheme::kCtlAlphaSel;
            case State::Hover:
            case State::Edit:     return Sp3ctraTheme::kCtlAlphaHot;
        }
        return Sp3ctraTheme::kCtlAlphaIdle
             + (Sp3ctraTheme::kCtlAlphaHot - Sp3ctraTheme::kCtlAlphaIdle) * lk.heat;
    }

    /** The SOLID ink of a widget's moving part (a slider thumb, a toggle
     *  knob, a knob pointer): the accent itself, desaturated at rest and
     *  full under the pointer / while edited. Unlike Ink::fill it is never
     *  the dark core — a thumb must stay visible at rest. */
    inline juce::Colour valueInk(Look lk, juce::Colour accent = active(),
                                 bool enabled = true) noexcept
    {
        const bool resting = (lk.state == State::Idle && lk.heat <= 0.0f);
        const juce::Colour base = resting ? restOf(accent) : accent;
        return base.withMultipliedAlpha(enabled ? inkAlpha(lk)
                                                : Sp3ctraTheme::kCtlAlphaOff);
    }

    //══════════════════════════════════════════════════════════════════════════
    // 4. Heat — "this was just changed, and not by my mouse"
    //══════════════════════════════════════════════════════════════════════════

    /** Far enough in the past that heat() is 0 — the "never edited" stamp. */
    constexpr double kNever = -1.0e12;

    inline double nowMs() noexcept { return juce::Time::getMillisecondCounterHiRes(); }

    /** 1 → 0 over Sp3ctraTheme::kCtlGlowMs after `editMs`. */
    inline float heatSince(double editMs, double now = nowMs()) noexcept
    {
        return (float) juce::jlimit(0.0, 1.0,
                                    1.0 - (now - editMs) / Sp3ctraTheme::kCtlGlowMs);
    }

    // — Widget heat: carried by the component itself, so the LookAndFeel can
    //   read it for ANY widget without a per-widget subclass.

    /** Property name of the last remote-edit stamp on a component. */
    inline const juce::Identifier& heatProperty()
    {
        static const juce::Identifier id ("sp3ctraEditMs");
        return id;
    }

    /** Stamp "a controller just moved this control". Repainting is the
     *  caller's business — MidiLearnAttachment drives a short timer while
     *  the glow lasts.
     *
     *  Deliberately NOT raised on every value change: rebinding a page to
     *  another instance rewrites every box, and that is not an edit. A
     *  canvas handle, whose Bound knows the difference, glows on automation
     *  and presets too. */
    inline void touch(juce::Component& c)
    {
        c.getProperties().set(heatProperty(), nowMs());
    }

    /** The remote-edit heat of a widget (0 when it was never touched). */
    inline float heatOf(const juce::Component& c, double now = nowMs()) noexcept
    {
        const auto v = c.getProperties().getWithDefault(heatProperty(), juce::var());
        return v.isVoid() ? 0.0f : heatSince((double) v, now);
    }

    /** The Look of a plain widget: its live mouse state + its remote heat.
     *  ONE reading for every LookAndFeel branch. */
    inline Look lookOf(const juce::Component& c, bool down, bool over,
                       bool selected = false) noexcept
    {
        return stateOf(down, over, selected, heatOf(c));
    }

    //══════════════════════════════════════════════════════════════════════════
    // 5. MidiTouch — the bus that tells a control "a controller just moved you"
    //══════════════════════════════════════════════════════════════════════════
    //
    // MidiMappingEngine publishes the last parameter a controller moved; the
    // editor drains it every tick (MIDI-follow). It forwards it here, and the
    // MidiLearnAttachment of that parameter — the object that already knows
    // (component, paramId) for EVERY mappable control — lights its component.
    // One wire, every mapped control in the interface.

    namespace MidiTouch
    {
        /** Anything that wants to hear "paramId was moved by MIDI". */
        struct Sink
        {
            virtual ~Sink() = default;
            virtual void midiTouched(const juce::String& paramId) = 0;
        };

        inline std::vector<Sink*>& sinks()
        {
            static std::vector<Sink*> v;
            return v;
        }

        inline void add(Sink* s)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            if (s != nullptr) sinks().push_back(s);
        }

        inline void remove(Sink* s)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            auto& v = sinks();
            for (auto it = v.begin(); it != v.end(); ++it)
                if (*it == s) { v.erase(it); return; }
        }

        /** Message thread only — called by the editor's MIDI-follow drain. */
        inline void note(const juce::String& paramId)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            if (paramId.isEmpty()) return;
            // Copy: a sink may add/remove attachments while lighting up.
            const auto snapshot = sinks();
            for (auto* s : snapshot) s->midiTouched(paramId);
        }
    } // namespace MidiTouch

    //══════════════════════════════════════════════════════════════════════════
    // 6. Bound — THE parameter binding of a canvas editor
    //══════════════════════════════════════════════════════════════════════════
    //
    // Every graphic editor used to carry its own `struct Bound` (param +
    // ParameterAttachment + mirrored value + gesture helpers). This is that
    // struct, once, plus the heat: the attachment fires on EVERY change —
    // the editor's own drag, a numeric box below, a MIDI CC, automation, a
    // preset — so a handle knows when it is being edited whoever moved it.

    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float  value  = 0.0f;      ///< mirrored value, in parameter units
        double editMs = kNever;    ///< last change from ANY source

        /** Bind to `id`; `onChange` runs after `value` is refreshed (repaint,
         *  derived state…). The initial update never counts as an edit, so
         *  rebinding a page to another instance lights nothing. */
        void bind(juce::AudioProcessorValueTreeState& apvts, const juce::String& id,
                  std::function<void(float)> onChange = {})
        {
            attach.reset();
            editMs = kNever;
            param  = apvts.getParameter(id);
            jassert(param != nullptr);
            if (param == nullptr) return;

            armed_ = false;
            attach = std::make_unique<juce::ParameterAttachment>(
                *param, [this, cb = std::move(onChange)] (float v)
                {
                    value = v;
                    if (armed_) editMs = nowMs();
                    if (cb) cb(v);
                });
            attach->sendInitialUpdate();
            armed_ = true;
        }

        void reset() { attach.reset(); param = nullptr; editMs = kNever; }

        /** 1 → 0 over kCtlGlowMs after the last change (0 = idle). */
        float heat(double now = nowMs()) const noexcept { return heatSince(editMs, now); }
        bool  lit (double now = nowMs()) const noexcept { return heat(now) > 0.0f; }

        /** Mark an edit that did not go through the attachment (a local,
         *  non-APVTS handle — the EQ's string mode). */
        void touch() noexcept { editMs = nowMs(); }

        // Gesture helpers — a canvas drag is one host-automatable gesture.
        void begin()                 { if (attach) attach->beginGesture(); }
        void end()                   { if (attach) attach->endGesture(); }
        void setGesture(float v)     { if (attach) attach->setValueAsPartOfGesture(v); }
        void setComplete(float v)    { if (attach) attach->setValueAsCompleteGesture(v); }

        /** The double-click of the UI-wide gesture contract
         *  (ui/Sp3ctraGestures.h): back to the parameter's declared default,
         *  as one complete host gesture. */
        void toDefault()
        {
            if (attach && param)
                attach->setValueAsCompleteGesture(param->convertFrom0to1(param->getDefaultValue()));
        }

    private:
        bool armed_ = false;   // sendInitialUpdate() must not glow
    };

    /** Heat of the hottest of several bindings (a handle whose one grip
     *  drives freq + gain + width, a group of paper settings…). */
    inline float heatOfAny(std::initializer_list<const Bound*> bounds,
                           double now = nowMs()) noexcept
    {
        float h = 0.0f;
        for (const auto* b : bounds)
            if (b != nullptr) h = juce::jmax(h, b->heat(now));
        return h;
    }
} // namespace Sp3ctraControls

//══════════════════════════════════════════════════════════════════════════════
// 5. The hand grammar — what the POINTER says about the gesture
//══════════════════════════════════════════════════════════════════════════════
/**
 * Three states, one rule, shared by every canvas editor:
 *
 *   • at rest, over anything that answers   OPEN hand — "there is something
 *                                           here", without pretending to know
 *                                           what you will do with it;
 *   • pressed, on something you HOLD        CLOSED hand — a handle, a line, a
 *                                           frame: it is now in your hand and
 *                                           follows it until you let go;
 *   • pressed, on something you PUSH        INDEX finger — a value dragged up
 *     or pick                               or down, a pictogram chosen: there
 *                                           is nothing to hold, only to press.
 *
 * The cursor therefore changes AT THE CLICK, not on hover: hovering never
 * commits to a gesture. Over a dead zone the plain arrow returns — showing a
 * hand where nothing can be seized would lie.
 *
 * JUCE has no closed hand (DraggingHandCursor is [NSCursor openHandCursor] on
 * macOS, an open one), so the fist below is drawn once and cached. Both live
 * here rather than in one editor: the grammar is the charter's, not a page's.
 */
namespace Sp3ctraCursors
{
    /** At rest over anything that answers. */
    inline juce::MouseCursor openHand() noexcept
    { return juce::MouseCursor::DraggingHandCursor; }

    /** Pressed on something you push, or pick. */
    inline juce::MouseCursor finger() noexcept
    { return juce::MouseCursor::PointingHandCursor; }

    /** Pressed on something you hold. Drawn at 2× and handed to JUCE with the
     *  matching scale, so it stays crisp on a retina display; the hotspot is
     *  in LOGICAL units (juce_MouseCursor_mac.mm passes it through as points). */
    inline const juce::MouseCursor& closedHand()
    {
        static const juce::MouseCursor cursor = []
        {
            constexpr int   kPx    = 32;     // 16 logical points at 2×
            constexpr float kScale = 2.0f;
            juce::Image img(juce::Image::ARGB, kPx, kPx, true);
            juce::Graphics g(img);

            // A fist: the palm, four knuckles over it, the thumb at its side.
            juce::Path fist;
            fist.addRoundedRectangle(7.0f, 12.0f, 18.0f, 16.0f, 5.0f);
            for (int i = 0; i < 4; ++i)
            {
                const float cx = 9.8f + (float) i * 4.9f;
                const float cy = 12.4f - (i == 0 || i == 3 ? 0.0f : 1.0f);
                fist.addEllipse(cx - 3.1f, cy - 3.1f, 6.2f, 6.2f);
            }
            fist.addEllipse(4.4f, 15.0f, 7.0f, 7.6f);          // the thumb

            // Outline first, fill on top: the fill hides the seams the stroke
            // leaves between the overlapping parts, so the silhouette reads as
            // one shape with a clean dark edge on any background.
            g.setColour(juce::Colours::black.withAlpha(0.9f));
            g.strokePath(fist, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
            g.setColour(juce::Colours::white);
            g.fillPath(fist);

            return juce::MouseCursor(img, 8, 8, kScale);       // logical centre
        }();
        return cursor;
    }
}
