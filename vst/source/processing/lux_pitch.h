/*
 * lux_pitch.h
 *
 * LuxPitch — MIDI-driven image line shifter for pitch control.
 *
 * Supports monophonic and polyphonic modes (up to 10 simultaneous voices).
 * Polyphonic blend: white bg → min (darkest wins), black bg → max (brightest wins).
 *
 * RT-safety: All functions are pure C, allocation-free, bounded O(N * voices).
 *            No JUCE dependencies. No mutex/lock.
 *            MIDI state is communicated via C11 atomics (audio → image thread).
 *
 * Author: zhonx
 * Created: 2026-04-16
 */

#ifndef LUX_PITCH_H
#define LUX_PITCH_H

#include <stdint.h>

#ifdef __cplusplus
  #include <atomic>
  #define LP_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define LP_ATOMIC(T) _Atomic T
#endif

/* Maximum buffer size (> 6912 for 400 DPI) */
#define LUX_PITCH_MAX_PIXELS 8192

/* Maximum polyphonic voices */
#define LUX_PITCH_MAX_VOICES 10

/* Background mode */
#define LUX_PITCH_BG_BLACK  0
#define LUX_PITCH_BG_WHITE  1

/* Step coupling mode */
#define LUX_PITCH_COUPLING_LUXSTRAL  0
#define LUX_PITCH_COUPLING_FREE      1

/* ADSR envelope stages */
#define LUX_PITCH_ENV_IDLE     0
#define LUX_PITCH_ENV_ATTACK   1
#define LUX_PITCH_ENV_DECAY    2
#define LUX_PITCH_ENV_SUSTAIN  3
#define LUX_PITCH_ENV_RELEASE  4

/* ============================================================================
 * LuxPitchVoiceMidi — Per-voice atomic MIDI state (audio → image thread)
 * ============================================================================ */
typedef struct {
    LP_ATOMIC(int)   active;        /* 1 = note held, 0 = released (in release phase or idle) */
    LP_ATOMIC(int)   note;          /* MIDI note number (0-127) */
    LP_ATOMIC(int)   velocity;      /* MIDI velocity (0-127) */
    LP_ATOMIC(int)   retrigger;     /* 1 = voice was stolen while active: force re-ATTACK */
    LP_ATOMIC(int)   sustained;     /* 1 = note released while sustain pedal (CC64) held */
} LuxPitchVoiceMidi;

/* ============================================================================
 * LuxPitchVoiceState — Per-voice runtime state (image thread only)
 * ============================================================================ */
typedef struct {
    int      note;              /* Latched note number (image thread copy) */
    float    velocity_norm;     /* Latched normalised velocity [0,1] */
    float    current_shift;     /* Smoothed shift in pixels (with glide) */
    float    target_shift;      /* Target shift from note + bend */
    float    envelope_level;    /* Current ADSR level [0, 1] */
    int      envelope_stage;    /* LUX_PITCH_ENV_* */
    float    env_phase;         /* Normalised progress [0,1] through current segment */
    float    seg_start_level;   /* Envelope level captured at segment entry (click-free) */
    int      prev_active;       /* Previous active state (edge detection) */
    uint32_t age;               /* Incremented each note-on (for voice stealing) */
} LuxPitchVoiceState;

/* ============================================================================
 * LuxPitchMidiState — Global atomic state (audio → image thread)
 * ============================================================================ */
typedef struct {
    LuxPitchVoiceMidi voices[LUX_PITCH_MAX_VOICES];
    LP_ATOMIC(int)    pitch_bend;   /* Pitch bend (-8192..+8191), center = 0 */
    LP_ATOMIC(int)    voice_count;  /* Number of currently allocated voices (informational) */
    LP_ATOMIC(int)    sustain;      /* CC64 sustain pedal: 1 = held */
} LuxPitchMidiState;

