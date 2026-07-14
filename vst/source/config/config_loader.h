/* config_loader.h */

#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stdint.h>
#include "../utils/logger.h"

/**************************************************************************************
 * Sp3ctra Runtime Configuration Structure
 **************************************************************************************/

/* LuxStral phase-management onset modes (luxstral_phase_mode) — the physical
 * initial conditions applied to an oscillator's phase when its note attacks.
 * See the field's comment block in the struct below for the full contract.
 * Order matches the "luxstralPhaseMode" APVTS choice parameter.              */
enum {
  LUXSTRAL_PHASE_MODE_FREE   = 0,  /* legacy free-running phases              */
  LUXSTRAL_PHASE_MODE_STRIKE = 1,  /* struck string: ±sine, sign by position  */
  LUXSTRAL_PHASE_MODE_PLUCK  = 2,  /* plucked string: ±cosine, sign by pos.   */
  LUXSTRAL_PHASE_MODE_BELL   = 3,  /* percussion: fixed hash (pos. = impact)  */
  LUXSTRAL_PHASE_MODE_BREATH = 4,  /* reed/flute: fresh random per attack     */
};

/* ── Per-OUT (send) conditioning bank — synth-split P1 ─────────────────────
 * One entry per OUT-module pool slot (0..7). An OUT module conditions its
 * chain's image flux before sending it to the global synthesis engine.
 * Values come from the luxstralOut{N}_* / luxsynthOut{N}_* / luxwaveOut{N}_*
 * APVTS banks — the ONLY conditioning authority (the legacy global fields
 * were deleted on 2026-07-12). One bank slot per OUT-module instance.
 * contrast_min / range_db are LuxStral-only (ignored by the other banks). */
#define LUX_OUT_MAX_SLOTS 8
typedef struct {
    int   negative;      /* Negative (inversion) toggle                     */
    int   dc_blocking;   /* DC blocking (per-line mean removal) toggle      */
    float gamma;         /* photographic gamma pow(x, 1/g); 1.0 = off       */
    float contrast_min;  /* LuxStral: contrast floor for blurred images     */
    float range_db;      /* LuxStral: inverse-dB decode window (Range dB)   */
    float intensity;     /* pre-engine mix weight of this send (1.0=unity)  */
    int   enabled;       /* per-send power (rack LED); off = silent send    */
} lux_out_params_t;

