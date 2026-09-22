/**
 * @file TimbreGenRenderer.h
 * @brief TIMBRE — parametric instrument-spectrum generator (sibling of SCORE),
 *        and the NOTE PAINTER every synthesised page shares.
 *
 * Where SCORE analyses a WAV (STFT) into a printable spectrogram, TIMBRE
 * SYNTHESISES the spectrogram directly from timbre parameters: a set of
 * partials (harmonic series with stretch/comb/tilt, or inharmonic tables)
 * drawn as horizontal lines on the same log-frequency band the instrument
 * scans. One A4 portrait page carries kNumSlots = 6 sounds side by side
 * (time runs left→right inside each slot), so a single print yields six
 * playable timbre strips.
 *
 * The timbre MODEL (TimbreSlotParams → partials → ink) lives here and is
 * shared: MIDI SCORE paints every MIDI note with the same NotePainter, so a
 * timbre tuned on the TIMBRE page sounds identical under a MIDI piece, and
 * every texture below reaches both pages at once.
 *
 * What a synthesiser's spectrogram lacks, and the textures that supply it:
 *   - an onset BURST: the pick, hammer, tongue or bow grip — a short
 *     broadband grain at the attack whose width shrinks with frequency, the
 *     way a click reads on a constant-Q analysis;
 *   - sustained NOISE: breath, bow hair, wind — a grained floor tied to the
 *     note's fundamental and its envelope;
 *   - UNISON: a detuned pair per partial (piano strings, sections, chorus).
 *     The instrument reproduces the beating physically — two oscillators a
 *     few cents apart — so nothing needs to be faked;
 *   - a BODY resonance fixed in absolute frequency, so one preset sounds
 *     different from note to note as a real instrument does;
 *   - a BRIGHTNESS RAMP at the attack: brass blooms upward, a pluck's highs
 *     arrive first;
 *   - DRIFT: a slow wander of the whole stack, a partial is never a ruler.
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
#include <functional>
#include <vector>
#include "ScoreGenRenderer.h"   // scoregen::RenderResult + score_engine.h constants

namespace timbregen
{

constexpr int kNumSlots = 6;   ///< sounds per A4 page (user requirement)

/** Level below which a relative-dB texture is OFF (never painted). The
 *  printable window is 50 dB, so this is a clear "nothing" rather than a
 *  faint something. */
constexpr double kOffDb = -60.0;

/** Spectral recipes are independent of preset identity: editing a factory
 * sound keeps its character, and sessions store it even after becoming Custom.
 * Append only; numeric values are persisted. Neutral preserves legacy patches. */
enum class Spectrum { Neutral, Piano, Tine, Plucked, FingerBass, PickBass,
    SlapBass, Bowed, Flute, Clarinet, DoubleReed, Brass, VoiceAh, VoiceOo,
    VoiceEe, Tonewheel, FM, Count };
const char* spectrumName(int spectrum);

//==============================================================================
/** One sound slot — everything needed to synthesise its partial set.
 *  All levels are relative dB (0 = slot's strongest partial before tilt). */
struct TimbreSlotParams
{
    bool   enabled       = true;
    int    preset        = 0;      ///< index into presetName() (kPresetCustom = hand-tuned)
    int    customBase    = -1;     ///< preset a Custom patch drifted from (-1 = none):
                                   ///< lets the UI say "Custom (Piano)" instead of an
                                   ///< anonymous "Custom" — display only, never rendered
    int    midiNote      = 57;     ///< fundamental (57 = A3 = 220 Hz)
    int    numPartials   = 24;     ///< harmonic mode only (tables are fixed); 0 = no
                                   ///< partial stack at all (noise-only sounds)
    double slopeDbPerOct = -6.0;   ///< spectral tilt applied per octave of partial index
    double oddBias       = 0.0;    ///< 0 = all harmonics … 1 = odd only (evens −48 dB)
    double inharmonicity = 0.0;    ///< stiff-string stretch B: f_n = n·f0·√(1+B·n²)
    double combDepth     = 0.0;    ///< pluck comb 0..1 (amp × |sin(π·n·pos)|^depth)
    double combPos       = 0.28;   ///< pluck position along the string (0.05..0.5)
    double attackMs      = 4.0;    ///< smooth amplitude ramp at note start
    double decaySec      = 0.0;    ///< time for the FUNDAMENTAL to fall 60 dB; 0 = sustain
    double hfDamp        = 0.5;    ///< 0..1 — higher partials decay faster (decay-rate mult)
    double levelDb       = 0.0;    ///< slot gain (−24..+6)
    bool   bellMode      = false;  ///< use an inharmonic partial TABLE instead of harmonics
    int    bellTable     = 0;      ///< index into tableName() (church bell, struck bar…)

