/*
 * wave_generation.c
 *
 *  Created on: 24 avr. 2019
 *      Author: zhonx
 *
 * REFACTORED: Single shared power-of-2 sine table (g_sine_table).
 *
 * All oscillators share one 4 KB table (SINE_TABLE_SIZE = 1024 entries).
 * The table fits entirely in L1 cache, eliminating the cache-miss overhead
 * of the former per-comma multi-table design (~760 KB scattered across memory).
 *
 * For each note: phase_inc = frequency × SINE_TABLE_SIZE / Fs
 * Linear interpolation in the hot path achieves < −107 dB THD per oscillator.
 */

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include "wave_generation.h"
#include "synth_luxstral_algorithms.h"
#include "config_synth_luxstral.h"
#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include <stdatomic.h>

#include "logger.h"
#include "wave_generation.h"

#define PI (3.14159265358979323846)

/* Global definitions --------------------------------------------------------*/

/** Shared sine table — 4 KB, initialized once, read-only in RT path */
float g_sine_table[SINE_TABLE_SIZE];

/* Bandlimited square wave table — precomputed once at startup (see init_sine_table()) */
float g_square_table[SINE_TABLE_SIZE];

/*
 * Waveform morph factor: 0.0 = pure sine, 1.0 = pure square.
 * Written by StrokeForge preprocessor (non-RT), read by synthesis workers (RT).
 * volatile + relaxed atomic semantics: single float, naturally atomic on ARM64/x86-64.
 */

/** Legacy waveParams instance (kept for hot-reload compatibility) */
volatile struct waveParams wavesGeneratorParams;

/** Global waves pointer (allocated in synth_luxstral_runtime.c) */
volatile struct wave *waves = NULL;

/**************************************************************************************
 * Hot-reload frequency range — simplified state machine
 * Only 2 states: IDLE and PENDING (regeneration happens immediately when safe)
 * Global fade provides smooth transitions automatically.
 **************************************************************************************/
typedef enum
{
    FREQ_REINIT_IDLE = 0,
    FREQ_REINIT_PENDING,
} freq_reinit_state_t;

static _Atomic int g_freq_reinit_state = FREQ_REINIT_IDLE;

/* Global fade coefficient (0.0 to 1.0) — applied to entire output signal */
static float g_global_fade_current = 1.0f;
static float g_global_fade_target  = 1.0f;

/* Exponential fade alpha — computed dynamically from actual sample rate (50 ms tau) */
static float get_global_fade_alpha(void)
{
    float Fs = (float)g_sp3ctra_config.sampling_frequency;
    if (Fs < 8000.0f)
        Fs = 48000.0f;
    const float tau_s = 0.05f;
    return 1.0f - expf(-1.0f / (tau_s * Fs));
}
#define GLOBAL_FADE_ALPHA get_global_fade_alpha()

/* Private function prototypes -----------------------------------------------*/
static float calculate_frequency_for_note(int note, int total_notes,
                                           float low_freq, float high_freq);

/* Private user code ---------------------------------------------------------*/

/**
 * @brief Calculate frequency for a note using logarithmic distribution.
 *
 * Formula: freq = low_freq × (high_freq / low_freq)^(note / (total_notes − 1))
 * Gives independent control of both frequency bounds.
 */
static float calculate_frequency_for_note(int note, int total_notes,
                                           float low_freq, float high_freq)
{
    if (total_notes <= 1)
        return low_freq;
    float ratio     = (float)note / (float)(total_notes - 1);
    float frequency = low_freq * powf(high_freq / low_freq, ratio);
    return frequency;
}

/**************************************************************************************
 * Physiological (Equal-Loudness) Compensation
 * Based on inverse A-weighting (IEC 61672:2003)
 **************************************************************************************/

