/*
 * synth_luxstral_algorithms.h
 *
 * Centralized algorithms for additive synthesis
 * Contains core processing algorithms used by both threading and sequential modes
 *
 * Author: zhonx
 * Created: 21 sep. 2025
 */

#ifndef SYNTH_LUXSTRAL_ALGORITHMS_H
#define SYNTH_LUXSTRAL_ALGORITHMS_H

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include <stdint.h>
#include "wave_generation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Function prototypes -------------------------------------------------------*/


/**
 * @brief Apply GAP_LIMITER volume ramp for a single note
 * @param note Note index
 * @param target_volume Target volume for the note
 * @param volumeBuffer Output volume buffer for audio samples
 * @retval None
 */
/**
 * @brief Apply GAP_LIMITER volume ramp for a single note
 * @param note Note index
 * @param target_volume Target volume for the note
 * @param pre_wave Pointer to precomputed waveform samples for this note (AUDIO_BUFFER_SIZE, in WAVE_AMP_RESOLUTION scale)
 * @param volumeBuffer Output volume buffer for audio samples
 * @retval None
 */
/* `waves` is the OWNING engine's oscillator array (worker->engine->waves) — the
 * per-note current_volume/target_volume it reads/writes must be per-engine (M8). */
void apply_gap_limiter_ramp(volatile struct wave *waves, int note, float target_volume, const float *pre_wave, float *volumeBuffer);

/**
 * @brief Precompute GAP_LIMITER envelope coefficients for all oscillators
 * Called at startup and when tau parameters change at runtime
 * @retval None
 */
void update_gap_limiter_coefficients(void);
/* Parameterized core (M8): writes alpha_up / alpha_down_weighted into any
 * oscillator array with caller-supplied taus — used for engine B's private
 * waves[] so its Attack/Release knobs are independent of engine A's. */
struct wave;
void update_gap_limiter_coefficients_for(volatile struct wave *w,
                                         float tau_up_ms, float tau_down_ms);

/**
 * @brief Generate waveform samples using precomputed data
 * @param note Note index
 * @param waveBuffer Output waveform buffer
 * @param precomputed_wave_data Precomputed waveform data
 * @retval None
 */
void generate_waveform_samples(int note, float *waveBuffer, const float *precomputed_wave_data);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_LUXSTRAL_ALGORITHMS_H */
