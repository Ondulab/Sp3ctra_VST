/*
 * wave_generation.h
 *
 *  Created on: 24 avr. 2019
 *      Author: zhonx
 *
 * REFACTORED: Single shared power-of-2 sine table replaces per-comma tables.
 * All oscillators read from g_sine_table[SINE_TABLE_SIZE], which fits entirely
 * in L1 cache (4 KB), eliminating cache misses from the former ~760 KB multi-table design.
 *
 * phase_inc per note = frequency × SINE_TABLE_SIZE / Fs
 * phase_acc ∈ [0, SINE_TABLE_SIZE), float, enables sub-sample linear interpolation
 *
 * THD with linear interpolation (N=1024): < −107 dB per oscillator
 * Total noise floor (3456 osc, RMS sum): < −72 dB — well below perceptual threshold
 */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __WAVE_GENERATION_H
#define __WAVE_GENERATION_H

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include "synth_luxstral.h"
#include <stdint.h>
#include <math.h>

/* Private includes ----------------------------------------------------------*/

/* Exported types ------------------------------------------------------------*/
typedef enum
{
    MAJOR,
    MINOR,
} harmonizationType;

struct waveParams
{
    uint32_t commaPerSemitone;
    uint32_t startFrequency;
    harmonizationType harmonization;
    uint32_t harmonizationLevel;
    uint32_t waveformOrder;
};

/*
 * Wave oscillator descriptor.
 *
 * Each note has an independent float phase accumulator reading into the global
 * shared sine table g_sine_table[SINE_TABLE_SIZE].
 *
 * phase_inc = frequency × SINE_TABLE_SIZE / Fs
 *   → same formula regardless of frequency range or octave
 *   → linear interpolation between g_sine_table[i0] and g_sine_table[i1]
 *     gives sub-sample phase accuracy with < −107 dB THD per oscillator
 */
struct wave
{
    /* Phase accumulator — position in g_sine_table[], ∈ [0, SINE_TABLE_SIZE) */
    float phase_acc;
    /* Phase increment per output sample: frequency × SINE_TABLE_SIZE / Fs    */
    float phase_inc;

    /* Volume envelope */
    float target_volume;
    float current_volume;

    /* GAP_LIMITER: Precomputed envelope coefficients (RT-optimized) */
    float alpha_up;                  /* Attack coefficient (precomputed)                    */
    float alpha_down_weighted;       /* Release coefficient with frequency weighting         */

    /* Physiological (equal-loudness) gain — precomputed at init, RMS-normalized */
    float physiological_gain;

    /* Note frequency in Hz */
    float frequency;

    /* Stereo panoramization */
    float pan_position;   /* Pan position: −1.0 (left) to +1.0 (right) */
    float left_gain;      /* Left channel gain (0.0 to 1.0)             */
    float right_gain;     /* Right channel gain (0.0 to 1.0)            */
};

/* Exported constants --------------------------------------------------------*/

/*
 * Shared sine table — power-of-2 size for O(1) wrap via bitmask.
 * 1024 entries × 4 bytes = 4 KB → fits entirely in L1 cache (typically 32 KB).
 * Increasing to 2048 improves THD by ~12 dB at cost of 8 KB (still L1-resident).
 */
#define SINE_TABLE_SIZE   1024
#define SINE_TABLE_MASK   (SINE_TABLE_SIZE - 1)   /* = 1023 — for (i0+1) & MASK wrap */

/* Amplitude resolution for the sine table (matching legacy WAVE_AMP_RESOLUTION) */
#ifndef WAVE_AMP_RESOLUTION
#define WAVE_AMP_RESOLUTION  1.0f
#endif

/* Global shared sine table (initialized once by init_sine_table()) */
extern float g_sine_table[SINE_TABLE_SIZE];