    int    spectrum      = (int) Spectrum::Neutral;
    double cutoffHz      = 0.0;    ///< 24 dB/oct low-pass; 0 = open
    double filterSweepOct= 0.0;    ///< initial cutoff offset in octaves
    double filterDecayMs = 180.0;  ///< cutoff settles exponentially to cutoffHz
    double pitchAttackCents = 0.0; ///< initial pitch offset (pluck, kick, fretless)
    double pitchSettleMs = 35.0;   ///< offset settles exponentially to zero

    // Vibrato — a pitch wave of the whole partial stack (on the log axis,
    // ±cents = a constant vertical offset).
    double vibCents      = 0.0;    ///< peak depth in cents (0 = off)
    double vibRateHz     = 5.5;    ///< nominal oscillation rate
    double vibOnsetSec   = 0.4;    ///< time for the vibrato to develop after note start
    double vibLife       = 0.5;    ///< 0..1 — humanisation: waving depth, drifting rate,
                                   ///<        per-note randomisation (0 = mechanical)

    // ── Textures (see the file header) ───────────────────────────────────────
    double driftCents    = 0.0;    ///< slow wander of the whole stack (peak, cents)
    double brightRamp    = 0.0;    ///< −1..+1: partial n attacks in attackMs × n^ramp —
                                   ///< >0 the highs bloom later (brass), <0 earlier (pluck)
    double unisonCents   = 0.0;    ///< detuned pair per partial, total spread (0 = single)
    double bodyHz        = 0.0;    ///< one body resonance in absolute Hz (0 = none)
    double bodyDb        = 0.0;    ///< its gain (±), half an octave wide
    double burstDb       = kOffDb; ///< total onset-noise RMS vs one peak partial
    double burstMs       = 8.0;    ///< its length at 1 kHz (scales as 1/√f)
    double burstTilt     = 0.0;    ///< its colour, dB per octave above 1 kHz
    double noiseDb       = kOffDb; ///< total sustained-noise RMS vs one peak partial
    double noiseTilt     = -3.0;   ///< its colour, dB per octave above the fundamental
};

//==============================================================================
// Presets — parameter templates for classic timbres. applyPreset() overwrites
// the TIMBRAL fields only (enabled / midiNote / levelDb are the user's).
//==============================================================================
int         numPresets();
const char* presetFamily(int preset);
const char* presetSubfamily(int preset);
int         presetSuggestedNote(int preset); ///< preview register; never transposes MIDI
const char* presetName(int preset);          ///< "Square", "Bell (church)"…
void        applyPreset(TimbreSlotParams& p, int preset);
constexpr int kPresetCustom = -1;            ///< sentinel: hand-tuned (no template)

/** Inharmonic partial tables (bellMode). */
int         numTables();
const char* tableName(int table);            ///< "Church bell", "Membrane"…

//==============================================================================
/** Page-level settings (shared by the 6 slots). Defaults mirror SCORE. */
struct TimbrePageSettings
{
    double printerDpi     = SCORE_DEFAULT_PRINTER_DPI;      // 400
    double dynamicRangeDB = SCORE_DEFAULT_DYNAMIC_RANGE_DB; // 50
    double minFreq        = SCORE_DEFAULT_MIN_FREQ;         // overridden by tuning
    double maxFreq        = SCORE_DEFAULT_MAX_FREQ;
    double writingSpeed   = 2.5;    ///< cm/s — sets each slot's duration (width/speed)
    double lineWidthMM    = 0.30;   ///< fundamental width; upper partials narrow, power stays fixed
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

/** Partial set for one slot (independent of the page layout). Empty when
 *  numPartials is 0 in harmonic mode. */
std::vector<Partial> computePartials(const TimbreSlotParams& p);

/** MIDI note number → "A3 (220.0 Hz)" style label. */
juce::String midiNoteLabel(int midiNote);

/** MIDI note number → frequency (A440 equal temperament). */
double midiNoteHz(int midiNote);

/** MIDI velocity → ink dB: 127 = 0 dB, 1 = −rangeDb. */
inline double velocityDb(int vel, double rangeDb)
{
    return -rangeDb * (1.0 - (double) juce::jlimit(1, 127, vel) / 127.0);
}

//==============================================================================
// Persistence — one codec for every page that stores a timbre (TIMBRE slots,
// MIDI SCORE voices), so a new parameter is saved and read back everywhere
// with one edit. Only the TIMBRAL fields, the preset identity and the level:
// enabled / midiNote are page business (MIDI SCORE has no note per voice).
// Missing keys keep the current value, so older states load unchanged.
//==============================================================================
void encodeParams(juce::DynamicObject& o, const TimbreSlotParams& p);
void decodeParams(const juce::DynamicObject& o, TimbreSlotParams& p);

//==============================================================================
// The shared note painter
//==============================================================================

/** Geometry of the band inside the target image + the time window. */
struct BandGeom
{
    double x0Px     = 0.0;   ///< px column of t0 (may be fractional)
    int    xMin = 0, xMax = 0;
    double yBottom  = 0.0;   ///< px row of minFreq (band bottom)
    double heightPx = 0.0;
    int    yTop = 0, yBot = 0;
    double pxPerSec = 1.0;
    double t0 = 0.0, t1 = 0.0;
};

/** A timbre prepared for painting: the partial RATIOS (relative dBs, decay
 *  multipliers) are note-independent, so they are computed once at a
 *  reference fundamental and rescaled per note. */
struct VoiceModel
{
    bool   enabled   = false;
    double refHz     = 440.0;
    double maxAmpDb  = -1.0e9;   ///< strongest partial (0 dB when there is none but
                                 ///< a texture is on — the textures' reference)
    std::vector<Partial> rel;

