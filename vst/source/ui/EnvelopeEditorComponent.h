/**
 * @file EnvelopeEditorComponent.h
 * @brief Integrated ADSR editor — shaped curve + segment bending + compact boxes.
 *
 * One self-contained widget that replaces the old "graph + slider rows" pair on
 * the PITCH / MASK pages.  It owns BOTH the graphical curve and the compact
 * numeric boxes, so the tab no longer needs duplicate sliders.  All controls
 * bind to APVTS parameters → everything stays host-automatable and
 * MIDI-mappable.
 *
 * Chrome (ModuleEditorChrome.h): each lane is a framed graph with its caption
 * top-left and its value boxes in a row BELOW the frame (labels above the
 * boxes).  Every handle is painted by Sp3ctraHandles (lime — Idle / Hover /
 * Drag, plus the lime drag readout); curve, fill, frames and labels keep the
 * module colour (`accent`).
 *
 * ── Alpha lane (always) — "ENVELOPE" ────────────────────────────────────────
 *   • A / D / S / R node handles set time / sustain (drag).
 *   • A / D / R *bend* handles (segment midpoints) set per-segment curvature
 *     by dragging the segment up/down — exactly the shape the DSP applies
 *     (shared lux_env_shape()).  curve ∈ [-1,1], 0 = linear.
 *   • Compact value boxes (Atck / Dcay / Sus / Rel) below the frame — click /
 *     drag to set, hold to reset, double-click to type.  Bound through
 *     SliderAttachment to the same parameters.
 *
 * ── Width lane (MASK only) — "WIDTH" ────────────────────────────────────────
 *   A second framed lane on the SAME time axis with three draggable nodes —
 *   Width @ Attack → Width → Width @ Release — plus its own value boxes below.
 *
 * Display mapping: each A/D/R segment's time axis uses a sqrt(ms/maxMs) "log-ish"
 * mapping so short times stay editable; the sustain plateau has a fixed width.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleEditorChrome.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraBarSlider.h"
#include <memory>
#include <vector>

class EnvelopeEditorComponent : public juce::Component
{
public:
    /** Natural strip heights — tab pages reserve preferredHeight() + a small gap.
     *  Each lane = a ModuleChrome frame + its box row below (kBelowFrameH).
     *  The frame heights keep the plots at their historical pixel heights
     *  (69 px alpha; 62 px alpha + 36 px width) — see kLaneTop / kLanePad in
     *  the .cpp. The width frame is fixed; the alpha frame takes the rest. */
    static constexpr int kAlphaFrameH          = 104;   ///< alpha lane frame, single lane
    static constexpr int kAlphaFrameHWithWidth = 97;   ///< alpha lane frame above a width lane
    static constexpr int kWidthFrameH          = 71;   ///< width lane frame (MASK)
    static constexpr int kPreferredH          = kAlphaFrameH + ModuleChrome::kBelowFrameH;      // 140
    static constexpr int kPreferredHWithWidth = kAlphaFrameHWithWidth + ModuleChrome::kBelowFrameH
                                              + ModuleChrome::kEditorGap
                                              + kWidthFrameH + ModuleChrome::kBelowFrameH;      // 244

    /** decay/sustain IDs are OPTIONAL: pass empty for an AR envelope (rise to
     *  peak then release, no decay/sustain plateau) — used by LuxStral.
     *  Curve IDs drive the per-segment bend handles — OPTIONAL: when omitted
     *  (empty), the segments render linear and no bend handles are shown
     *  (used for audio ADSRs that have no curvature parameters).
     *  widthBase/widthAttack/widthRelease IDs are optional too (MASK only):
     *  when provided, the editable width lane is shown. */
    EnvelopeEditorComponent(juce::AudioProcessorValueTreeState& apvts,
                            juce::Colour accentColour,
                            const juce::String& attackParamId,
                            const juce::String& decayParamId,
                            const juce::String& sustainParamId,
                            const juce::String& releaseParamId,
                            const juce::String& attackCurveParamId  = {},
                            const juce::String& decayCurveParamId   = {},
                            const juce::String& releaseCurveParamId = {},
                            const juce::String& widthBaseParamId    = {},
                            const juce::String& widthAttackParamId  = {},
                            const juce::String& widthReleaseParamId = {});
    ~EnvelopeEditorComponent() override;

    /** Optional MIDI-learn wiring — call BEFORE setParamIds; the right-click
     *  popups on the value boxes then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** Rebind every handle/box to another parameter set — the per-instance
     *  rebind path for the contextual pages (same id semantics as the
     *  constructor: empty decay/sustain → AR, empty curves → linear, empty
     *  width ids → no width lane). */
    void setParamIds(const juce::String& attackParamId,
                     const juce::String& decayParamId,
                     const juce::String& sustainParamId,
                     const juce::String& releaseParamId,
                     const juce::String& attackCurveParamId  = {},
                     const juce::String& decayCurveParamId   = {},
                     const juce::String& releaseCurveParamId = {},
                     const juce::String& widthBaseParamId    = {},
                     const juce::String& widthAttackParamId  = {},
                     const juce::String& widthReleaseParamId = {});

    /** Natural height for this instance (depends on whether the width lane exists). */
    int preferredHeight() const noexcept
    { return hasWidth ? kPreferredHWithWidth : kPreferredH; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    enum class Handle { None,
                        Attack, Decay, Sustain, Release,   // alpha nodes
                        BendA, BendD, BendR,               // alpha segment curvature
                        WAttack, WBase, WRelease };        // width-lane nodes

    /** Screen-space layout shared by both lanes (identical time axis). */
    struct Geometry
    {
        juce::Rectangle<float> alpha, width;
        float segMaxW = 0, susW = 0;
        float xStart = 0, xA = 0, xD = 0, xSusEnd = 0, xR = 0;
        float aYBase = 0, aYPeak = 0, aYSus = 0;   // alpha lane
        float wYAtk = 0, wYBase = 0, wYRel = 0;    // width lane (mask)
        bool  valid = false;
    };

    Geometry computeGeometry() const;
    juce::Point<float> handlePos(Handle h, const Geometry& geo) const;
    Handle handleAt(juce::Point<float> p, const Geometry& geo) const;
    void   updateCursor(Handle h);
    void   beginHandleGesture(Handle h);
    void   endHandleGesture(Handle h);

    // ── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    /** What a node drives: the double-click resets it, the long press types it. */
    Sp3ctraGestures::BoundList boundsOf(Handle h);
    /** Long press on a node: the drag gesture closes, the bubble opens. */
    void holdToType();
    Sp3ctraGestures::Hold hold_;
    void   applyDrag(Handle h, juce::Point<float> p, const Geometry& geo);

    /** Curve solved from a desired shape value at phase 0.5 (segment bending). */
    static float curveFromHalfValue(float targetS) noexcept;

    /** sqrt time mapping helpers (segment-local). */
    static float timeToX(float ms, float maxMs, float segMaxW) noexcept;
    static float xToTime(float dx, float maxMs, float segMaxW) noexcept;

    /** Append a shaped segment (phase 0→1) to a path in screen space. */
    static void appendShapedSegment(juce::Path& p, float x0, float x1,
                                    float v0, float v1, float curve,
                                    float yTop, float yBot);

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    bool hasWidth = false;
    bool hasCurve = false;   ///< per-segment bend handles shown only when curve IDs given
    bool isAR     = false;   ///< AR mode (no decay/sustain): rise to peak then release

    // ── Bound parameters (ParameterAttachment manages begin/endGesture) ──────
    /** The shared parameter binding (ui/Sp3ctraControls.h — attachment,
     *  mirrored value and EDIT HEAT) plus this editor's cached range: the
     *  nodes map ms / px onto the plot with it. */
    struct Bound : Sp3ctraControls::Bound
    {
        float min = 0.0f, max = 1.0f;
    };
    Bound a, d, s, r;          // attack ms / decay ms / sustain lvl / release ms
    Bound aCurve, dCurve, rCurve;
    Bound wBase, wAtk, wRel;   // mask widths (px)

    void bind(Bound& b, const juce::String& id, bool readRange = true);

    /** Remote-edit heat of the parameter a node drives — a box below, a MIDI
     *  CC or automation lights the node exactly like a drag. */
    float handleHeat(Handle h) const noexcept;

    // ── Compact numeric boxes (SliderAttachment) ────────────────────────────
    Sp3ctraBarSlider boxA, boxD, boxS, boxR;
    Sp3ctraBarSlider boxWAtk, boxW, boxWRel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        boxAAtt, boxDAtt, boxSAtt, boxRAtt, boxWAtkAtt, boxWAtt, boxWRelAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;
    void initBox(Sp3ctraBarSlider& box, const juce::String& paramId,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att);

    // Lane frames (set in resized(), consumed by computeGeometry()/paint());
    // the plots live inside ModuleChrome::graphOf(frame).
    juce::Rectangle<float> alphaFrame_, widthFrame_;

    // ── Interaction state ────────────────────────────────────────────────────
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopeEditorComponent)
};