/* ============================================================================
 * LuxPitchConfig — Parameters synced from APVTS
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   polyphony_enabled;        /* 0 = mono (legacy), 1 = poly (up to MAX_VOICES) */
    int   background_mode;          /* LUX_PITCH_BG_BLACK or _WHITE */
    int   reference_note;           /* MIDI note for zero shift (default 57 = A3) */
    int   coupling_mode;            /* COUPLING_LUXSTRAL or COUPLING_FREE */
    float free_pixels_per_semitone; /* User-defined step size (free mode) */
    float pitch_bend_range;         /* Pitch bend range in semitones (default 2) */

    /* ADSR envelope (ms) */
    float attack_ms;
    float decay_ms;
    float sustain_level;            /* [0.0, 1.0] */
    float release_ms;

    /* Per-segment curvature [-1,1] (0 = linear, >0 convex, <0 concave).
     * Drives lux_env_shape() for the attack / decay / release segments. */
    float attack_curve;
    float decay_curve;
    float release_curve;

    /* Glide (portamento) */
    float glide_time_ms;

    /* LFO (vibrato) */
    float lfo_rate_hz;
    float lfo_depth_semitones;

    /* Velocity coupling */
    int   velocity_coupling;
} LuxPitchConfig;

/* ============================================================================
 * LuxPitchState — Complete runtime state
 * ============================================================================ */
typedef struct {
    LuxPitchConfig     config;
    LuxPitchMidiState  midi;

    /* Per-voice runtime state (image thread only) */
    LuxPitchVoiceState voices[LUX_PITCH_MAX_VOICES];
    uint32_t           next_age;       /* Monotonic counter for voice stealing */

    /* Global runtime state */
    float    lfo_phase;          /* LFO phase [0, 2*PI] */
    uint64_t last_frame_ts_us;   /* Timestamp of last frame for dt */

    /* Preallocated output buffers */
    uint8_t  out_r[LUX_PITCH_MAX_PIXELS];
    uint8_t  out_g[LUX_PITCH_MAX_PIXELS];
    uint8_t  out_b[LUX_PITCH_MAX_PIXELS];
} LuxPitchState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

void lux_pitch_init(LuxPitchState *state);
void lux_pitch_reset(LuxPitchState *state);
LuxPitchConfig lux_pitch_config_default(void);

/* ── MIDI event helpers (called from audio thread — RT-safe) ───────────────── */

void lux_pitch_note_on(LuxPitchState *state, int note, float velocity);
void lux_pitch_note_off(LuxPitchState *state, int note);
void lux_pitch_set_pitch_bend(LuxPitchState *state, float bend);

/* CC64 sustain pedal. While held, note-offs are deferred (voices keep
 * sounding); releasing the pedal releases every deferred voice through the
 * normal RELEASE envelope. RT-safe. */
void lux_pitch_set_sustain(LuxPitchState *state, int on);

/* MIDI CC 123 "All Notes Off": mark every active voice as released.
 * Voices enter their normal RELEASE phase (exponential decay), so the
 * tail remains musical instead of being cut abruptly. RT-safe. */
void lux_pitch_all_notes_off(LuxPitchState *state);

/* ── Frame processing ──────────────────────────────────────────────────────── */

void lux_pitch_process_frame(
    LuxPitchState    *state,
    const uint8_t    *in_r,
    const uint8_t    *in_g,
    const uint8_t    *in_b,
    int               pixel_count,
    int               luxstral_num_octaves,
    const uint8_t   **out_r,
    const uint8_t   **out_g,
    const uint8_t   **out_b
);

/* ── Global instance ───────────────────────────────────────────────────────── */
/*
 * Single simulation (M2 refactor): the synthesis-thread instance is the only
 * authoritative state.  Visual consumers (CIS visualizer, video waterfall)
 * read the insert taps published by the chain executor
 * (audio_image_buffers_get_insert_tap_pointers) — they never re-simulate, so
 * what you SEE is exactly what the engines CONSUME.
 */
extern LuxPitchState g_lux_pitch_proc;

#ifdef __cplusplus
}
#endif

#endif /* LUX_PITCH_H */