    double attackSec = 0.004, decaySec = 0.0, levelDb = 0.0, brightRamp = 0.0;
    double vibCents = 0.0, vibRateHz = 5.5, vibOnsetSec = 0.4, vibLife = 0.5;
    int spectrum = (int) Spectrum::Neutral;
    double cutoffHz = 0.0, filterSweepOct = 0.0, filterDecaySec = 0.18;
    double pitchAttackCents = 0.0, pitchSettleSec = 0.035;
    double driftCents = 0.0;
    double unisonCents = 0.0, bodyHz = 0.0, bodyDb = 0.0;
    double burstDb = kOffDb, burstSec = 0.008, burstTilt = 0.0;
    double noiseDb = kOffDb, noiseTilt = -3.0;

    bool hasBurst() const noexcept { return burstDb > kOffDb + 0.01; }
    bool hasNoise() const noexcept { return noiseDb > kOffDb + 0.01; }
    bool hasInk()   const noexcept { return enabled && (! rel.empty() || hasBurst() || hasNoise()); }
};

VoiceModel buildVoiceModel(const TimbreSlotParams& p);

/** Ink conventions of one render. */
struct InkSettings
{
    double minFreq        = SCORE_DEFAULT_MIN_FREQ;
    double maxFreq        = SCORE_DEFAULT_MAX_FREQ;
    double dynamicRangeDB = SCORE_DEFAULT_DYNAMIC_RANGE_DB;
    double lineWidthMM    = 0.30;
    double dpiY           = 400.0;   ///< vertical resolution (line width → px)
    double maxDb          = 0.0;     ///< reference level before distributing partial power across rows
};

/** One note to paint. Levels are relative dB in the caller's own scale —
 *  InkSettings::maxDb is subtracted once. */
struct NoteInk
{
    double f0Hz      = 440.0;
    double startSec  = 0.0, endSec = 1.0;
    double levelDb   = 0.0;      ///< velocity + voice gain (whatever the caller normalises)
    size_t noteIndex = 0;        ///< seeds every per-note randomness (deterministic re-render)
    double fadeSec   = 0.0;      ///< end fade (anti-click); ≤ 0 = min(40 ms, 20 % of the note)

    // Captured curves (Sp3ctra MPE takes), times RELATIVE to startSec:
    // bend in cents, level 0..127 replacing the velocity's dB from then on.
    const std::vector<std::pair<float, float>>* bendPts  = nullptr;
    const std::vector<std::pair<float, float>>* levelPts = nullptr;
    int    velocity        = 127;
    double velocityRangeDb = 24.0;

    // Per-column L/R attenuations (dB ≤ 0) indexed from BandGeom::xMin —
    // nullptr = centred (grey ink). Both or neither.
    const double* panDbL = nullptr;
    const double* panDbR = nullptr;

    // Extra gain at a partial's ABSOLUTE frequency (a voice EQ). Applied
    // after normalisation, like the level — a boost darkens toward black.
    const std::function<double(double)>* gainDbAt = nullptr;
};

/** Paints notes into ONE image band. Construct once per render (it pins the
 *  bitmap and reuses its per-column scratch), then paint() every note. */
class NotePainter
{
public:
    NotePainter(juce::Image& img, const BandGeom& g, const InkSettings& ink);
    ~NotePainter();

    void paint(const VoiceModel& vm, const NoteInk& n);

    /** True once any note was painted with a pan (colour ink on the page). */
    bool usedStereoInk() const noexcept { return stereoInk_; }

private:
    struct Impl;
    Impl* impl_;
    bool  stereoInk_ = false;

    JUCE_DECLARE_NON_COPYABLE(NotePainter)
};

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
