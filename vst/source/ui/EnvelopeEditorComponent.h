/**
 * @file EnvelopeEditorComponent.h
 * @brief M5 — draggable ADSR envelope editor (~96 px tall).
 *
 * Sits at the top of the PITCH / MASK PLAY pages and binds the module's four
 * envelope parameters (attack / decay / sustain / release).  The numeric
 * slider rows below remain the fallback — both stay in sync automatically
 * through the APVTS parameters.
 *
 * Display mapping: the time axis of each A/D/R segment uses a "log-ish"
 * sqrt(ms / maxMs) mapping so short times stay editable; the sustain plateau
 * has a fixed display width.
 *
 * Handles (hover highlights + cursor change, host-correct gestures through
 * juce::ParameterAttachment beginGesture/endGesture):
 *   • A node — x sets the attack time              (y pinned to peak)
 *   • D node — x sets the decay time               (y locked to sustain)
 *   • S node — y sets the sustain level            (mid-plateau)
 *   • R node — x sets the release time             (y pinned to zero)
 * A small value readout ("A 12 ms") follows the handle while dragging.
 *
 * MASK extra: a read-only dashed curve (50 % alpha) overlays the width-bloom
 * trajectory — starts at widthAttackPx, lerps to width over the D segment,
 * holds, then goes to widthReleasePx over R.  Levels are normalised through
 * the width parameters' own 8..8192 px range.  Pure display, refreshed by a
 * 4 Hz poll of the current parameter values.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../UITheme.h"
#include <memory>

class EnvelopeEditorComponent : public juce::Component,
                                private juce::Timer
{
public:
    /** Natural strip height — tab pages reserve this + a small gap. */
    static constexpr int kPreferredH = 96;

    /** widthBase/widthAttack/widthRelease IDs are optional (MASK only):
     *  when provided, the read-only width-bloom curve is overlaid. */
    EnvelopeEditorComponent(juce::AudioProcessorValueTreeState& apvts,
                            juce::Colour accentColour,
                            const juce::String& attackParamId,
                            const juce::String& decayParamId,
                            const juce::String& sustainParamId,
                            const juce::String& releaseParamId,
                            const juce::String& widthBaseParamId    = {},
                            const juce::String& widthAttackParamId  = {},
                            const juce::String& widthReleaseParamId = {});
    ~EnvelopeEditorComponent() override;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    enum class Handle { None, Attack, Decay, Sustain, Release };

    /** Screen-space layout of the envelope for the current values. */
    struct Geometry
    {
        juce::Rectangle<float> inner;
        float segMaxW = 0, susW = 0;
        float xStart = 0, xA = 0, xD = 0, xSusEnd = 0, xR = 0;
        float yBase = 0, yPeak = 0, ySus = 0;
        bool  valid = false;
    };

    Geometry computeGeometry() const;
    juce::Point<float> handlePos(Handle h, const Geometry& geo) const;
    Handle handleAt(juce::Point<float> p, const Geometry& geo) const;
    void   updateCursor(Handle h);

    void timerCallback() override;          // 4 Hz width-curve poll (MASK only)

    /** sqrt time mapping helpers (segment-local). */
    static float timeToX(float ms, float maxMs, float segMaxW) noexcept;
    static float xToTime(float dx, float maxMs, float segMaxW) noexcept;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;

    // ── Bound parameters (attachments manage begin/endChangeGesture) ─────────
    juce::RangedAudioParameter* aParam = nullptr;
    juce::RangedAudioParameter* dParam = nullptr;
    juce::RangedAudioParameter* sParam = nullptr;
    juce::RangedAudioParameter* rParam = nullptr;
    std::unique_ptr<juce::ParameterAttachment> aAttach, dAttach, sAttach, rAttach;

    // Cached denormalised values (updated by the attachments' callbacks)
    float aMs = 10.0f, dMs = 50.0f, sLvl = 1.0f, rMs = 100.0f;
    float aMin = 0.5f, aMax = 5000.0f;
    float dMin = 0.5f, dMax = 5000.0f;
    float rMin = 0.5f, rMax = 5000.0f;

    // ── Optional MASK width-bloom curve (read-only display) ──────────────────
    juce::RangedAudioParameter* wBaseParam = nullptr;
    juce::RangedAudioParameter* wAtkParam  = nullptr;
    juce::RangedAudioParameter* wRelParam  = nullptr;
    float wBase = 256.0f, wAtk = 1024.0f, wRel = 1024.0f;

    // ── Interaction state ─────────────────────────────────────────────────────
    Handle hovered  { Handle::None };
    Handle dragging { Handle::None };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopeEditorComponent)
};
