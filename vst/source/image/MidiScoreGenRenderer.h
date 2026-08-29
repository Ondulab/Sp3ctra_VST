/**
 * @file MidiScoreGenRenderer.h
 * @brief MIDI SCORE — renders a standard MIDI file into a printable /
 *        playable graphical score (sibling of SCORE and TIMBRE).
 *
 * Port of the offline tool Sp3ctra-Midi-to-Score-Gen (midi_to_sp3ctra.py):
 * every MIDI note becomes a horizontal bar (X = time at the writing speed,
 * Y = pitch on the LOG frequency axis, ink = velocity). Upgrades over the
 * Python script:
 *   - polyphony and multi-track files are fully supported (the script was
 *     monophonic, first track only);
 *   - each note is drawn as the PARTIAL STACK of a timbre instead of a bare
 *     fundamental line: the per-voice timbre model is TimbreGenRenderer's
 *     (same presets — Square, Brass, Bell…, same envelope semantics), so a
 *     printed piece carries the instrument's spectrum, not just its pitch;
 *   - a time→pan automation line (MidiScoreSettings::panPoints) tints the
 *     ink with SCORE's stereo convention (left = red, right = blue,
 *     centre = grey/mono) so the piece can wander between the ears;
 *   - a graphic EQ PER VOICE (MidiScoreSettings::voiceEq) shifts each
 *     partial's level at its own frequency, so a voice can be brightened or
 *     tamed against the others without touching the rest of the score.
 *
 * Voices: notes are grouped into up to kMaxVoices voices (MIDI tracks when
 * the file is multi-track, MIDI channels otherwise); each voice owns one
 * timbregen::TimbreSlotParams (its midiNote field is ignored — the
 * fundamental comes from each note).
 *
 * Encoding conventions are identical to SCORE / TIMBRE so a print plays in
 * tune through the unchanged reader:
 *   - band height  = SCORE_CIS_HEIGHT_MM (219.456 mm — the CIS sensor span)
 *   - LOG frequency axis over [minFreq..maxFreq] (equal space per octave)
 *   - inverted greyscale, dB-linear over dynamicRangeDB (white = silence)
 *   - export via scoregen::exportImage (PNG pHYs / JPEG JFIF density stamp)
 *
 * A piece is usually longer than one page, so there are two render paths:
 *   - renderStrip(): the band alone over an arbitrary time window, with
 *     independent time (px/s) and frequency (DPI) resolutions — feeds the
 *     shared score player (playback) and the UI preview;
 *   - renderPage(): one A4 portrait page (same margins as SCORE/TIMBRE) for
 *     print export; long pieces paginate via pageCount()/pageSeconds().
 *
 * Pure functions, no UI, no globals — safe to call from any thread.
 */
#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include "TimbreGenRenderer.h"   // timbregen partial model + scoregen::RenderResult
#include "../processing/shape_eq.h"   // typed-handle EQ evaluator (ShapeEqComponent)

namespace midiscoregen
{

constexpr int kMaxVoices = 6;   ///< voice (track/channel) timbre slots

//==============================================================================
/** One extracted note, already assigned to a voice. Times are absolute
 *  seconds from the start of the piece (tempo map applied). */
struct NoteEvent
{
    int    note     = 60;    ///< MIDI note number (fundamental)
    int    velocity = 100;   ///< 1..127
    double startSec = 0.0;
    double endSec   = 0.0;
    int    voice    = 0;     ///< 0..kMaxVoices-1