/*
 * Waveform morphing tables — same layout as g_sine_table, precomputed once.
 *
 * g_square_table[k]   = bandlimited square wave at phase k/SINE_TABLE_SIZE
 *                       4/π × Σ sin((2n+1)×2πk/N)/(2n+1), n=0..WAVETABLE_HARMONICS-1
 *
 * Blend at RT: sample = lerp(g_sine_table[idx], g_square_table[idx], g_waveform_morph)
 * No wavetable rebuild needed — just change g_waveform_morph (written by preprocessor,
 * read by RT workers with relaxed atomics; single float, ARM64-naturally-atomic).
 */
#define WAVETABLE_HARMONICS  16   /* number of odd harmonics for square wave */

extern float g_square_table[SINE_TABLE_SIZE];

/*
 * Waveform morph factor — written by StrokeForge preprocessor, read by RT workers.
 * 0.0f = pure sine  |  1.0f = pure square
 * Updated non-RT (preprocessor thread) via __atomic_store_n / memory_order_relaxed.
 * Read RT via  __atomic_load_n  / memory_order_relaxed (single float, coherent on ARM64).
 */
extern volatile float g_waveform_morph;

/* Global wave array and waveParams (allocated in synth_luxstral_runtime.c) */
extern volatile struct wave *waves;

/* Exported macro ------------------------------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/

/**
 * @brief Initialize the global shared sine table.
 *
 * Must be called once before any synthesis (in synth_IfftInit).
 * Fills g_sine_table[i] = sin(2π × i / SINE_TABLE_SIZE) × WAVE_AMP_RESOLUTION.
 * O(SINE_TABLE_SIZE) — runs once, not in RT path.
 */
void init_sine_table(void);

/**
 * @brief Initialize wave descriptors for all notes.
 *
 * Computes frequency and phase_inc for each oscillator using logarithmic
 * distribution between low_frequency and high_frequency from g_sp3ctra_config.
 * Does NOT allocate any memory (shared sine table already initialized by init_sine_table).
 *
 * @param waves     Pointer to dynamically allocated wave array (from synth_runtime)
 * @param parameters Legacy waveParams (startFrequency / commaPerSemitone used for
 *                   hot-reload compatibility only — actual frequencies come from
 *                   g_sp3ctra_config.low_frequency / high_frequency)
 */
void init_waves(volatile struct wave *waves,
                volatile struct waveParams *parameters);

/**
 * @brief Compute physiological (equal-loudness) gain compensation for a given frequency.
 *
 * Uses inverse A-weighting (IEC 61672:2003) to compensate for human hearing sensitivity.
 * Boosts frequencies where the ear is less sensitive (bass, extreme treble).
 *
 * @param frequency_hz Frequency in Hz (must be > 0)
 * @return Gain factor (1.0 = no change at 1 kHz reference)
 */
float compute_physiological_gain(float frequency_hz);

/**************************************************************************************
 * Hot-reload frequency range API
 * Thread-safe mechanism for changing frequency range at runtime
 **************************************************************************************/

/** @brief Request frequency range reinit from UI thread (thread-safe). */
void request_frequency_reinit(void);

/**
 * @brief Check and process pending frequency reinit.
 * Called at the beginning of synth_IfftMode() BEFORE workers start.
 * @return 1 if reinit was performed, 0 otherwise
 */
int check_and_process_frequency_reinit(void);

/**
 * @brief Force-reset the frequency reinit state machine to IDLE.
 * Call before request_frequency_reinit() after the audio thread has been stopped.
 */
void reset_frequency_reinit_state(void);

/** @brief Check if frequency reinit is currently in progress (fading out). */
int is_frequency_reinit_fading_out(void);

/**************************************************************************************
 * Global fade API for smooth transitions
 **************************************************************************************/

/**
 * @brief Get and update global fade factor for a single sample.
 * Call once per output sample. Returns value ∈ [0.0, 1.0].
 */
float get_global_fade_factor_and_update(void);

/** @brief Get current global fade factor without updating. */
float get_global_fade_factor(void);

/* Exported extern -----------------------------------------------------------*/
extern volatile struct waveParams wavesGeneratorParams;

/* Private defines -----------------------------------------------------------*/

#endif /* __WAVE_GENERATION_H */
