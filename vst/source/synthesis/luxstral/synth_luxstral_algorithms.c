/*
 * synth_luxstral_algorithms.c
 *
 * Centralized algorithms for additive synthesis
 * Contains core processing algorithms used by both threading and sequential modes
 *
 * Author: zhonx
 * Created: 21 sep. 2025
 */

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include "synth_luxstral_algorithms.h"
#include "synth_luxstral_math.h"
#include "wave_generation.h"
#include <stdio.h>
#include <math.h>


/* Function implementations --------------------------------------------------*/

/**
 * @brief Precompute gap limiter envelope coefficients for all oscillators
 * Called at startup and when tau parameters change at runtime
 * @retval None
 */
void update_gap_limiter_coefficients_for(volatile struct wave *w,
                                         float tau_up_ms, float tau_down_ms) {
    // Safety check: the target array must be initialized
    if (w == NULL) {
        log_warning("LUXSTRAL", "update_gap_limiter_coefficients_for: waves is NULL, skipping");
        return;  // Silently return if called before initialization
    }

    const float Fs = (float)g_sp3ctra_config.sampling_frequency;

    // Clamp tau to avoid division by zero or denormals
    if (tau_up_ms < 0.01f) tau_up_ms = 0.01f;
    if (tau_up_ms > TAU_UP_MAX_MS) tau_up_ms = TAU_UP_MAX_MS;

    const float tau_up_s = tau_up_ms * 0.001f;

    // Compute attack alpha (exponential envelope coefficient)
    float alpha_up = 1.0f - expf(-1.0f / (tau_up_s * Fs));

    // Clamp alpha to reasonable bounds
    if (alpha_up < ALPHA_MIN) alpha_up = ALPHA_MIN;
    if (alpha_up > 1.0f) alpha_up = 1.0f;

    // Clamp tau to avoid division by zero or denormals
    if (tau_down_ms < 0.01f) tau_down_ms = 0.01f;
    if (tau_down_ms > TAU_DOWN_MAX_MS) tau_down_ms = TAU_DOWN_MAX_MS;

    const float tau_down_s = tau_down_ms * 0.001f;

    // Compute release alpha (exponential envelope coefficient)
    float alpha_down = 1.0f - expf(-1.0f / (tau_down_s * Fs));

    // Clamp alpha to reasonable bounds
    if (alpha_down < ALPHA_MIN) alpha_down = ALPHA_MIN;
    if (alpha_down > 1.0f) alpha_down = 1.0f;

    // Precompute for each oscillator
    const int num_notes = get_current_number_of_notes();

    for (int note = 0; note < num_notes; note++) {
        // Store attack coefficient (frequency-independent)
        w[note].alpha_up = alpha_up;

        // Compute frequency-dependent release weighting
        float f = w[note].frequency;
        if (f < 1.0f) f = 1.0f;

        float ratio = f / g_sp3ctra_config.decay_freq_ref_hz;
        float g_down = powf(ratio, -g_sp3ctra_config.decay_freq_beta);

        // Clamp frequency weight
        if (g_down < DECAY_FREQ_MIN) g_down = DECAY_FREQ_MIN;
        if (g_down > DECAY_FREQ_MAX) g_down = DECAY_FREQ_MAX;

        // Store weighted release coefficient
        w[note].alpha_down_weighted = alpha_down * g_down;
    }
}

void update_gap_limiter_coefficients(void) {
    // Legacy entry — engine A: global waves[] + the global (A) tau params.
    update_gap_limiter_coefficients_for(waves,
                                        g_sp3ctra_config.tau_up_base_ms,
                                        g_sp3ctra_config.tau_down_base_ms);
}


/**
 * @brief Apply GAP_LIMITER volume ramp for a single note
 * @param note Note index
 * @param target_volume Target volume for the note
 * @param pre_wave Precomputed waveform data (unused)
 * @param volumeBuffer Output volume buffer for audio samples
 * @retval None
 */
void apply_gap_limiter_ramp(volatile struct wave *waves, int note, float target_volume, const float *pre_wave, float *volumeBuffer) {
    // `waves` (param) shadows the global — engine A passes the global array,
    // engine B its own copy, so per-note current_volume stays per-engine (M8).
    (void)pre_wave; // No longer used (phase weighting removed)

    // Apply per-note physiological (equal-loudness) gain.
    // Gain is RMS-normalized at init so total energy is preserved.
    // waves[note].physiological_gain = 1.0 when filter is disabled.
    target_volume *= waves[note].physiological_gain;

    // Set the target volume for the oscillator
    waves[note].target_volume = target_volume;

    // Local copies (avoid repeated volatile access)
    float v = waves[note].current_volume;
    const float t = waves[note].target_volume;

    // PROGRESSIVE ATTACK MODE: Traditional envelope with attack and release
    // Use precomputed envelope coefficients (no complex calculations in RT path!)
    const float alpha = (t > v) ? waves[note].alpha_up : waves[note].alpha_down_weighted;
    
    // Use optimized envelope function (NEON-accelerated on ARM)
    float final_volume = apply_envelope_ramp(volumeBuffer, v, t, alpha,
                                            g_sp3ctra_config.audio_buffer_size,
                                            0.0f, (float)VOLUME_AMP_RESOLUTION);
    
    // Write back current volume once per buffer
    waves[note].current_volume = final_volume;
}


/**
 * @brief Apply RELATIVE_MODE processing to image buffer
 * @param imageBuffer_q31 Input/output buffer
 * @param start_note Starting note index
 * @param end_note Ending note index (exclusive)
 * @retval None
 */
void apply_relative_mode(int32_t *imageBuffer_q31, int start_note, int end_note) {
  (void)imageBuffer_q31; // Mark as unused to suppress warning
  (void)start_note;      // Mark as unused to suppress warning
  (void)end_note;        // Mark as unused to suppress warning
#ifdef RELATIVE_MODE
    // Special processing for RELATIVE_MODE
    if (start_note < end_note - 1) {
        sub_int32((int32_t *)&imageBuffer_q31[0],
                  (int32_t *)&imageBuffer_q31[1],
                  (int32_t *)&imageBuffer_q31[0],
                  end_note - start_note - 1);

        clip_int32((int32_t *)imageBuffer_q31, 0, VOLUME_AMP_RESOLUTION,
                   end_note - start_note);
    }

    if (end_note == NUMBER_OF_NOTES) {
        imageBuffer_q31[end_note - start_note - 1] = 0;
    }
#endif
}

/**
 * @brief Generate waveform samples using precomputed data
 * @param note Note index
 * @param waveBuffer Output waveform buffer
 * @param precomputed_wave_data Precomputed waveform data
 * @retval None
 */
void generate_waveform_samples(int note, float *waveBuffer, 
                              const float *precomputed_wave_data) {
    (void)note; // Mark as unused to suppress warning
    // CRITICAL FIX: Normalize waveform data from integer range to float [-1.0, +1.0]
    const float normalization_factor = 1.0f / (float)WAVE_AMP_RESOLUTION;
    for (int buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
        waveBuffer[buff_idx] = precomputed_wave_data[buff_idx] * normalization_factor;
    }
}
