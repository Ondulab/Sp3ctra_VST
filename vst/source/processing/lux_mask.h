/*
 * lux_mask.h
 *
 * LuxMask — MIDI-driven mobile spotlight ("synesthetic EQ") for a stable image.
 *
 * Mirrors LuxPitch ownership patterns (3 global instances: visualizer / proc /
 * video) and voice-management rules, but instead of translating the image, it
 * reveals it through a spatial LP/HP/BP filter.  The played note anchors the
 * cutoff (keyboard tracking) and the ADSR output drives the filter openness
 * (optionally inverted → open by default).  The revealed passband is full
 * opacity with a slope-controlled soft edge; position can wobble via the LFO.
 *
 * Polyphonic blend rule: additive on alpha, clamped to 1.
 *
 * RT-safety: Pure C, allocation-free, bounded O(N * voices).
 *            No JUCE deps, no mutex, no logging.
 *            MIDI state is exchanged via C11 atomics (audio → image thread).
 *
 * Author: zhonx
 * Created: 2026-06-08
 */

#ifndef LUX_MASK_H
#define LUX_MASK_H

#include <stdint.h>

#ifdef __cplusplus
  #include <atomic>
  #define LM_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define LM_ATOMIC(T) _Atomic T
#endif

/* Capacity matches LuxPitch (>6912 for 400 DPI CIS). */
#define LUX_MASK_MAX_PIXELS 8192

/* Polyphony cap. */
#define LUX_MASK_MAX_VOICES 10

/* Background mode (only meaningful when floor == 0 — pure spotlight). */
#define LUX_MASK_BG_BLACK  0
#define LUX_MASK_BG_WHITE  1

/* Pixel-per-semitone coupling. */
#define LUX_MASK_COUPLING_LUXSTRAL  0
#define LUX_MASK_COUPLING_FREE      1

/* ADSR stages (same encoding as LuxPitch). */
#define LUX_MASK_ENV_IDLE     0
#define LUX_MASK_ENV_ATTACK   1
#define LUX_MASK_ENV_DECAY    2
#define LUX_MASK_ENV_SUSTAIN  3
#define LUX_MASK_ENV_RELEASE  4

/* ============================================================================
 * LuxMaskVoiceMidi — Per-voice atomic MIDI state (audio → image thread).
 * ============================================================================ */
typedef struct {
    LM_ATOMIC(int) active;
    LM_ATOMIC(int) note;
    LM_ATOMIC(int) velocity;
    LM_ATOMIC(int) retrigger;     /* 1 = voice was stolen while active: force re-ATTACK */
    LM_ATOMIC(int) sustained;     /* 1 = note released while sustain pedal (CC64) held */
} LuxMaskVoiceMidi;

/* ============================================================================
 * LuxMaskVoiceState — Per-voice runtime state (image thread only).
 * ============================================================================ */
typedef struct {
    int      note;
    float    velocity_norm;
    float    current_pos;        /* Glide-smoothed mask center in pixels. */
    float    target_pos;         /* Target center (from note + bend). */
    float    envelope_level;     /* 0..1 ADSR level. */
    int      envelope_stage;
    float    env_phase;          /* Normalised progress [0,1] through current segment (alpha shaping). */
    float    seg_start_level;    /* Envelope level captured at segment entry (click-free). */
    int      prev_active;
    uint32_t age;
    /* Envelope snapshots (alpha -> cutoff envelope shaping): */
    float    peak_level;         /* Envelope peak at attack (velocity-scaled). */
    float    release_start_level;/* Envelope level at the moment release began. */
} LuxMaskVoiceState;

/* ============================================================================
 * LuxMaskMidiState — Global atomic MIDI state.
 * ============================================================================ */
typedef struct {
    LuxMaskVoiceMidi voices[LUX_MASK_MAX_VOICES];
    LM_ATOMIC(int)   pitch_bend;
    LM_ATOMIC(int)   voice_count;
    LM_ATOMIC(int)   sustain;     /* CC64 sustain pedal: 1 = held */
} LuxMaskMidiState;