static float compute_a_weighting_ra(float f)
{
    float f2  = f * f;
    float f4  = f2 * f2;
    float num = 12194.0f * 12194.0f * f4;
    float d1  = f2 + 20.6f  * 20.6f;
    float d2  = f2 + 107.7f * 107.7f;
    float d3  = f2 + 737.9f * 737.9f;
    float d4  = f2 + 12194.0f * 12194.0f;
    float den = d1 * sqrtf(d2 * d3) * d4;
    if (den < 1e-30f)
        return 0.0f;
    return num / den;
}

float compute_physiological_gain(float frequency_hz)
{
    if (frequency_hz <= 0.0f)
        return 1.0f;
    if (frequency_hz < 20.0f)
        frequency_hz = 20.0f;
    if (frequency_hz > 20000.0f)
        frequency_hz = 20000.0f;

    float ra_f  = compute_a_weighting_ra(frequency_hz);
    float ra_1k = compute_a_weighting_ra(1000.0f);
    if (ra_f < 1e-30f || ra_1k < 1e-30f)
        return 1.0f;

    float a_db = 20.0f * log10f(ra_f / ra_1k);

    float depth = g_sp3ctra_config.physiological_correction_depth;
    if (depth < 0.0f)
        depth = 0.0f;
    if (depth > 1.0f)
        depth = 1.0f;

    float corrected_db = -a_db * depth;
    if (corrected_db > 15.0f)
        corrected_db = 15.0f;
    if (corrected_db < -15.0f)
        corrected_db = -15.0f;

    return powf(10.0f, corrected_db / 20.0f);
}

/**************************************************************************************
 * Shared Sine Table Initialization
 **************************************************************************************/

/**
 * @brief Initialize the global shared sine table.
 *
 * g_sine_table[i] = sin(2π × i / SINE_TABLE_SIZE) × WAVE_AMP_RESOLUTION
 *
 * Called once at startup (synth_IfftInit), never in the RT path.
 * 4 KB total → fits in L1 cache and remains resident across all audio callbacks.
 */
void init_sine_table(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++)
    {
        g_sine_table[i] = sinf(2.0f * (float)PI * (float)i / (float)SINE_TABLE_SIZE)
                          * (float)WAVE_AMP_RESOLUTION;
        /* Square wave: bandlimited sum of odd harmonics.
         * Normalised so peak ≈ 1.0 (same scale as g_sine_table).
         * 4/π × Σ_{n=0..WAVETABLE_HARMONICS-1} sin((2n+1)×θ)/(2n+1)
         * At N=16 harmonics the Gibbs ripple is ~9 % but the waveform is clearly square.
         */
        {
            double theta = 2.0 * 3.14159265358979323846 * i / SINE_TABLE_SIZE;
            double sq = 0.0;
            int n;
            for (n = 0; n < WAVETABLE_HARMONICS; n++)
            {
                int h = 2 * n + 1;
                sq += sin(h * theta) / h;
            }
            g_square_table[i] = (float)(sq * 4.0 / 3.14159265358979323846)
                                 * (float)WAVE_AMP_RESOLUTION;
        }
    }
    log_info("SYNTH", "Sine table initialized: %d entries, %.0f bytes, L1-resident",
             SINE_TABLE_SIZE, (float)(SINE_TABLE_SIZE * sizeof(float)));
}

/**************************************************************************************
 * Wave Descriptor Initialization
 **************************************************************************************/

/**
 * @brief Initialize wave descriptors for all notes.
 *
 * For each note:
 *   frequency = logarithmic distribution between low_freq and high_freq
 *   phase_inc = frequency × SINE_TABLE_SIZE / Fs
 *
 * No memory allocation — the global g_sine_table is the only waveform storage.
 * Physiological gains are optionally computed and RMS-normalized.
 */
