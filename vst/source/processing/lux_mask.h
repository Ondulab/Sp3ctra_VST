/*
 * lux_mask.h
 *
 * LuxMask — MIDI-driven mobile spotlight ("synesthetic EQ") for a stable image.
 *
 * Mirrors LuxPitch ownership patterns (3 global instances: visualizer / proc /
 * video) and voice-management rules, but instead of translating the image, it
 * reveals it through a mobile, shape-controlled mask whose center is driven by
 * MIDI note, width by env + LFO, and intensity by ADSR + velocity.
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

/* Internal LUT size for gauss shape pre-computation.
 * Indexed by d ∈ [0, 4] (beyond that, shape ≈ 0). */
#define LUX_MASK_LUT_SIZE 1024

/* ============================================================================
 * LuxMaskVoiceMidi — Per-voice atomic MIDI state (audio → image thread).
 * ============================================================================ */
typedef struct {
    LM_ATOMIC(int) active;
    LM_ATOMIC(int) note;
    LM_ATOMIC(int) velocity;
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
    int      prev_active;
    uint32_t age;
    /* Snapshots for width-bloom envelope: */
    float    peak_level;         /* Envelope peak at attack (velocity-scaled). */
    float    release_start_level;/* Envelope level at the moment release began. */

    /* Time progress *per segment* (in seconds, normalised by segment duration
     * to obtain t ∈ [0, 1]).  Used by the width interpolation so that the
     * Width @ Attack → Width transition (DECAY segment) and the Width →
     * Width @ Release transition (RELEASE segment) follow the *time* axis of
     * the segment, NOT the audio amplitude.
     *
     * This decouples the width modulation from the Sustain Level — a high
     * sustain no longer shrinks the visible width transition. */
    float    decay_progress_s;
    float    release_progress_s;
} LuxMaskVoiceState;

/* ============================================================================
 * LuxMaskMidiState — Global atomic MIDI state.
 * ============================================================================ */
typedef struct {
    LuxMaskVoiceMidi voices[LUX_MASK_MAX_VOICES];
    LM_ATOMIC(int)   pitch_bend;
    LM_ATOMIC(int)   voice_count;
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

    /* Mask shape — gauss only (other shapes had no perceptible musical impact). */
    float width_base;               /* base width in pixels (8..8192) — width during SUSTAIN */

    /* ADSR (acts on alpha) */
    float attack_ms;
    float decay_ms;
    float sustain_level;
    float release_ms;

    /* Width horizons — absolute widths in pixels, fully ADSR-driven.
     *
     *   width_attack_px  : 8..8192 — width reached at note-on (during ATTACK stage).
     *                                Then collapses to width_base during DECAY.
     *                                Can be larger or smaller than width_base.
     *
     *   width_release_px : 8..8192 — width reached at full release (envelope = 0).
     *                                During RELEASE width grows from width_base to
     *                                width_release_px following the envelope decay.
     *                                Can be larger or smaller than width_base.
     *
     * Velocity coupling:
     *   When velocity_coupling is ON the *effective* horizon is pondered between
     *   width_base (velocity=0) and the configured horizon (velocity=1):
     *     w_attack_eff  = lerp(width_base, width_attack_px,  velocity)
     *     w_release_eff = lerp(width_base, width_release_px, velocity)
     *   When velocity_coupling is OFF the configured horizons are used as-is.
     */
    float width_attack_px;
    float width_release_px;

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

    /* Gauss shape LUT (precomputed at init, indexed by d = |i - pos| / width,
     * clamped to [0, 4]). */
    float    shape_lut_gauss[LUX_MASK_LUT_SIZE];

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

/* ── Global instances (mirror LuxPitch ownership pattern) ──────────────────── */
extern LuxMaskState g_lux_mask;
extern LuxMaskState g_lux_mask_proc;
extern LuxMaskState g_lux_mask_vid;

#ifdef __cplusplus
}
#endif

#endif /* LUX_MASK_H */
