/*
 * synth_luxsynth_engine.h
 *
 * LuxSynth additive synthesis engine for VST integration.
 * Ported from legacy synth_luxsynth.c — pure C, RT-safe.
 *
 * Architecture:
 *   - Reads FFT magnitudes + pan positions from image pipeline (polyphonic path)
 *   - Generates additive synthesis: N sine oscillators per voice,
 *     weighted by FFT-derived spectral envelope
 *   - 8-voice polyphony with shared ADSR (volume + filter)
 *   - Lock-free: no allocation, no mutex, no I/O in hot path
 *
 * Author: Cline (ported from legacy)
 */

#ifndef SYNTH_LUXSYNTH_ENGINE_H
#define SYNTH_LUXSYNTH_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "synth_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * COMPILE-TIME LIMITS
 * ========================================================================== */

#define LUXSYNTH_MAX_VOICES          8
#define LUXSYNTH_MAX_OSCILLATORS   128   /* Maps to FFT bins */
#define LUXSYNTH_MAX_BUFFER_SIZE  4096

/* ============================================================================
 * OSCILLATOR STATE
 * ========================================================================== */

typedef struct {
    float phase;           /* Current phase [0, 2*PI) */
    float phase_increment; /* Phase step per sample */
} LuxSynthOscillator;

/* ============================================================================
 * PER-VOICE STATE
 * ========================================================================== */

typedef struct {
    /* Oscillator bank */
    LuxSynthOscillator oscillators[LUXSYNTH_MAX_OSCILLATORS];
    int num_oscillators;

    /* Voice identity */
    int   midi_note;                    /* -1 = inactive */
    uint8_t velocity;                   /* 0-127 */
    float frequency;                    /* Fundamental frequency (Hz) */
    unsigned long long trigger_order;   /* For LRU voice stealing */

    /* ADSR envelopes */
    AdsrEnvelope volume_env;
    AdsrEnvelope filter_env;

    /* State */
    bool active;
} LuxSynthVoice;

/* ============================================================================
 * SPECTRAL DATA (read from image pipeline, lock-free)
 * ========================================================================== */

typedef struct {
    float magnitudes[LUXSYNTH_MAX_OSCILLATORS];    /* FFT bin magnitudes [0,1] */
    float pan_positions[LUXSYNTH_MAX_OSCILLATORS];  /* Stereo pan per bin [-1,1] */
    float harmonicity[LUXSYNTH_MAX_OSCILLATORS];    /* Harmonicity per bin [0,1] */
    float left_gains[LUXSYNTH_MAX_OSCILLATORS];     /* Pre-computed left gain */
    float right_gains[LUXSYNTH_MAX_OSCILLATORS];    /* Pre-computed right gain */
    int   num_bins;                                 /* Number of valid bins */
} LuxSynthSpectralData;

/* ============================================================================
 * ENGINE CONFIGURATION (updated atomically from UI thread)
 * ========================================================================== */

typedef struct {
    /* ADSR volume envelope */
    float attack_ms;
    float decay_ms;
    float sustain_level;
    float release_ms;
    float attack_curve;          /* [-1,1] segment curvature, 0 = linear */
    float decay_curve;
    float release_curve;

    /* ADSR filter envelope */
    float filter_attack_ms;
    float filter_decay_ms;
    float filter_sustain;
    float filter_release_ms;
    float filter_attack_curve;   /* [-1,1] */
    float filter_decay_curve;
    float filter_release_curve;
    float filter_cutoff;        /* Base cutoff as fraction of Nyquist [0,1] */
    float filter_env_depth;     /* How much envelope modulates cutoff [0,1] */

    /* LFO */
    float lfo_rate_hz;
    float lfo_depth_semitones;

    /* Spectral parameters */
    /* (gamma removed 2026-07-12 — conditioning is per-OUT, pixel domain) */
    int   num_oscillators;      /* Active oscillators per voice (1-128) */

    /* Master */
    float master_volume;
    float sample_rate;
    int   buffer_size;

    /* Enable flag */
    bool  enabled;
} LuxSynthConfig;

/* ============================================================================
 * ENGINE STATE (opaque, preallocated)
 * ========================================================================== */