    /** Continuous curves captured with the note (Sp3ctra MPE takes, where one
     *  channel carries one voice at a time). Piecewise-constant breakpoints,
     *  times in seconds RELATIVE to startSec:
     *   - bendPts:  pitch offset in CENTS (per-channel pitch wheel, ±2 st —
     *     the writer's RPN convention; general files are read with the same
     *     MIDI-default range);
     *   - levelPts: 0..127 level (channel pressure / CC11) whose ink dB
     *     REPLACES the note-on velocity's from that breakpoint on.
     *  Empty on plain note files — the renderer then draws flat bars. */
    std::vector<std::pair<float, float>> bendPts;
    std::vector<std::pair<float, float>> levelPts;
};

/** Parsed MIDI file — everything the renderer and the UI need. */
struct MidiScoreData
{
    bool         ok = false;
    juce::String error;                   ///< set when !ok
    juce::String sourcePath;              ///< the .mid file this came from
    /** True when the file is a Sp3ctra MIDI TAP take (track-name marker):
     *  every channel is then folded into ONE voice — an MPE take spreads its
     *  voices over 15 channels that mean allocation, not musical parts. */
    bool         sp3ctraCapture = false;
    std::vector<NoteEvent> notes;         ///< sorted by startSec
    double       durationSec = 0.0;       ///< last note-off
    int          numVoices   = 0;         ///< voices actually used (≤ kMaxVoices)
    std::array<juce::String, kMaxVoices> voiceNames;      ///< "Track 2 – Flute", "Ch 10"…
    std::array<int, kMaxVoices>          voiceNoteCount {};
    juce::String log;                     ///< parse summary / warnings
};

/** Reads a .mid file: tempo map applied (timestamps in seconds), note-on/off
 *  pairs matched, notes grouped into voices (tracks when the file has several
 *  note-bearing tracks, MIDI channels otherwise; overflow beyond kMaxVoices
 *  merges into the last voice with a warning). Never throws; returns ok=false
 *  with a message on any failure. */
MidiScoreData parseMidiFile(const juce::File& file);

//==============================================================================
/** One breakpoint of the pan automation line: pos = fraction of the piece
 *  (0 = start, 1 = end), pan = −1 (full LEFT) .. +1 (full RIGHT). */
struct PanPoint
{
    double pos = 0.0;
    double pan = 0.0;
};

/** Pan at posFrac ∈ [0..1]: smooth S-curve (Hermite smoothstep) between
 *  consecutive points — flat tangent at every handle, never overshoots the
 *  handles' values — flat before the first and after the last point,
 *  0 (centre) when the list is empty. Points must be sorted by pos. Shared
 *  by the renderer and the UI overlay so both agree exactly. */
double panAt(const std::vector<PanPoint>& points, double posFrac);

//==============================================================================
/** Page/encoding settings (defaults mirror SCORE / TIMBRE). */
struct MidiScoreSettings
{
    double printerDpi      = SCORE_DEFAULT_PRINTER_DPI;      // 400
    double dynamicRangeDB  = SCORE_DEFAULT_DYNAMIC_RANGE_DB; // 50
    double minFreq         = SCORE_DEFAULT_MIN_FREQ;         // overridden by tuning
    double maxFreq         = SCORE_DEFAULT_MAX_FREQ;
    double writingSpeed    = 2.5;    ///< cm/s — time scale of the printed band
    double lineWidthMM     = 0.30;   ///< partial line thickness on paper
    double velocityRangeDb = 24.0;   ///< velocity 127→0 dB … 1→−range (ink depth)
    double bottomMarginMM  = SCORE_DEFAULT_BOTTOM_MARGIN_MM; // 50.8
    double spectroHeightMM = SCORE_CIS_HEIGHT_MM;            // 219.456 — never change
    bool   showLabels      = false;  ///< OPT-IN title + footer in the TOP margin
    /** Export sheet size (SCORE's convention): 0 = A4 portrait, 1 = A3
     *  landscape (same 297 mm height — double the music per page), 2 = FULL
     *  (a single sheet wide enough for the whole piece, no pagination). */
    int    pageFormat      = 0;

    /** Time→pan automation, ONE CURVE PER VOICE. All empty (or all-centre) =
     *  greyscale output, exactly the historical bytes. Any real pan tints
     *  that voice's ink with SCORE's stereo convention (left = red,
     *  right = blue, centre = grey), linear-balance law: the centre keeps
     *  both sides at 0 dB, panning only attenuates the far side — so
     *  RenderResult.stereo turns true and the frames must be loaded with
     *  stereo=true for LuxStral's colour-temperature panning. Voices with an
     *  empty curve keep printing grey (centre) next to panned ones. */
    std::array<std::vector<PanPoint>, kMaxVoices> panPoints;

