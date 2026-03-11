/*
 * wave_generation.c
 *
 *  Created on: 24 avr. 2019
 *      Author: zhonx
 */

/* Includes ------------------------------------------------------------------*/
#include "config.h"
#include "wave_generation.h"
#include "../../config/config_loader.h"

#include "math.h"
#include "stdio.h"
#include "stdlib.h"

#include "error.h"
#include "logger.h"
#include "wave_generation.h"

#define PI (3.14159265358979323846)

/* Global variables definitions (moved from shared.c) */
volatile struct waveParams wavesGeneratorParams;
volatile struct wave *waves = NULL;  // Now a pointer (allocated in synth_luxstral_runtime.c)
volatile float *unitary_waveform = NULL;  // Now a pointer (allocated in synth_luxstral_runtime.c)

/* Private includes ----------------------------------------------------------*/

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static float calculate_frequency(uint32_t comma_cnt,
                                 volatile struct waveParams *params);
static uint32_t calculate_waveform(uint32_t current_aera_size,
                                   uint32_t current_unitary_waveform_cell,
                                   uint32_t buffer_len,
                                   volatile struct waveParams *params,
                                   float amplitude_scale);
static float compute_a_weighting_ra(float f);
static float compute_physiological_gain(float frequency_hz);

/* Private user code ---------------------------------------------------------*/

static float calculate_frequency(uint32_t comma_cnt,
                                 volatile struct waveParams *params) {
  float frequency = 0.00;
  frequency =
      params->startFrequency *
      pow(2, ((float)comma_cnt /
              (12.0 * ((g_sp3ctra_config.semitone_per_octave * (float)params->commaPerSemitone) /
                       (12.00 / (log(2)) *
                        log((params->startFrequency * 2.00) /
                            (float)params->startFrequency))))));

  return frequency;
}

static uint32_t calculate_waveform(uint32_t current_aera_size,
                                   uint32_t current_unitary_waveform_cell,
                                   uint32_t buffer_len,
                                   volatile struct waveParams *params,
                                   float amplitude_scale) {
  (void)params; // Suppress unused parameter warning

  unitary_waveform[current_unitary_waveform_cell] = 0;

  // Generate sinusoidal waveform (SIN is now implicit)
  // amplitude_scale applies RMS-normalized physiological compensation (1.0 = no change)
  for (uint32_t x = 0; x < current_aera_size; x++) {
    // sanity check
    if (current_unitary_waveform_cell < buffer_len) {
      unitary_waveform[current_unitary_waveform_cell] =
          ((sin((x * 2.00 * PI) / (float)current_aera_size))) *
          WAVE_AMP_RESOLUTION * amplitude_scale;
    }
    current_unitary_waveform_cell++;
  }

  return current_unitary_waveform_cell;
}

/**************************************************************************************
 * Physiological (Equal-Loudness) Compensation
 * Based on inverse A-weighting (IEC 61672:2003)
 *
 * The gain is normalized so that 1 kHz = 1.0 (reference frequency).
 * An RMS normalization pass is then applied across all commas so that
 * mean(gain^2) = 1.0, preserving total energy during summation.
 **************************************************************************************/

/**
 * @brief Compute the A-weighting relative response RA(f) for a given frequency
 *
 * Formula from IEC 61672:2003:
 * RA(f) = (12194^2 * f^4) / ((f^2 + 20.6^2) * sqrt((f^2 + 107.7^2)(f^2 + 737.9^2)) * (f^2 + 12194^2))
 */
static float compute_a_weighting_ra(float f) {
    float f2 = f * f;
    float f4 = f2 * f2;

    float num = 12194.0f * 12194.0f * f4;

    float d1 = f2 + 20.6f * 20.6f;
    float d2 = f2 + 107.7f * 107.7f;
    float d3 = f2 + 737.9f * 737.9f;
    float d4 = f2 + 12194.0f * 12194.0f;

    float den = d1 * sqrtf(d2 * d3) * d4;

    if (den < 1e-30f) return 0.0f;

    return num / den;
}

/**
 * @brief Compute physiological (A-weighting inverse) gain compensation for a given frequency
 *
 * Uses the inverse of IEC 61672:2003 A-weighting to partially compensate for
 * human hearing sensitivity variations across the frequency range.
 *
 * The correction is scaled by g_sp3ctra_config.physiological_correction_depth
 * (0.0 = flat, 1.0 = full inverse A-weighting) applied in the dB domain so
 * that depth=0 always yields gain=1.0 and the curve scales continuously.
 *
 * Max clamp is ±12 dB (factor 4.0 / 0.25) to avoid excessive colouration.
 * Full A-weighting inverse at 65 Hz would be ~+26 dB without clamping;
 * depth=0.3 reduces this to ~+8 dB, which is much more musical.
 *
 * @param frequency_hz Frequency in Hz (must be > 0)
 * @return Linear gain factor (1.0 = flat, depends on depth)
 */