/* ============================================================================
 * LuxMaskConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   polyphony_enabled;
    int   background_mode;          /* LUX_MASK_BG_* */
    int   reference_note;           /* default 57 (A3) */
    int   coupling_mode;            /* LUX_MASK_COUPLING_* */
    float free_pixels_per_semitone;
    float pitch_bend_range;         /* in semitones */

    /* ── Spatial bandpass filter driven by the ADSR ─────────────────────────
     * Always a bandpass centred on the played note (keyboard tracking).  The
     * ADSR output (0..1, including Sustain) is the "openness": at 0 the band
     * collapses to nothing (so the release fades to background); at 1 the band
     * spans `filter_width_pct` of the image.
     *
     * `filter_offset_pct` shifts the band CENTRE away from the note, as a % of
     * the image and decoupled from the width:
     *     0   = band centred on the note,
     *    >0   = band centred above the note, <0 = below.
     * The offset is openness-scaled like the width, so at env == 0 the centre
     * sits on the note (band collapsed) and sweeps out to its offset as env
     * opens — a glide-like swept offset at the attack, independent of width.
     *
     *   W      = openness * filter_width_pct/100  * pixel_count
     *   centre = note + openness * filter_offset_pct/100 * pixel_count
     *   lo = centre - W/2,  hi = centre + W/2. */
    float filter_width_pct;  /* 0..100  — band width at full open, % of image */
    float filter_offset_pct; /* -100..100 — band-centre offset, % of image */
    float filter_slope;      /* 0..1 — edge steepness (1 = sharp, 0 = soft) */

    /* ADSR (drives the filter cutoff/openness) */
    float attack_ms;
    float decay_ms;
    float sustain_level;
    float release_ms;

    /* Per-segment curvature [-1,1] (0 = linear, >0 convex, <0 concave).
     * Drives lux_env_shape() for the attack / decay / release segments. */
    float attack_curve;
    float decay_curve;
    float release_curve;

    /* Glide */
    float glide_time_ms;

    /* LFO on position only (vibrato).
     * LFO on width was removed — too mechanical, breaks the gesture-driven
     * envelope feel of the spotlight. */
    float lfo_pos_rate_hz;
    float lfo_pos_depth_semitones;

    /* Velocity coupling (velocity → alpha peak) */
    int   velocity_coupling;
} LuxMaskConfig;

/* ============================================================================
 * LuxMaskState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxMaskConfig     config;
    LuxMaskMidiState  midi;

    LuxMaskVoiceState voices[LUX_MASK_MAX_VOICES];
    uint32_t          next_age;

    /* Modulators */
    float    lfo_pos_phase;
    uint64_t last_frame_ts_us;

    /* Last processed pixel count (image thread -> UI for the filter-response
     * overlay; plain int, tear-free enough for a display read). */
    int      last_pixel_count;

    /* Preallocated output buffers (mask applied per pixel). */
    uint8_t  out_r[LUX_MASK_MAX_PIXELS];
    uint8_t  out_g[LUX_MASK_MAX_PIXELS];
    uint8_t  out_b[LUX_MASK_MAX_PIXELS];

    /* Per-frame scratch (alpha accumulator across voices, in 0..1 fixed-point
     * stored as float for simplicity).  Reused each call, never published. */
    float    alpha_buf[LUX_MASK_MAX_PIXELS];
} LuxMaskState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void           lux_mask_init(LuxMaskState *state);
void           lux_mask_reset(LuxMaskState *state);
LuxMaskConfig  lux_mask_config_default(void);

/* ── MIDI helpers (audio thread, RT-safe) ──────────────────────────────────── */
void lux_mask_note_on(LuxMaskState *state, int note, float velocity);
void lux_mask_note_off(LuxMaskState *state, int note);
void lux_mask_set_pitch_bend(LuxMaskState *state, float bend); /* [-1, +1] */

/* CC64 sustain pedal. While held, note-offs are deferred (voices keep
 * shining); releasing the pedal releases every deferred voice through the
 * normal RELEASE envelope. RT-safe. */
void lux_mask_set_sustain(LuxMaskState *state, int on);

/* MIDI CC 123 "All Notes Off": mark every active voice as released.
 * Voices enter their normal RELEASE phase (exponential decay), so the
 * tail remains musical instead of being cut abruptly. RT-safe. */
void lux_mask_all_notes_off(LuxMaskState *state);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Apply the mask to one RGB frame.  The output is allocated inside `state`
 * (out_r/out_g/out_b).  When the engine is disabled or no voice is active the
 * input pointers are returned as-is.
 */
void lux_mask_process_frame(
    LuxMaskState   *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance (mirrors LuxPitch single-simulation model, M2) ────────── */
extern LuxMaskState g_lux_mask_proc;

#ifdef __cplusplus
}
#endif

#endif /* LUX_MASK_H */