    /** ONE GRAPHIC EQ PER VOICE — the voice's own tone control, not a global
     *  image filter. Every partial's level is shifted by the curve's gain at
     *  the PARTIAL's frequency before its ink is drawn, so the EQ belongs to
     *  the voice exactly like Level (dB): preview, playback and print all
     *  carry it, and two voices can be EQ'd against each other.
     *
     *  Like levelDb it is deliberately EXCLUDED from the global ink
     *  normalisation (globalMaxDb) — a boost darkens toward full black, a cut
     *  lightens toward silence, instead of being cancelled by a rescale.
     *  A flat/empty curve is a strict no-op (historical bytes). */
    struct VoiceEq
    {
        double minFreq = 0.0, maxFreq = 0.0;   ///< log span the handles cover
        ShapeEqHandle handles[SHAPE_EQ_MAX_HANDLES] {};   ///< typed curve handles
        float levelDb = 0.0f;                  ///< whole-curve gain fader
        bool hasCurve = false;                 ///< a curve was decoded / captured

        /** False when there is nothing to apply — callers hoist this out of
         *  their inner loops. */
        bool active() const noexcept
        {
            return hasCurve && minFreq > 0.0 && maxFreq > minFreq
                && shape_eq_is_flat_level(handles, SHAPE_EQ_MAX_HANDLES,
                                          levelDb) == 0;
        }

        /** Gain (dB) at @p hz — the same curve ShapeEqComponent draws,
         *  clamped to the span's edges outside it. 0 dB when inactive. */
        float gainDbAt(double hz) const noexcept
        {
            if (! hasCurve || minFreq <= 0.0 || maxFreq <= minFreq)
                return 0.0f;
            if (hz < minFreq) hz = minFreq;
            if (hz > maxFreq) hz = maxFreq;
            const float x01  = (float) (std::log(hz / minFreq)
                                        / std::log(maxFreq / minFreq));
            const float span = (float) (std::log(maxFreq / minFreq)
                                        / std::log(2.0));
            return shape_eq_db_level(handles, SHAPE_EQ_MAX_HANDLES, levelDb,
                                     x01, span);
        }
    };
    std::array<VoiceEq, kMaxVoices> voiceEq;
};

/** Seconds of music one page band holds at these settings (A4 or A3 width —
 *  a FULL sheet has no fixed window, callers must not rely on it there). */
double pageSeconds(const MidiScoreSettings& s);

/** Number of pages needed for the whole piece at the chosen sheet size
 *  (≥ 1 when it has notes; always 1 for the FULL format). */
int pageCount(const MidiScoreData& data, const MidiScoreSettings& s);

//==============================================================================
/** Renders the band ALONE (no page margins) over [t0Sec..t1Sec], with
 *  independent horizontal (pxPerSec — time) and vertical (dpiY — frequency
 *  axis) resolutions. result.spectroBand covers the whole image. Windows with
 *  no notes render white (silence keeps its width — a rest is part of the
 *  piece); fails only on degenerate geometry or when NO enabled voice has
 *  notes at all. Used for playback frames and the UI preview. */
scoregen::RenderResult renderStrip(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    double t0Sec, double t1Sec,
    double pxPerSec, double dpiY);

/** Renders ONE export sheet starting at t0Sec: white page at the chosen
 *  format (A4 portrait / A3 landscape — FULL ignores t0Sec and stretches the
 *  sheet to hold the whole piece), band at the SCORE geometry, opt-in
 *  title/footer in the top margin. t0Sec is free — this is what the export
 *  zone picker renders. pageTag (e.g. "page 2/7") is appended to the opt-in
 *  title when non-empty. Notes crossing the window edges keep their envelope
 *  phase (tau is absolute).
 *
 *  progress (optional) is called ON THE CALLING THREAD with a fraction in
 *  [0..1] as the note render advances — it covers the drawing only, not the
 *  file encode a caller may do next. */
scoregen::RenderResult renderSheet(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    double t0Sec,
    const juce::String& pageTag = {},
    const std::function<void(double)>& progress = {});

/** Paginated wrapper over renderSheet(): page pageIndex ∈ [0..pageCount-1]
 *  at t0 = pageIndex × pageSeconds(). */
scoregen::RenderResult renderPage(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    int pageIndex,
    const std::function<void(double)>& progress = {});

} // namespace midiscoregen