static float compute_physiological_gain(float frequency_hz) {
    if (frequency_hz <= 0.0f) return 1.0f;

    // Clamp frequency to audible range to avoid extreme values
    if (frequency_hz < 20.0f)    frequency_hz = 20.0f;
    if (frequency_hz > 20000.0f) frequency_hz = 20000.0f;

    float ra_f  = compute_a_weighting_ra(frequency_hz);
    float ra_1k = compute_a_weighting_ra(1000.0f); // Reference: 0 dB at 1 kHz

    if (ra_f < 1e-30f || ra_1k < 1e-30f) return 1.0f;

    // A-weighting relative to 1 kHz (in dB)
    float a_db = 20.0f * log10f(ra_f / ra_1k);

    // Apply correction depth in dB domain: depth=0 → 0 dB correction, depth=1 → full inverse
    float depth = g_sp3ctra_config.physiological_correction_depth;
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;

    float corrected_db = -a_db * depth;

    // Safety clamp: ±15 dB (factor ~5.6 max boost, 0.18 max cut)
    // At depth=0.5, 65 Hz produces ~13 dB → headroom without hard clipping.
    if (corrected_db >  15.0f) corrected_db =  15.0f;
    if (corrected_db < -15.0f) corrected_db = -15.0f;

    return powf(10.0f, corrected_db / 20.0f);
}

uint32_t init_waves(volatile float *unitary_waveform,
                    volatile struct wave *waves,
                    volatile struct waveParams *parameters) {
  uint32_t buffer_len = 0;
  uint32_t note = 0;
  uint32_t current_unitary_waveform_cell = 0;

  // Number of unique waveforms (one per comma in reference octave)
  const uint32_t commas_per_octave =
      (uint32_t)(g_sp3ctra_config.semitone_per_octave) * parameters->commaPerSemitone;

  log_info("SYNTH", "---------- WAVES INIT ---------");

  // First pass: calculate total buffer length for first-octave waveforms
  for (uint32_t comma_cnt = 0; comma_cnt < commas_per_octave; comma_cnt++) {
    float frequency = calculate_frequency(comma_cnt, parameters);
    buffer_len += (uint32_t)(g_sp3ctra_config.sampling_frequency / frequency);
  }

  // Physiological filter status
  const int phys_filter = g_sp3ctra_config.physiological_filter_enabled;
  log_info("SYNTH", "Physiological (equal-loudness) filter: %s",
           phys_filter ? "ENABLED" : "DISABLED");

  // Second pass: compute and store waveforms for the reference octave (always ±1.0)
  // Physiological compensation is handled via waves[note].physiological_gain
  // applied at mixdown in apply_gap_limiter_ramp().
  for (uint32_t comma_cnt = 0; comma_cnt < commas_per_octave; comma_cnt++) {
    float frequency = calculate_frequency(comma_cnt, parameters);

    uint32_t current_aera_size = (uint32_t)((g_sp3ctra_config.sampling_frequency / frequency));

    // Waveforms always at unity amplitude ±1.0
    current_unitary_waveform_cell =
        calculate_waveform(current_aera_size, current_unitary_waveform_cell,
                           buffer_len, parameters, 1.0f);

    // Assign this waveform to all octaves
    for (uint32_t octave = 0;
         octave <= (get_current_number_of_notes() / commas_per_octave);
         octave++) {
      note = comma_cnt + commas_per_octave * octave;
      if ((int)note < get_current_number_of_notes()) {
        waves[note].frequency = frequency * pow(2, octave);
        waves[note].area_size = current_aera_size;
        waves[note].start_ptr =
            &unitary_waveform[current_unitary_waveform_cell - current_aera_size];
        waves[note].current_idx = 0;
        waves[note].octave_coeff = pow(2, octave);
        waves[note].octave_divider = 1;
        waves[note].physiological_gain = 1.0f;  // initialized; set below
      }
    }
  }

  // -----------------------------------------------------------------------
  // Per-note physiological gain (Option A):
  // Each note gets a gain based on its ACTUAL frequency. Waveforms are shared
  // across octaves so amplitude cannot encode per-octave correction.
  // Gains are RMS-normalized so mean(gain^2) = 1.0 across all notes.
  // -----------------------------------------------------------------------
  {
    const int total_notes = get_current_number_of_notes();
    if (phys_filter && total_notes > 0) {
      // Step 1: compute raw gains per note
      for (int n = 0; n < total_notes; n++)
        waves[n].physiological_gain = compute_physiological_gain(waves[n].frequency);
      // Step 2: RMS
      float sum_sq = 0.0f;
      for (int n = 0; n < total_notes; n++)
        sum_sq += waves[n].physiological_gain * waves[n].physiological_gain;
      float rms = sqrtf(sum_sq / (float)total_notes);
      // Step 3: normalize
      if (rms > 1e-9f) {
        for (int n = 0; n < total_notes; n++)
          waves[n].physiological_gain /= rms;
      }
      log_info("SYNTH", "Physiological gains: RMS=%.4f (normalized per actual note frequency)", rms);
    } else {
      const int total_notes_off = get_current_number_of_notes();
      for (int n = 0; n < total_notes_off; n++)
        waves[n].physiological_gain = 1.0f;
    }
  }

  if ((int)note < get_current_number_of_notes()) {
    log_error("SYNTH", "Wave generation configuration failed: current pixel = %d", (int)note);
    die("wave init failed");
  }

  log_info("SYNTH", "-------------------------------");

  return buffer_len;
}