typedef struct {
    LuxSynthVoice voices[LUXSYNTH_MAX_VOICES];
    int num_voices;

    /* Global trigger counter for LRU voice stealing */
    unsigned long long current_trigger_order;

    /* Global LFO */
    LfoState global_lfo;

    /* Configuration (copied atomically from UI) */
    LuxSynthConfig config;

    /* Spectral data — RENDER copy, owned by the audio thread. Writers never
     * touch it: they stage into `spectral_pending` under the seqlock below,
     * and luxsynth_engine_process latches it at BLOCK START, ramping the
     * magnitudes across the block (a push must never step the waveform
     * mid-block — the unsynchronised memcpy did, audible as crackle at the
     * feed's ~250 Hz push rate with a moving image). */
    LuxSynthSpectralData spectral;

    /* Writer-side staging (luxsynth_feed_tick, synth thread). seqlock:
     * odd = writer inside; the render latch retries next block on a torn
     * read (spec_applied_seq tracks the last even seq actually applied). */
    LuxSynthSpectralData spectral_pending;
    volatile uint32_t    spec_pending_seq;
    uint32_t             spec_applied_seq;

    /* Sample rate cache */
    float sample_rate;
    float inv_sample_rate;

    /* Audio output buffers (preallocated) */
    float output_left[LUXSYNTH_MAX_BUFFER_SIZE];
    float output_right[LUXSYNTH_MAX_BUFFER_SIZE];

    /* Initialization flag */
    bool initialized;
} LuxSynthEngine;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initialize the LuxSynth engine. Call once at startup.
 * @param engine  Preallocated engine state
 * @param sample_rate  Audio sample rate (Hz)
 * @param buffer_size  Audio buffer size (samples)
 * @return 0 on success, -1 on failure
 */
int luxsynth_engine_init(LuxSynthEngine *engine, float sample_rate, int buffer_size);

/**
 * @brief Reset engine state (all voices off, counters zeroed).
 */
void luxsynth_engine_reset(LuxSynthEngine *engine);

/**
 * @brief Update engine configuration from UI thread.
 * @note Call from main thread; engine reads atomically from audio thread.
 */
void luxsynth_engine_set_config(LuxSynthEngine *engine, const LuxSynthConfig *config);

/**
 * @brief Update spectral data from image pipeline.
 * @note Call from pipeline thread; engine reads snapshot in process().
 */
void luxsynth_engine_set_spectral_data(LuxSynthEngine *engine,
                                        const float *magnitudes,
                                        const float *pan_positions,
                                        const float *harmonicity,
                                        const float *left_gains,
                                        const float *right_gains,
                                        int num_bins);

/**
 * @brief Process one audio buffer (RT hot path).
 * @param engine  Engine state
 * @param num_samples  Number of samples to generate
 * @param out_left   Output left channel (preallocated, num_samples floats)
 * @param out_right  Output right channel (preallocated, num_samples floats)
 *
 * @note RT-SAFE: No allocation, no lock, no I/O. O(voices * oscillators * samples).
 */
void luxsynth_engine_process(LuxSynthEngine *engine, int num_samples,
                              float *out_left, float *out_right);

/**
 * @brief Handle MIDI Note On. RT-safe.
 * @return Voice index allocated, or -1 if failed.
 */
int luxsynth_engine_note_on(LuxSynthEngine *engine, uint8_t note, uint8_t velocity);

/**
 * @brief Handle MIDI Note Off. RT-safe.
 * @return Voice index released, or -1 if not found.
 */
int luxsynth_engine_note_off(LuxSynthEngine *engine, uint8_t note);

/**
 * @brief Release every non-idle voice (ADSR release, click-free). RT-safe;
 * call from the audio thread. Used when the engine's last enabled OUT send
 * goes away, so voices drain before the render gate closes.
 */
void luxsynth_engine_all_notes_off(LuxSynthEngine *engine);

/**
 * @brief Convert MIDI note to frequency (Hz). RT-safe.
 */
float luxsynth_midi_to_freq(uint8_t note);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_LUXSYNTH_ENGINE_H */