void init_waves(volatile struct wave *waves,
                volatile struct waveParams *parameters)
{
    (void)parameters; /* Actual frequencies come from g_sp3ctra_config */

    const int   total_notes  = get_current_number_of_notes();
    const float low_freq     = g_sp3ctra_config.low_frequency;
    const float high_freq    = g_sp3ctra_config.high_frequency;
    const int   sample_rate  = g_sp3ctra_config.sampling_frequency;

    /* Precomputed factor: phase_inc = freq × (SINE_TABLE_SIZE / Fs) */
    const float phase_inc_factor = (float)SINE_TABLE_SIZE / (float)sample_rate;

    log_info("SYNTH", "---------- WAVES INIT (shared sine table) ----------");
    log_info("SYNTH", "Freq range: %.1f - %.1f Hz, %d notes, Fs=%d Hz",
             low_freq, high_freq, total_notes, sample_rate);
    log_info("SYNTH", "Sine table: %d entries (%.0f bytes) — L1 resident",
             SINE_TABLE_SIZE, (float)(SINE_TABLE_SIZE * sizeof(float)));

    /* Diagnostic: log frequency mapping at key positions */
    log_info("SYNTH", "Frequency mapping check:");
    int test_pos[] = {0, total_notes / 4, total_notes / 2, (3 * total_notes) / 4, total_notes - 1};
    for (int ti = 0; ti < 5; ti++)
    {
        int test_note = test_pos[ti];
        if (test_note >= 0 && test_note < total_notes)
        {
            float tf = calculate_frequency_for_note(test_note, total_notes, low_freq, high_freq);
            log_info("SYNTH", "  Note %4d/%d (%.0f%%) → %.2f Hz, phase_inc=%.5f",
                     test_note, total_notes,
                     100.0f * test_note / (float)(total_notes > 1 ? total_notes - 1 : 1),
                     tf, tf * phase_inc_factor);
        }
    }

    /* Initialize all oscillator descriptors */
    for (int note = 0; note < total_notes; note++)
    {
        float freq = calculate_frequency_for_note(note, total_notes, low_freq, high_freq);

        waves[note].frequency          = freq;
        waves[note].phase_inc          = freq * phase_inc_factor;
        waves[note].phase_acc          = 0.0f; /* Randomized by caller after this function */
        waves[note].detune_offset      = 0.0f; /* Redrawn at each phase-reset onset */
        waves[note].current_volume     = 0.0f;
        waves[note].target_volume      = 0.0f;
        waves[note].physiological_gain = 1.0f; /* Set below if filter enabled */
    }

    /* Log first and last note */
    if (total_notes > 0)
    {
        log_info("SYNTH", "First note: %.2f Hz, phase_inc=%.5f",
                 waves[0].frequency, (double)waves[0].phase_inc);
        log_info("SYNTH", "Last  note: %.2f Hz, phase_inc=%.5f",
                 waves[total_notes - 1].frequency, (double)waves[total_notes - 1].phase_inc);
    }

    /* -----------------------------------------------------------------------
     * Per-note physiological gain (Option A):
     * Each note gets a gain based on its ACTUAL frequency.
     * Gains are RMS-normalized across ALL notes so that mean(gain²) = 1.0.
     * ----------------------------------------------------------------------- */
    int phys_filter = g_sp3ctra_config.physiological_filter_enabled;
    log_info("SYNTH", "Physiological filter: %s", phys_filter ? "ENABLED" : "DISABLED");

    if (phys_filter)
    {
        /* Step 1: raw gains */
        for (int n = 0; n < total_notes; n++)
            waves[n].physiological_gain = compute_physiological_gain(waves[n].frequency);

        /* Step 2: RMS across all notes */
        float sum_sq = 0.0f;
        for (int n = 0; n < total_notes; n++)
            sum_sq += waves[n].physiological_gain * waves[n].physiological_gain;
        float rms = (total_notes > 0) ? sqrtf(sum_sq / (float)total_notes) : 1.0f;

        /* Step 3: normalize — mean(gain²) = 1.0 */
        if (rms > 1e-9f)
        {
            for (int n = 0; n < total_notes; n++)
                waves[n].physiological_gain /= rms;
        }

        float g_min = waves[0].physiological_gain;
        float g_max = waves[0].physiological_gain;
        for (int n = 1; n < total_notes; n++)
        {
            if (waves[n].physiological_gain < g_min) g_min = waves[n].physiological_gain;
            if (waves[n].physiological_gain > g_max) g_max = waves[n].physiological_gain;
        }
        log_info("SYNTH", "Physiological gains: RMS=%.4f → normalized range [%.3f, %.3f]",
                 rms, g_min, g_max);
    }

    log_info("SYNTH", "----------------------------------------------------");
}

