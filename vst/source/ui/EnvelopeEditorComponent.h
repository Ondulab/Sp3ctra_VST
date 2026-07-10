/**
 * @file EnvelopeEditorComponent.h
 * @brief Integrated ADSR editor — shaped curve + segment bending + compact boxes.
 *
 * One self-contained widget that replaces the old "graph + slider rows" pair on
 * the PITCH / MASK pages.  It owns BOTH the graphical curve and the compact
 * numeric boxes underneath, so the tab no longer needs duplicate sliders.  All
 * controls bind to APVTS parameters → everything stays host-automatable and
 * MIDI-mappable.
 *
 * ── Alpha lane (always) ─────────────────────────────────────────────────────
 *   • A / D / S / R node handles set time / sustain (drag).
 *   • A / D / R *bend* handles (segment midpoints) set per-segment curvature
 *     by dragging the segment up/down — exactly the shape the DSP applies
 *     (shared lux_env_shape()).  curve ∈ [-1,1], 0 = linear.
 *   • Compact value boxes (Atck / Dcay / Sus / Rel) below — drag or double-click
 *     to type.  Bound through SliderAttachment to the same parameters.
 *
 * ── Width lane (MASK only) ──────────────────────────────────────────────────
 *   A second editable lane on the SAME time axis with three draggable nodes —
 *   Width @ Attack → Width → Width @ Release — plus its own value boxes.  This
 *   replaces the old read-only dashed overlay and the separate width sliders.
 *
 * Display mapping: each A/D/R segment's time axis uses a sqrt(ms/maxMs) "log-ish"
 * mapping so short times stay editable; the sustain plateau has a fixed width.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include <memory>
#include <vector>

class EnvelopeEditorComponent : public juce::Component
{
public:
    /** Natural strip heights — tab pages reserve preferredHeight() + a small gap. */
    static constexpr int kPreferredH          = 124; // alpha lane + box row
    static constexpr int kPreferredHWithWidth = 196; // + width lane + width box row

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
    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float value = 0.0f, min = 0.0f, max = 1.0f;
    };
    Bound a, d, s, r;          // attack ms / decay ms / sustain lvl / release ms
    Bound aCurve, dCurve, rCurve;
    Bound wBase, wAtk, wRel;   // mask widths (px)

    void bind(Bound& b, const juce::String& id, bool readRange = true);

    // ── Compact numeric boxes (SliderAttachment) ────────────────────────────
    juce::Slider boxA, boxD, boxS, boxR;
    juce::Slider boxWAtk, boxW, boxWRel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        boxAAtt, boxDAtt, boxSAtt, boxRAtt, boxWAtkAtt, boxWAtt, boxWRelAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;
    void initBox(juce::Slider& box, const juce::String& paramId,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att);

    // Lane rectangles (set in resized(), consumed by computeGeometry()/paint()).
    juce::Rectangle<float> alphaLaneRect_, widthLaneRect_;

    // ── Interaction state ────────────────────────────────────────────────────
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopeEditorComponent)
};
