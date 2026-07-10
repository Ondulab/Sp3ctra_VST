/**
 * @file TimbreGenRenderer.h
 * @brief TIMBRE — parametric instrument-spectrum generator (sibling of SCORE).
 *
 * Where SCORE analyses a WAV (STFT) into a printable spectrogram, TIMBRE
 * SYNTHESISES the spectrogram directly from timbre parameters: a set of
 * partials (harmonic series with stretch/comb/tilt, or inharmonic bell
 * tables) drawn as horizontal lines on the same log-frequency band the
 * instrument scans. One A4 portrait page carries kNumSlots = 6 sounds side
 * by side (time runs left→right inside each slot), so a single print yields
 * six playable timbre strips.
 *
 * Encoding conventions are identical to SCORE so a print plays back in tune
 * and at the intended level through the unchanged reader:
 *   - band height  = SCORE_CIS_HEIGHT_MM (219.456 mm — the CIS sensor span)
 *   - LOG frequency axis over [minFreq..maxFreq] (equal space per octave)
 *   - inverted greyscale, dB-linear over dynamicRangeDB (white = silence)
 *   - export via scoregen::exportImage (PNG pHYs / JPEG JFIF density stamp)
 *
 * Pure functions, no UI, no globals — safe to call from any thread.
 */
#pragma once

#include <array>
#include <vector>
#include "ScoreGenRenderer.h"   // scoregen::RenderResult + score_engine.h constants

namespace timbregen
{

constexpr int kNumSlots = 6;   ///< sounds per A4 page (user requirement)

//==============================================================================
/** One sound slot — everything needed to synthesise its partial set.
 *  All levels are relative dB (0 = slot's strongest partial before tilt). */
struct TimbreSlotParams
{
    bool   enabled       = true;
    int    preset        = 0;      ///< index into presetName() (kPresetCustom = hand-tuned)
    int    midiNote      = 57;     ///< fundamental (57 = A3 = 220 Hz)
    int    numPartials   = 24;     ///< harmonic mode only (bell tables are fixed)
    double slopeDbPerOct = -6.0;   ///< spectral tilt applied per octave of partial index
    double oddBias       = 0.0;    ///< 0 = all harmonics … 1 = odd only (evens −48 dB)
    double inharmonicity = 0.0;    ///< stiff-string stretch B: f_n = n·f0·√(1+B·n²)
    double combDepth     = 0.0;    ///< pluck comb 0..1 (amp × |sin(π·n·pos)|^depth)
    double combPos       = 0.28;   ///< pluck position along the string (0.05..0.5)
    double attackMs      = 4.0;    ///< linear amplitude ramp at slot start
    double decaySec      = 0.0;    ///< time for the FUNDAMENTAL to fall 60 dB; 0 = sustain
    double hfDamp        = 0.5;    ///< 0..1 — higher partials decay faster (decay-rate mult)
    double levelDb       = 0.0;    ///< slot gain (−24..+6)
    bool   bellMode      = false;  ///< use an inharmonic partial TABLE instead of harmonics
    int    bellTable     = 0;      ///< 0 = church bell, 1 = struck bar (glockenspiel)
};

//==============================================================================
// Presets — parameter templates for classic timbres. applyPreset() overwrites
// the TIMBRAL fields only (enabled / midiNote / levelDb are the user's).
//==============================================================================
int         numPresets();
const char* presetName(int preset);          ///< "Square", "Bell (church)"…
void        applyPreset(TimbreSlotParams& p, int preset);
constexpr int kPresetCustom = -1;            ///< sentinel: hand-tuned (no template)

//==============================================================================
/** Page-level settings (shared by the 6 slots). Defaults mirror SCORE. */
struct TimbrePageSettings
{
    double printerDpi     = SCORE_DEFAULT_PRINTER_DPI;      // 400
    double dynamicRangeDB = SCORE_DEFAULT_DYNAMIC_RANGE_DB; // 50
    double minFreq        = SCORE_DEFAULT_MIN_FREQ;         // overridden by tuning
    double maxFreq        = SCORE_DEFAULT_MAX_FREQ;
    double writingSpeed   = 2.5;    ///< cm/s — sets each slot's duration (width/speed)
    double lineWidthMM    = 0.30;   ///< partial line thickness on paper
    double slotGapMM      = 4.0;    ///< white silence between adjacent sounds
    double bottomMarginMM = SCORE_DEFAULT_BOTTOM_MARGIN_MM; // 50.8
    double spectroHeightMM= SCORE_CIS_HEIGHT_MM;            // 219.456 — never change
    bool   showLabels     = false;  ///< OPT-IN slot titles + footer, drawn at the very
                                    ///< TOP of the page (far from the band). Cut marks
                                    ///< at the slot edges are always drawn.
};

/** Seconds of sound one slot holds at these settings (slot width / speed). */
double slotSeconds(const TimbrePageSettings& s);

//==============================================================================
/** Computed partial line, exposed for the UI (spectrum inspector / tooltips). */
struct Partial
{
    double freqHz;     ///< absolute frequency
    double ampDb;      ///< level relative to the slot's strongest partial (≤ 0)
    double decayMul;   ///< decay-rate multiplier (1 = fundamental's rate)
};

/** Partial set for one slot (independent of the page layout). */
std::vector<Partial> computePartials(const TimbreSlotParams& p);

/** MIDI note number → "A3 (220.0 Hz)" style label. */
juce::String midiNoteLabel(int midiNote);

/** MIDI note number → frequency (A440 equal temperament). */
double midiNoteHz(int midiNote);

//==============================================================================
/** Renders the full A4 portrait page: white page, six timbre strips across the
 *  CIS-height band, slot labels + cut marks in the margins (outside the band).
 *  Fast (pure drawing, no FFT) — callable synchronously on the message thread.
 *  Returns ok=false with a message when no slot is enabled or the geometry is
 *  degenerate. result.spectroBand is the scanned/played region (like SCORE). */
scoregen::RenderResult renderTimbrePage(
    const std::array<TimbreSlotParams, kNumSlots>& slots,
    const TimbrePageSettings& settings);

} // namespace timbregen
