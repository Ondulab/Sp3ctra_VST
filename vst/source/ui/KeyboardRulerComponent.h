/**
 * @file KeyboardRulerComponent.h
 * @brief M5 — 26-px piano-ruler strip placed directly under zone 1.
 *
 * Visible only while the selected chain block is PITCH or MASK.  The strip
 * shares the visualizer's x-extent so its pixel columns line up 1:1 with the
 * CIS image above it, mapped through the SAME pixel↔note formulas as the C
 * engines (lux_pitch.c / lux_mask.c):
 *
 *   pps  = (coupling == LUXSTRAL) ? pixel_count / (num_octaves * 12)
 *                                 : free_pixels_per_semitone
 *   MASK : spot centre px = pixel_count*0.5 + (note - reference_note) * pps
 *   PITCH: shift px       = (note - reference_note) * pps
 *          (marker shown at pixel_count*0.5 + shift)
 *
 * Drawn elements:
 *   • piano white/black key pattern (key boundaries at half-semitone pixels),
 *     octave labels (C2, C3, …) when space allows — all subdued colours;
 *   • reference-note marker (triangle + line) in the module accent colour
 *     (always at the image centre, by construction of the mapping);
 *   • live voice overlay at 30 Hz: per active voice, a translucent accent
 *     band (MASK) or a vertical marker (PITCH) whose opacity follows the
 *     ADSR envelope level, plus the note name when there is room.
 *
 * Interaction: ALT- or SHIFT-click on a key sets the module's reference-note
 * parameter (host-notified gesture).  Plain clicks do nothing — this strip is
 * a display instrument.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include <vector>

class KeyboardRulerComponent : public juce::Component,
                               public juce::SettableTooltipClient,
                               private juce::Timer
{
public:
    /** Which engine the ruler reflects (mapping + voices + accent colour). */
    enum class Module { Pitch, Mask };

    /** Strip height — the editor reserves this many px under zone 1. */
    static constexpr int kPreferredH = 26;

    explicit KeyboardRulerComponent(Sp3ctraAudioProcessor& p);
    ~KeyboardRulerComponent() override;

    void setModule(Module m);
    Module getModule() const noexcept { return module; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;          // 30 Hz overlay refresh
    void refreshSnapshot();                 // engine state → POD snapshot
    void rebuildStaticOverlays();           // octave labels + ref marker path

    float pixelToX(float px) const noexcept;
    float xToPixel(float x)  const noexcept;

    Sp3ctraAudioProcessor& processor;
    Module       module { Module::Pitch };
    juce::Colour accent { 0xffe06bb8 };     // pink (PITCH) / teal (MASK)

    // ── Mapping snapshot (mirrors lux_pitch.c / lux_mask.c) ──────────────────
    float pixelCount { 3456.0f };
    float pps        { 24.0f };             // pixels per semitone
    int   refNote    { 57 };                // engine reference note (MIDI)

    // ── Voice snapshot, rebuilt at 30 Hz (POD → cheap change detection) ──────
    struct VoiceSnap
    {
        float posPx;                        // marker / band centre (CIS pixels)
        float env;                          // ADSR level [0, 1]
        float widthPx;                      // MASK band width (pixels)
        int   note;                         // engine note (post octave offset)
    };
    static constexpr int kMaxVoices = 10;   // == LUX_PITCH/MASK_MAX_VOICES
    VoiceSnap voices[kMaxVoices] {};
    int       numVoices { 0 };

    // Note-name labels cached outside paint (avoid allocations in paint)
    juce::String voiceLabels[kMaxVoices];
    int          voiceLabelNotes[kMaxVoices];

    // ── Static overlays cached when the mapping / size changes ───────────────
    struct OctLabel { float x; juce::String text; };
    std::vector<OctLabel> octLabels;
    juce::Path refMarker;

    // Change detectors
    float lastPps        { -1.0f };
    int   lastRefNote    { -1 };
    float lastPixelCount { -1.0f };
    int   lastWidth      { -1 };
    VoiceSnap lastVoices[kMaxVoices] {};
    int       lastNumVoices { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeyboardRulerComponent)
};
