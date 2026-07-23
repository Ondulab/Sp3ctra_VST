#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "../luxsampler/LuxSampler.h"
#include "SlotSpectralEditorComponent.h"
#include "../image/ScoreEqComponent.h"
#include "../midi/MidiLearnAttachment.h"   // right-click MIDI-Learn on play controls

class Sp3ctraAudioProcessor;

/**
 * @brief Compact pictogram button for one loop mode (radio-style).
 *
 * Reuses the SCORE transport-button visual language (rounded dark cell, accent
 * glow when active) so the SAMPLER loop controls read at a glance — the old
 * truncated "NONE / LOOP / INV / PING" text buttons were unreadable. Unlike
 * SCORE (whose play button is larger than its loop/reverse pictograms), all
 * four loop buttons here share ONE size.
 *
 * Glyphs:
 *   None     → straight right arrow  (──▶)  : play once, no repeat
 *   Loop     → racetrack loop, arrow left   : repeat forward
 *   Inverse  → racetrack loop mirrored      : repeat backward
 *   PingPong → double-headed arrow (◀─▶)    : bounce forward / backward
 *
 * These are mutually-exclusive radio buttons, not JUCE toggles: the active one
 * is pushed in via setActive(); the click is wired through Button::onClick.
 */
class LoopModeButton : public juce::Button
{
public:
    enum class Glyph { None, Loop, Inverse, PingPong };

    LoopModeButton() : juce::Button("loopMode") {}

    void setGlyph(Glyph g) noexcept { glyph = g; }