/**************************************************************************************
 * Hot-reload frequency range — Simplified Implementation
 **************************************************************************************/

void request_frequency_reinit(void)
{
    int expected = FREQ_REINIT_IDLE;
    if (atomic_compare_exchange_strong(&g_freq_reinit_state, &expected, FREQ_REINIT_PENDING))
    {
        g_global_fade_target = 0.0f;
        log_info("FREQ_REINIT", "Frequency reinit requested - starting global fade out");
    }
    else
    {
        log_warning("FREQ_REINIT", "Reinit already in progress, ignoring request");
    }
}

int is_frequency_reinit_fading_out(void)
{
    return atomic_load(&g_freq_reinit_state) == FREQ_REINIT_PENDING;
}

void reset_frequency_reinit_state(void)
{
    int prev = atomic_exchange(&g_freq_reinit_state, FREQ_REINIT_IDLE);
    if (prev != FREQ_REINIT_IDLE)
    {
        log_info("FREQ_REINIT", "Stale reinit state cleared (was %d) - ready for new request", prev);
    }
}

int check_and_process_frequency_reinit(void)
{
    int state = atomic_load(&g_freq_reinit_state);

    if (state == FREQ_REINIT_IDLE)
        return 0;

    if (state == FREQ_REINIT_PENDING)
    {
        /* Workers are waiting on start_barrier — safe to regenerate */
        wavesGeneratorParams.startFrequency  = (uint32_t)g_sp3ctra_config.start_frequency;
        wavesGeneratorParams.commaPerSemitone = g_sp3ctra_config.comma_per_semitone;

        log_info("FREQ_REINIT", "Regenerating oscillator descriptors for %.1f - %.1f Hz",
                 g_sp3ctra_config.low_frequency, g_sp3ctra_config.high_frequency);

        init_waves(waves, &wavesGeneratorParams);
        update_gap_limiter_coefficients();

        /* Randomize phases to avoid constructive interference at reinit */
        int num_notes = get_current_number_of_notes();
        for (int note = 0; note < num_notes; note++)
        {
#ifdef __APPLE__
            uint32_t r = arc4random();
#else
            uint32_t r = (uint32_t)rand();
#endif
            waves[note].phase_acc     = (float)(r % SINE_TABLE_SIZE);
            waves[note].current_volume = 0.0f;
        }

        g_global_fade_target = 1.0f;
        atomic_store(&g_freq_reinit_state, FREQ_REINIT_IDLE);
        log_info("FREQ_REINIT", "Frequency reinit complete - global fade will handle transition");
        return 1;
    }

    return 0;
}

/**************************************************************************************
 * Global fade functions
 **************************************************************************************/

float get_global_fade_factor_and_update(void)
{
    g_global_fade_current += GLOBAL_FADE_ALPHA * (g_global_fade_target - g_global_fade_current);

    if (g_global_fade_current < 0.0001f && g_global_fade_target == 0.0f)
        g_global_fade_current = 0.0f;
    if (g_global_fade_current > 0.9999f && g_global_fade_target == 1.0f)
        g_global_fade_current = 1.0f;

    return g_global_fade_current;
}

float get_global_fade_factor(void)
{
    return g_global_fade_current;
}
