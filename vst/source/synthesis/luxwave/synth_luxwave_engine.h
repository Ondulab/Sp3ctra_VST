/*
 * synth_luxwave_engine.h
 *
 * LuxWave dynamic optical wavetable synthesis engine for VST integration.
 * Ported from legacy synth_luxwave.c — pure C, RT-safe.
 *
 * Architecture:
 *   - Reads pixel luminance from the LuxSynth preprocessed grayscale line
 *   - Each image line becomes one period of a waveform (wavetable)
 *   - MIDI controls pitch (playback speed), scan mode shapes timbre
 *   - 8-voice polyphony with ADSR (volume + filter) and LFO vibrato
 *   - Lock-free double-buffer for wavetable data (no race condition)
 *   - Crossfade on wavetable update to prevent discontinuities
 */

#ifndef SYNTH_LUXWAVE_ENGINE_H
#define SYNTH_LUXWAVE_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * COMPILE-TIME LIMITS
 * ========================================================================== */

#define LUXWAVE_MAX_VOICES        8
#define LUXWAVE_MAX_PIXELS     4096
#define LUXWAVE_MAX_BUFFER_SIZE 4096
#define LUXWAVE_MAX_FREQUENCY 12000.0f
#define LUXWAVE_DEFAULT_AMPLITUDE  0.5f
#define LUXWAVE_MIN_AUDIBLE     0.001f

/* Crossfade length in samples when wavetable content changes */
#define LUXWAVE_CROSSFADE_SAMPLES 128

/* ============================================================================
 * ENUMERATIONS
 * ========================================================================== */

typedef enum {
    LUXWAVE_SCAN_LEFT_TO_RIGHT = 0,
    LUXWAVE_SCAN_RIGHT_TO_LEFT = 1,
    LUXWAVE_SCAN_DUAL          = 2
} LuxWaveScanMode;

/* Self-contained ADSR stages (no dependency on synth_common.h) */
typedef enum {
    LW_ADSR_IDLE = 0,
    LW_ADSR_ATTACK,
    LW_ADSR_DECAY,
    LW_ADSR_SUSTAIN,
    LW_ADSR_RELEASE
} LwAdsrStage;

/* ============================================================================
 * PER-VOICE ADSR ENVELOPE
 * ========================================================================== */

typedef struct {
    LwAdsrStage stage;
    float       level;
} LwAdsrEnv;

/* ============================================================================
 * PER-VOICE LOWPASS FILTER
 * ========================================================================== */

typedef struct {
    float prev_output;
} LuxWaveLowpass;

/* ============================================================================
 * PER-VOICE STATE
 * ========================================================================== */

typedef struct {
    float    phase;
    float    frequency;
    int      midi_note;
    uint8_t  velocity;
    bool     active;
    unsigned long long trigger_order;

    LwAdsrEnv volume_env;
    LwAdsrEnv filter_env;
    LuxWaveLowpass lowpass;
} LuxWaveVoice;

/* ============================================================================
 * ENGINE CONFIGURATION
 * ========================================================================== */

typedef struct {
    /* Volume ADSR */
    float attack_ms;
    float decay_ms;
    float sustain_level;
    float release_ms;

    /* Filter ADSR */
    float filter_attack_ms;
    float filter_decay_ms;
    float filter_sustain;
    float filter_release_ms;
    float filter_cutoff_hz;
    float filter_env_depth_hz;

    /* LFO */
    float lfo_rate_hz;
    float lfo_depth_semitones;

    /* Scan */
    LuxWaveScanMode scan_mode;
    float amplitude;

    /* Master */
    float sample_rate;
    int   buffer_size;
    bool  enabled;
} LuxWaveConfig;

/* ============================================================================
 * ENGINE STATE
 * ========================================================================== */

typedef struct {
    LuxWaveVoice voices[LUXWAVE_MAX_VOICES];
    unsigned long long current_trigger_order;

    /* Global LFO */
    float lfo_phase;

    /* Configuration */
    LuxWaveConfig config;

    /*
     * Double-buffered wavetable: prevents race conditions between
     * the image processing thread (writer) and the audio RT thread (reader).
     *
     * wt_buf[0] and wt_buf[1] hold local copies of the image line.
     * wt_write_idx:  atomic — which buffer the writer fills next.
     * wt_read_idx:   which buffer the RT thread currently reads from.
     * wt_new_ready:  atomic flag — set by writer after swap, cleared by RT thread.
     *
     * Crossfade: when RT detects new data, it crossfades over
     * LUXWAVE_CROSSFADE_SAMPLES from old to new buffer to avoid clicks.
     */
    float wt_buf[2][LUXWAVE_MAX_PIXELS];
    int   wt_pixel_count[2];
    atomic_int wt_write_idx;     /* writer toggles this after memcpy */
    int        wt_read_idx;      /* RT thread's current read buffer */
    atomic_int wt_new_ready;     /* flag: 1 = new data available     */
    int        xfade_remaining;  /* samples left in crossfade        */
    int        xfade_old_idx;    /* buffer index of old wavetable    */

    /* Audio parameters */
    float sample_rate;
    float inv_sample_rate;
    float f_min;
    float f_max;

    /* Preallocated output buffers */
    float output_left[LUXWAVE_MAX_BUFFER_SIZE];
    float output_right[LUXWAVE_MAX_BUFFER_SIZE];

    bool initialized;
} LuxWaveEngine;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

int  luxwave_engine_init(LuxWaveEngine *engine, float sample_rate, int buffer_size);
void luxwave_engine_reset(LuxWaveEngine *engine);
void luxwave_engine_set_config(LuxWaveEngine *engine, const LuxWaveConfig *config);

/**
 * Set the wavetable image line (called from image processing thread).
 * Data is COPIED into an internal double-buffer — no pointer aliasing.
 */
void luxwave_engine_set_image_line(LuxWaveEngine *engine,
                                    const float *image_line,
                                    int pixel_count);

void luxwave_engine_process(LuxWaveEngine *engine, int num_samples,
                             float *out_left, float *out_right);

int  luxwave_engine_note_on(LuxWaveEngine *engine, uint8_t note, uint8_t velocity);
int  luxwave_engine_note_off(LuxWaveEngine *engine, uint8_t note);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_LUXWAVE_ENGINE_H */