    void setActive(bool a)
    {
        if (a == active) return;
        active = a;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto b  = getLocalBounds().toFloat().reduced(1.f);
        const bool on = active && isEnabled();
        const juce::Colour accent(0xffcc88ff); // SAMPLER / SLOT identity (purple)

        const juce::Colour bg = on ? accent.withAlpha(0.22f) : juce::Colour(0xff222230);
        g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
        g.fillRoundedRectangle(b, 3.f);
        g.setColour(on ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
        g.drawRoundedRectangle(b, 3.f, 1.f);

        // Icon region: centred + padded. Loop glyphs stay square; the arrows
        // are allowed a wider aspect so they don't look cramped on a wide cell.
        const auto inner = b.reduced(2.0f);
        const juce::Colour fg = on ? accent
                                   : juce::Colour(isEnabled() ? 0xff9aa6ba : 0xff555a62);
        switch (glyph)
        {
            case Glyph::None:     drawArrowGlyph   (g, centred(inner, 1.6f), fg);        break;
            case Glyph::Loop:     drawLoopGlyph    (g, centred(inner, 1.0f), fg, false); break;
            case Glyph::Inverse:  drawLoopGlyph    (g, centred(inner, 1.0f), fg, true);  break;
            case Glyph::PingPong: drawPingPongGlyph(g, centred(inner, 1.8f), fg);        break;
        }
    }

private:
    /** Centred sub-rect of @p a with width capped to @p aspect × its height. */
    static juce::Rectangle<float> centred(juce::Rectangle<float> a, float aspect)
    {
        const float h = a.getHeight();
        const float w = juce::jmin(a.getWidth(), h * aspect);
        return juce::Rectangle<float>(w, h).withCentre(a.getCentre());
    }

    /** Straight right arrow ──▶ : "play once, no loop". */
    static void drawArrowGlyph(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour col)
    {
        const float w = r.getWidth(), h = r.getHeight();
        const float th = juce::jmax(2.0f, h * 0.15f);
        const float cy = r.getCentreY();
        const float aW = w * 0.34f;                 // arrowhead length
        const float aH = h * 0.34f;                 // arrowhead half-height
        const float tipX  = r.getRight() - w * 0.02f;
        const float backX = tipX - aW;

        juce::Path shaft;
        shaft.startNewSubPath(r.getX() + w * 0.04f, cy);
        shaft.lineTo(backX, cy);
        g.setColour(col);
        g.strokePath(shaft, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        juce::Path head;
        head.addTriangle(tipX, cy, backX, cy - aH, backX, cy + aH);
        g.fillPath(head);
    }

    /** Double-headed arrow ◀──▶ : bounce forward / backward. */
    static void drawPingPongGlyph(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour col)
    {
        const float w = r.getWidth(), h = r.getHeight();
        const float th = juce::jmax(2.0f, h * 0.15f);
        const float cy = r.getCentreY();
        const float aW = w * 0.26f;                 // arrowhead length
        const float aH = h * 0.34f;                 // arrowhead half-height
        const float lTip = r.getX()     + w * 0.02f;
        const float rTip = r.getRight() - w * 0.02f;
        const float lBack = lTip + aW;
        const float rBack = rTip - aW;

        juce::Path shaft;
        shaft.startNewSubPath(lBack, cy);
        shaft.lineTo(rBack, cy);
        g.setColour(col);
        g.strokePath(shaft, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        juce::Path heads;
        heads.addTriangle(lTip, cy, lBack, cy - aH, lBack, cy + aH);
        heads.addTriangle(rTip, cy, rBack, cy - aH, rBack, cy + aH);
        g.fillPath(heads);
    }

    /** Stadium (racetrack) loop with a capping arrow — reads as "repeat".
     *  @p reversed mirrors it horizontally so the arrow points the other way
     *  (forward loop → arrow left; inverse loop → arrow right).
     *  Adapted from ScoreGenTabComponent::ScoreIconToggle::drawLoopGlyph. */
    static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                              juce::Colour col, bool reversed)
    {
        const float h  = r.getHeight();
        const float th = juce::jmax(2.0f, h * 0.12f);   // slightly thinner ring

        // Ring built symmetric about r's centre → centred by construction.
        const float ringH  = h * 0.64f;
        const float L = r.getX() + th * 0.6f;
        const float R = r.getRight() - th * 0.6f;
        const float T = r.getCentreY() - ringH * 0.5f;
        const float B = r.getCentreY() + ringH * 0.5f;
        const float radius = (B - T) * 0.5f;
        const float midY   = (T + B) * 0.5f;
        const float topLx  = L + radius;
        const float topRx  = R - radius;
        const float gx0    = juce::jmap(0.34f, topLx, topRx);
        const float gx1    = juce::jmap(0.66f, topLx, topRx);

        juce::Path loop;
        loop.startNewSubPath(gx1, T);
        loop.lineTo(topRx, T);
        loop.addCentredArc(topRx, midY, radius, radius, 0.0f,
                           0.0f, juce::MathConstants<float>::pi, false);
        loop.lineTo(topLx, B);
        loop.addCentredArc(topLx, midY, radius, radius, 0.0f,
                           juce::MathConstants<float>::pi,
                           juce::MathConstants<float>::twoPi, false);
        loop.lineTo(gx0, T);

        const float aH     = radius * 0.85f;            // slightly larger arrow head
        const float aTipX  = gx0 - th * 0.25f;
        const float aBackX = gx1 + th * 0.25f;
        juce::Path arrow;
        arrow.addTriangle(aTipX, T, aBackX, T - aH, aBackX, T + aH);

        // Centre on the RING ONLY (stroke-expanded) so the arrow's overshoot
        // never biases placement; the arrow rides along with the same offset.
        const auto ringBounds = loop.getBounds().expanded(th * 0.5f);
        const auto offset = r.getCentre() - ringBounds.getCentre();
        const auto move = juce::AffineTransform::translation(offset.x, offset.y);
        loop.applyTransform(move);
        arrow.applyTransform(move);

        // Inverse: mirror about r's vertical centre (x' = 2·cx − x).
        if (reversed)
        {
            const auto flip = juce::AffineTransform::scale(-1.0f, 1.0f)
                                  .translated(r.getCentreX() * 2.0f, 0.0f);
            loop.applyTransform(flip);
            arrow.applyTransform(flip);
        }

        g.setColour(col);
        g.strokePath(loop, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.fillPath(arrow);
    }

    Glyph glyph  = Glyph::Loop;
    bool  active = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopModeButton)
};

/**
 * @brief REC / PLAY transport button with a selectable momentary (hold) mode.
 *
 * In TOGGLE mode (@c momentary == false) it is a plain TextButton: a click fires
 * onClick (which flips record / play on↔off). In MOMENTARY mode a LEFT press
 * fires @c onPress and its matching release fires @c onRelease (press-to-start /
 * release-to-stop), and onClick is suppressed. A RIGHT press is always passed to
 * the base class so the right-click MIDI-Learn popup keeps working in both modes.
 * The mode itself is driven by the per-engine APVTS param (see samplerRecMode /
 * samplerPlayMode) and refreshed each timer tick.
 */
class TransportButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    std::function<void()> onPress;    // momentary: left-press edge
    std::function<void()> onRelease;  // momentary: matching release edge
    bool momentary = false;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (momentary && e.mods.isLeftButtonDown())
        {
            held_ = true;
            if (onPress) onPress();
            return;   // swallow — onClick must not also fire in momentary mode
        }
        juce::TextButton::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (held_)   // only release what WE pressed (mode may have flipped since)
        {
            held_ = false;
            if (onRelease) onRelease();
            return;
        }
        juce::TextButton::mouseUp(e);
    }

private:
    bool held_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportButton)
};