typedef struct {
    // Logging configuration
    log_level_t log_level;               // Logging level (ERROR, WARNING, INFO, DEBUG)
    
    // Audio system parameters (runtime configurable)
    int sampling_frequency;              // Sampling frequency in Hz (22050, 44100, 48000, 96000)
    int audio_buffer_size;               // Audio buffer size in frames
    
    // Synthesis parameters (user-configurable)
    float low_frequency;          // Starting frequency in Hz
    float high_frequency;         // Ending frequency in Hz
    int sensor_dpi;               // Sensor DPI (200 or 400)

    // Synthesis parameters (automatically calculated from above)
    float start_frequency;        // Same as low_frequency (kept for backward compatibility)
    int semitone_per_octave;      // Always 12 (standard musical scale)
    int comma_per_semitone;       // Calculated based on DPI and frequency range
    int pixels_per_note;          // Always 1 for maximum resolution
    int num_octaves;              // Fixed number of octaves (set by user, not calculated dynamically)
    int physiological_filter_enabled;       // Enable/disable A-weighting inverse compensation (0/1)
    float physiological_correction_depth;   // Correction depth in dB-domain (0.0=none, 1.0=full, default 0.3)
    
    // Image processing parameters - LUXSTRAL SYNTHESIS
    // (inversion/gamma/contrast_min are per-OUT bank fields now — see
    // lux_out_params_t; only the contrast-scan tuning stays global.)
    float additive_contrast_stride;            // Pixel sampling stride for optimization
    float additive_contrast_adjustment_power;  // Exponent for adjusting the contrast curve

    // Envelope slew parameters (runtime configurable; defaults from compile-time defines)
    float tau_up_base_ms;             // Base attack time in milliseconds
    float tau_down_base_ms;           // Base release time in milliseconds
    float decay_freq_ref_hz;          // Reference frequency in Hz for frequency weighting
    float decay_freq_beta;            // >0 slows highs, <0 speeds highs

    // Stereo processing parameters
    int stereo_mode_enabled;                   // Enable/disable stereo mode (0/1)
    float stereo_temperature_amplification;    // Global stereo intensity control
    float stereo_blue_red_weight;              // Weight for blue-red opponent axis
    float stereo_cyan_yellow_weight;           // Weight for cyan-yellow opponent axis
    float stereo_temperature_curve_exponent;   // Exponent for response curve shaping
    
    // Threading parameters
    int num_workers;                           // Number of worker threads for additive synthesis (1-8)
    
    // (Summation-normalization exponents deleted 2026-07-12 — replaced by the
    // RMS ceiling: rms_ceiling_gain in synth_luxstral.c.)

    // Soft limiter parameters
    float soft_limit_threshold;                // Soft limiter threshold (0.0-1.0)
    float soft_limit_knee;                     // Soft limiter knee width (0.0-1.0)
    
    // Noise gate parameters
    float noise_gate_threshold;                // Noise gate threshold (0.0-0.1, fraction of max volume)
    
    // (Purge 2026-07-12: the legacy poly_* engine-config block is gone — the
    // LuxSynth engine has its own LuxSynthConfig synced from the luxsynth*
    // APVTS params. Only the harmonicity pair below is consumed by the
    // pipeline, in image_preprocessor.c.)

    // LuxSynth harmonicity parameters (color-based timbre control)
    float poly_detune_max_cents;               // Maximum detune for semi-harmonic sounds (cents)
    float poly_harmonicity_curve_exponent;     // Exponent for harmonicity response curve (0.5-2.0)
    
    // Network configuration
    char udp_address[64];                      // UDP address for data reception (unicast or multicast)
    int udp_port;                              // UDP port for data reception
    char multicast_interface[64];              // Specific interface IP for multicast (empty = INADDR_ANY)

    // StrokeForge — Blob-centric harmonic morphing
    /* ── StrokeForge — Blob-to-note with waveform morphing ──────────────────
     * Blob width controls sine→square morphing; Gaussian focus concentrates
     * energy on the blob center note (attenuates neighboring oscillators).
     *
     * Two independent toggles:
     *   strokeforge_enabled   = 1 → full StrokeForge: Gaussian focus + sine→square morph
     *   strokeforge_focus_only = 1 → Gaussian focus only, no morph (pure sine in spectral mode)
     * When both are 0 → pure spectral passthrough (legacy behaviour).          */
    int   strokeforge_enabled;                 /* Master toggle (morph + focus)             */
    int   strokeforge_focus_only;              /* Focus-only mode: Gaussian focus, no morph */
    float strokeforge_blob_base_threshold;     /* Min amplitude for blob detection [0.01-0.5] */
    int   strokeforge_blob_min_width;          /* Min blob width in notes [1-50]            */
    int   strokeforge_blob_merge_gap;          /* Max gap between sub-blobs to merge [0-20] */
    float strokeforge_morph_width_scale;       /* Width (notes) where morph reaches 1.0 (square) [10-500] */
    float strokeforge_blob_focus_sigma;        /* Gaussian sigma in notes [0.5-100]; small=pure tone, large=spectral cloud */
    float strokeforge_spectral_width_threshold; /* Blob width (notes) >= this → raw spectral passthrough (no Gaussian); 0=disabled [0-3456] */

    /* ── LuxSynth blob detection — fully independent of StrokeForge/LuxStral ─
     *
     * These parameters control blob detection only for the SYNTH_BLOB
     * visualizer (CisVisualizerComponent::detectSynthBlobs).
     * They are completely separate from the StrokeForge parameters
     * (strokeforge_blob_*) which belong exclusively to the LuxStral path.
     *
     * luxsynth_blob_threshold  : brightness threshold in [0,1].
     *   A pixel whose localDataGray / 255 >= threshold is considered active.
     *   0.05 is a good default for an inverted CIS image.
     *
     * luxsynth_blob_min_width  : reject blobs narrower than this (pixels).
     *
     * luxsynth_blob_merge_gap  : active pixels separated by at most this many
     *   inactive pixels are merged into the same blob.
     *
     * luxsynth_blob_color_split : color-temperature jump (|(R-B)/255| between
     *   adjacent pixels) that terminates the running blob and starts a new one.
     *   0.20 ≈ a noticeable hue shift (e.g. neutral paper → warm ink edge).
     */
    float luxsynth_blob_threshold;    /* Min brightness [0.01-0.30], default 0.05 */
    int   luxsynth_blob_min_width;    /* Min blob width in pixels [1-200], default 10 */
    int   luxsynth_blob_merge_gap;    /* Max gap to merge [0-100], default 3       */
    float luxsynth_blob_color_split;  /* Color-temp jump to split blob [0.01-1.0], default 0.20 */

    /* ── Image Pipeline — live controls ──────────────────────────────────── */
    /* image_live_opacity : scale factor applied to out->additive.notes[]     */
    /*   1.0 = full amplitude   0.0 = complete silence (all notes → 0)        */
    float image_live_opacity;

    /* sampler_gamma : gamma applied to LuxSampler playback frames (1.0=off)  */
    float sampler_gamma;
    /* sampler_contrast_min : min intensity floor for sampler frames            */
    float sampler_contrast_min;
    /* sampler_freeze_mode : 0=PLAY, 1=PAUSE (freeze sampler last frame)        */
    int sampler_freeze_mode;

    /* raw_freeze_mode : upstream gate — controls data flow from UDP/conversion */
    /*   0 = PLAY  — data flows to Sampler and Live                            */
    /*   1 = HOLD  — freeze last raw frame (both S and L freeze)               */
    /*   2 = STOP  — force silence on all downstream paths                     */
    int raw_freeze_mode;

    /* (M8/purge 2026-07-12: the legacy per-path source-routing globals are
     * gone — the ChainPlan routes, inserts read the stream at their position.) */

    /* ── M4 — core-side LuxSynth engine feed (luxsynth_feed_tick) ─────────── */
    int   lx_fft_bins_choice;              /* 0=32, 1=64, 2=128, 3=256 harmonics */
    float lx_fft_smoothing;                /* [0..1] temporal smoothing (attack/release) */

    /* ── Inverse-dB decode law — ALWAYS ON (single decode chain) ─────────────
     * The grey → amplitude decode law is the exact inverse of the SCORE
     * encoder's linear-in-dB brightness map (score_engine.c):
     * amplitude = 10^((x−1)·range/20), applied AFTER the gamma stage (gamma
     * 1.0 = pure dB decode).
     * No toggle, no forcing — every stage keeps its own knob.  The exact
     * inverse of the SCORE encoder is recovered PER SEND with the OUT bank:
     *   Negative ON · DC Blocking OFF · Gamma 1.0 · Contrast Min 1.0 ·
     *   Attack 2 ms · Release 6 ms · Equal-Loudness OFF
     * (decay_freq_beta = 0 and phase reset are already law-independent).
     * The dB window is the PER-OUT range_db (luxstralOut{N}_rangeDb) and MUST
     * match the dynamicRangeDB the score was generated with (default 50).    */

    /* ── Per-OUT conditioning banks (synth-split P1) ─────────────────────────
     * Written from the luxstralOut{N}_* / luxsynthOut{N}_* / luxwaveOut{N}_*
     * APVTS banks by applyConfigurationToCore(); read by the pipeline config
     * builders (image_pipeline.c), preprocess_luxsynth() and the LuxWave feed.
     * The ONLY conditioning authority (legacy globals deleted 2026-07-12).   */
    lux_out_params_t luxstral_out[LUX_OUT_MAX_SLOTS];
    lux_out_params_t luxsynth_out[LUX_OUT_MAX_SLOTS];
    lux_out_params_t luxwave_out[LUX_OUT_MAX_SLOTS];

    /* ── Phase management: physical onset modes ──────────────────────────
     * Selecting a mode is the ONLY required gesture — the onset gate is
     * AUTO-CALIBRATED: the producer tracks a slow-decaying max of per-note
     * target volumes (LuxStralEngine.phase_onset_ref, ~10 s decay) and sets
     * the absolute gate to sensitivity_fraction × ref (floor 0.003), so the
     * feature works regardless of the material's internal volume scale.
     * luxstral_phase_sensitivity ∈ [0..1]: 1 = catch even soft onsets (3 %
     * of recent peak), 0 = only the hardest attacks (~50 % of recent peak).
     *
     * While a mode is active (mode ≠ FREE), an INAUDIBLE oscillator
     * (current_volume ≤ LUXSTRAL_PHASE_RESET_SILENCE_EPS) idles on a fresh
     * random phase re-drawn every buffer: the bank's resting state is
     * decorrelated, and a MODE CHANGE therefore never inherits the previous
     * mode's phase organization (sounding notes keep their phase — touching
     * them would click — and adopt the new law at their next attack). When
     * a note fires (target volume crosses the gate from silence = a strong
     * requested volume jump), the phase assigned depends on the mode's
     * physical initial conditions:
     *   STRIKE — struck string (piano): the hammer imparts VELOCITY → every
     *            partial starts as a sine (phase 0 or π); the sign pattern
     *            alternates along the bank with period 1/position notes
     *            (≈ sign of sin(n·π·p)). position 0 → all partials on 0.
     *   PLUCK  — plucked string (guitar/harp): initial SHAPE, zero velocity
     *            → every partial starts at an extremum (cosine, ±π/2), same
     *            position-driven sign pattern.
     *   BELL   — percussion: each mode's phase is set by the impact point —
     *            arbitrary-looking but FIXED per (note, position): every
     *            re-strike is bit-identical (sampler-repeatable) yet the
     *            band never phase-locks into a comb.
     *   BREATH — reed/flute: the oscillation grows out of turbulence; the
     *            phase is a fresh random draw at EVERY attack (living,
     *            non-repeatable).
     * Click-free by construction (only silent oscillators are touched); ε
     * doubles as hysteresis (no retrigger until the envelope releases).
     * Engines A and B.
     *
     * Phase drift — companion of the modes. Aligned onsets (STRIKE/PLUCK at
     * low position) leave the whole active band beating in lockstep (the
     * log-regular note grid gives every adjacent pair the same Δf) → a
     * single deep comb sweeping coherently = flanger. At each onset the
     * note also redraws a random micro-detune of ±drift cents
     * (phase_inc × (1 + detune_offset)): each pair then beats at a slightly
     * different rate and the post-attack coherence melts into ensemble
     * texture — higher notes first, like inharmonic partials. 0 = off.     */
    int   luxstral_phase_mode;             /* LUXSTRAL_PHASE_MODE_*, 0 = FREE (legacy) */
    float luxstral_phase_sensitivity;      /* onset sensitivity [0..1], relative to recent peak */
    float luxstral_phase_position;         /* strike/pluck position, BELL impact seed [0..1] */
    float luxstral_phase_drift_cents;      /* per-onset random detune ±cents [0..3], 0 = off */

    /* image_freeze_mode : transport state for the live image stream           */
    /*   0 = PLAY  — normal frame update                                       */
    /*   1 = HOLD  — freeze last captured frame (skip update)                  */
    /*   2 = WHITE — force all notes to 0.0 (silence)                         */
    int image_freeze_mode;

    /* image_sampler_opacity : opacity scale applied to the LuxSampler stream */
    /*   1.0 = full signal   0.0 = complete silence                            */
    float image_sampler_opacity;

    /* image_fade_in_ms : fade-in/out duration for the live stream (0-2000 ms) */
    int image_fade_in_ms;
} sp3ctra_config_t;

/**************************************************************************************
 * Global Configuration Instance
 **************************************************************************************/
extern sp3ctra_config_t g_sp3ctra_config;

#endif // CONFIG_LOADER_H