/**
 * @brief Edit panel for the currently selected LuxSampler slot.
 *
 * Layout — parameters in TWO columns on top, one merged editor below:
 *   Left column  : [REC][PLAY][CLEAR] · Speed · Loop · Floor
 *   Right column : [CROP][SAVE][LOAD] · Resume · Overdub · Curve · Power
 * (IMG removed — the bank level now lives in the grid's per-bank mixer.)
 *   Bottom       : SlotSpectralEditorComponent — the authentic captured image
 *                  with the time handles (Start/End/fades/playhead) AND the
 *                  frequency EQ curve overlaid on one surface.
 *
 * Control values are written directly to LuxSampler per-slot play params (Non-RT).
 * Values are refreshed from LuxSampler on slot switch (setSelectedSlot).
 * Button states are refreshed at ~33 Hz via internal Timer.
 */
class SlotEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit SlotEditorComponent(Sp3ctraAudioProcessor& proc);
    ~SlotEditorComponent() override;

    /** Switch to editing a different slot (0–11). Refreshes all controls. */
    void setSelectedSlot(int idx);
    int  getSelectedSlot() const noexcept { return selectedSlot; }

    /** Bind this editor (and its timeline) to sampler engine 0 (A) or 1 (B).
     *  Purges the engine's stale MIDI pulses before acting on them. */
    void setSamplerIndex(int i);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Pull slider / resume values from LuxSampler and update UI silently. */
    void refreshSliderValues();

    /** Update loop-mode button highlight to reflect current LoopMode. */
    void refreshLoopButtons();

    /** Apply LoopMode m to the selected slot and refresh button highlights. */
    void applyLoopMode(LoopMode m);

    /** (Re)create every right-click MIDI-Learn attachment so each play control /
     *  action button targets the CURRENT (engine, selected-slot) — "fixed slot
     *  per button". Called from setSelectedSlot / setSamplerIndex, like the
     *  SliderAttachments elsewhere. */
    void rebindMidiLearn();

    /** Run the SAVE action for one specific slot (timestamped .fslot + optional
     *  image). Shared by the SAVE button and the MIDI-mapped SAVE trigger. */
    void saveSlotToDisk(int slot);

    /** Drain per-slot REC/PLAY/SAVE MIDI action pulses for the bound engine and
     *  run them (message thread). Called from timerCallback. */
    void drainMidiActionPulses();

    Sp3ctraAudioProcessor& processor;
    int  selectedSlot  = 0;
    int  samplerIndex_ = 0;   // 0 = engine A, 1 = engine B
    bool blinkOn       = false;
    bool prevRecording_ = false; // detect record-stop to refresh the curve backdrop
    bool prevHasContent_ = false; // detect external CLEAR to reset the freq-curve view

    // ── Image + time + fades editor (middle) and SCORE-style EQ panel (bottom) ──
    SlotSpectralEditorComponent spectralEditor;
    ScoreEqComponent            eqEditor { juce::Colour(0xffcc88ff) };
    bool                        suppressEqPush_ = false; // guard during refresh

    /** Reload the EQ curve into eqEditor from the current slot (silent). */
    void refreshFreqCurve();

    // ── Action buttons ────────────────────────────────────────────────────────
    // REC / PLAY support Toggle (click) OR Momentary (hold) via TransportButton;
    // the mode is refreshed each timer tick from the per-engine APVTS param.
    TransportButton  recBtn   { "REC" };
    TransportButton  playBtn  { "PLAY" };
    juce::TextButton clearBtn { "CLEAR" };
    juce::TextButton cropBtn  { "CROP" };   // destructive trim to [start,end]
    juce::TextButton saveBtn  { "SAVE" };
    juce::TextButton loadBtn  { "LOAD" };

    // ── File chooser for LOAD (Non-RT, message thread) ────────────────────────
    std::unique_ptr<juce::FileChooser> fileChooser;

    /** Build the destination directory for SAVE.
     *  Uses the user's configured Sampler Output Dir if set, otherwise
     *  falls back to ~/Documents. Creates the directory if needed. */
    juce::File resolveSaveDirectory() const;

    // ── Labels ────────────────────────────────────────────────────────────────
    // (IMG brightness slider removed — the bank level fader in the grid's
    //  per-bank mixer drives the same engine param, inverted.)
    // Pre-EQ material floor (0 % = off … 100 % = total white mask).
    juce::Label  floorLabel { {}, "Floor" };
    juce::Slider floorSlider;
    juce::Label speedLabel { {}, "Speed" };
    juce::Label loopLabel  { {}, "Loop" };

    // ── Sliders ───────────────────────────────────────────────────────────────
    juce::Slider speedSlider; // 0.01–32.0×; skewed so 1.0× sits at centre position

    // ── Loop mode (4 radio-style icon buttons: NONE / LOOP / INVERSE / PINGPONG) ──
    LoopModeButton loopBtns[4];

    // ── Resume mode toggle ────────────────────────────────────────────────────
    juce::ToggleButton resumeToggle { "Resume from last position" };

    // ── Overdub / extend toggle (engine-wide) ─────────────────────────────────
    // When ON, REC on a non-empty slot appends after the existing take instead
    // of erasing it (tape-style "continue recording").
    juce::ToggleButton overdubToggle { "Overdub (extend REC)" };

    // ── Fade info labels — thin strip UNDER the image view ────────────────────
    // The fade curves are edited directly ON the image (drag the end handle
    // for the length, the mid-curve handle for the shape — see
    // SlotSpectralEditorComponent); these labels mirror the live type/power.
    // Right-click = MIDI-learn of the fade POWER.
    juce::Label fadeInInfo_, fadeOutInfo_;

    /** Refresh the fade info labels from the engine (type · power). */
    void refreshFadeInfo();

    // ── Unified MIDI-Learn (right-click any play control / action button) ──────
    // Recreated on every slot / engine rebind so the synthetic target ids track
    // the shown (engine, slot). See SamplerMidiTargets for the id scheme.
    std::vector<std::unique_ptr<MidiLearnAttachment>> midiLearn_;
    uint32_t lastValueTouchGen_ = 0;   // MIDI value-touch edge tracking (UI refresh)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotEditorComponent)
};
