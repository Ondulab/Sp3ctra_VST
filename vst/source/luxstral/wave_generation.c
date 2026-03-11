/*
 * wave_generation.c
 *
 *  Created on: 24 avr. 2019
 *      Author: zhonx
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

/* Global variables definitions (moved from shared.c) */
volatile struct waveParams wavesGeneratorParams;
volatile struct wave *waves = NULL;  // Now a pointer (allocated in synth_luxstral_runtime.c)
volatile float *unitary_waveform = NULL;  // Now a pointer (allocated in synth_luxstral_runtime.c)

/**************************************************************************************
 * Hot-reload frequency range - Simplified state machine
 * Only 2 states: IDLE and PENDING (regeneration happens immediately)
 * Global fade provides smooth transitions automatically
 **************************************************************************************/
typedef enum {
    FREQ_REINIT_IDLE = 0,       // No reinit pending
    FREQ_REINIT_PENDING,        // Reinit requested, will process next buffer
} freq_reinit_state_t;

static _Atomic int g_freq_reinit_state = FREQ_REINIT_IDLE;

// Global fade coefficient for smooth transitions (0.0 to 1.0)
// Applied to the entire output signal, not individual oscillators
static float g_global_fade_current = 1.0f;
static float g_global_fade_target = 1.0f;

// Exponential fade coefficient (sample-by-sample smoothing)
// 🔧 FIX: Compute fade alpha dynamically from actual sample rate
// Was hardcoded for 48kHz, causing wrong fade speed at other sample rates
// tau = 50ms → alpha = 1 - exp(-1/(tau_s * Fs))
static float get_global_fade_alpha(void) {
    float Fs = (float)g_sp3ctra_config.sampling_frequency;
    if (Fs < 8000.0f) Fs = 48000.0f;  // Safety fallback
    const float tau_s = 0.05f;  // 50ms fade time
    return 1.0f - expf(-1.0f / (tau_s * Fs));
}
#define GLOBAL_FADE_ALPHA get_global_fade_alpha()

/* Private includes ----------------------------------------------------------*/

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static float calculate_frequency_for_note(int note, int total_notes, float low_freq, float high_freq);
static uint32_t calculate_waveform(uint32_t current_aera_size,
                                   uint32_t current_unitary_waveform_cell,
                                   uint32_t buffer_len,
                                   volatile struct waveParams *params,
                                   float amplitude_scale);

/* Private user code ---------------------------------------------------------*/

/**
 * @brief Calculate frequency for a specific note using logarithmic distribution
 * 
 * NEW IMPLEMENTATION: Direct logarithmic interpolation between low_frequency and high_frequency
 * Formula: freq = low_freq * pow(high_freq / low_freq, note / (total_notes - 1))
 * 
 * This gives independent control of both frequency bounds.
 * 
 * @param note Note index (0 to total_notes-1)
 * @param total_notes Total number of notes
 * @param low_freq Lower frequency bound
 * @param high_freq Upper frequency bound
 * @return Frequency in Hz
 */
static float calculate_frequency_for_note(int note, int total_notes, float low_freq, float high_freq) {
    if (total_notes <= 1) return low_freq;
    
    // Logarithmic interpolation: freq = low * (high/low)^(note/(total-1))
    float ratio = (float)note / (float)(total_notes - 1);
    float frequency = low_freq * powf(high_freq / low_freq, ratio);
    
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
  // amplitude_scale applies physiological compensation (1.0 = no change)
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
 * The human ear is most sensitive around 1-5 kHz and less sensitive at
 * low and very high frequencies. This function computes a gain factor
 * that compensates for this non-uniform sensitivity:
 *   - Bass frequencies get boosted (gain > 1.0)
 *   - Mid frequencies (~1-4 kHz) stay near unity (gain ≈ 1.0)
 *   - Extreme treble gets slightly boosted (gain > 1.0)
 *
 * The gain is normalized so that 1 kHz = 1.0 (reference frequency).
 **************************************************************************************/

/**
 * @brief Compute the A-weighting relative response RA(f) for a given frequency
 * 
 * Formula from IEC 61672:2003:
 * RA(f) = (12194² × f⁴) / ((f² + 20.6²) × √((f² + 107.7²)(f² + 737.9²)) × (f² + 12194²))
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

float compute_physiological_gain(float frequency_hz) {
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
    // Scales the A-weighting inverse continuously to avoid excessive colouration.
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

/**
 * @brief Initialize waves with logarithmic distribution between low_frequency and high_frequency
 * 
 * REFACTORED: Uses g_sp3ctra_config.low_frequency and high_frequency directly
 * for independent control of both frequency bounds.
 * 
 * MEMORY OPTIMIZATION: Waveforms are generated only for the first octave,
 * then higher octaves reuse them with octave_coeff multiplier.
 */
uint32_t init_waves(volatile float *unitary_waveform,
                    volatile struct wave *waves,
                    volatile struct waveParams *parameters) {
  uint32_t buffer_len = 0;
  uint32_t note = 0;
  uint32_t current_unitary_waveform_cell = 0;
  
  const int total_notes = get_current_number_of_notes();
  const float low_freq = g_sp3ctra_config.low_frequency;
  const float high_freq = g_sp3ctra_config.high_frequency;
  const int sample_rate = g_sp3ctra_config.sampling_frequency;
  
  // 🎵 FIXED: Use user-defined num_octaves instead of dynamic calculation
  // This eliminates "jumps" when frequency range changes slightly
  int num_full_octaves = g_sp3ctra_config.num_octaves;
  if (num_full_octaves < 1) num_full_octaves = 1;
  if (num_full_octaves > 10) num_full_octaves = 10;
  
  // Calculate actual octave span for logging (informational only)
  float actual_octaves = log2f(high_freq / low_freq);
  
  // Notes in first octave (reference octave for waveform generation)
  int notes_per_octave = total_notes / num_full_octaves;
  if (notes_per_octave < 1) notes_per_octave = 1;
  
  // Effective comma per semitone for info
  float effective_comma = (float)notes_per_octave / 12.0f;

  log_info("SYNTH", "---------- WAVES INIT ---------");
  log_info("SYNTH", "Freq range: %.1f - %.1f Hz (%.2f actual, %d fixed octaves)", 
           low_freq, high_freq, actual_octaves, num_full_octaves);
  log_info("SYNTH", "Notes: %d, Notes/octave: %d, Effective commas/semitone: %.2f", 
           total_notes, notes_per_octave, effective_comma);
  
  // 🔍 DIAGNOSTIC: Log frequency mapping for key positions
  log_info("SYNTH", "🔍 Frequency mapping test:");
  int test_positions[] = {0, total_notes/4, total_notes/2, (3*total_notes)/4, total_notes-1};
  for (int ti = 0; ti < 5; ti++) {
    int test_note = test_positions[ti];
    if (test_note >= 0 && test_note < total_notes) {
      float test_freq = calculate_frequency_for_note(test_note, total_notes, low_freq, high_freq);
      log_info("SYNTH", "  Note %4d/%d (%.1f%%) → %.2f Hz", 
               test_note, total_notes, 100.0f * test_note / (total_notes-1), test_freq);
    }
  }

  // First pass: Calculate buffer_len needed for first octave waveforms only
  for (int comma_cnt = 0; comma_cnt < notes_per_octave; comma_cnt++) {
    // First octave: frequencies from low_freq to low_freq * 2
    float ratio = (float)comma_cnt / (float)notes_per_octave;
    float frequency = low_freq * powf(2.0f, ratio);  // Within first octave
    uint32_t area_size = (uint32_t)(sample_rate / frequency);
    if (area_size < 2) area_size = 2;
    buffer_len += area_size;
  }

  // Physiological filter status
  int phys_filter = g_sp3ctra_config.physiological_filter_enabled;
  log_info("SYNTH", "Physiological (equal-loudness) filter: %s", phys_filter ? "ENABLED" : "DISABLED");
  log_info("SYNTH", "Waveform buffer: %u samples (first octave only)", buffer_len);

  // Second pass: Generate waveforms for first octave (always at unity amplitude ±1.0)
  // Physiological compensation is handled separately via waves[note].physiological_gain
  // applied at mixdown in apply_gap_limiter_ramp().

  current_unitary_waveform_cell = 0;
  
  for (int comma_cnt = 0; comma_cnt < notes_per_octave; comma_cnt++) {
    // Calculate base frequency for this comma (first octave)
    float ratio = (float)comma_cnt / (float)notes_per_octave;
    float base_frequency = low_freq * powf(2.0f, ratio);
    
    // Calculate area size for base frequency
    uint32_t current_area_size = (uint32_t)(sample_rate / base_frequency);
    if (current_area_size < 2) current_area_size = 2;
    
    // Waveforms are always at unity amplitude ±1.0 (amplitude_scale = 1.0)
    // Physiological gain is stored per-note in struct wave and applied at mixdown
    current_unitary_waveform_cell = 
        calculate_waveform(current_area_size, current_unitary_waveform_cell,
                           buffer_len, parameters, 1.0f);

    // Assign this waveform to all octaves
    for (int octave = 0; octave < num_full_octaves + 1; octave++) {
      note = comma_cnt + notes_per_octave * octave;
      
      if ((int)note < total_notes) {
        // Calculate actual frequency for this note using logarithmic distribution
        float note_frequency = calculate_frequency_for_note(note, total_notes, low_freq, high_freq);
        
        // Store frequency
        waves[note].frequency = note_frequency;
        // Store area size (from base frequency)
        waves[note].area_size = current_area_size;
        // Store pointer to base waveform
        waves[note].start_ptr = &unitary_waveform[current_unitary_waveform_cell - current_area_size];
        // Float phase accumulator — initialized to 0, randomised after init_waves() returns
        waves[note].phase_acc = 0.0f;
        // phase_inc = note_frequency / base_frequency
        //   = 2^(octave + comma/notes_per_octave) which equals note_freq / (sample_rate/area_size)
        // Values below 1 for bass notes (sub-sample resolution), above 1 for treble (skip samples)
        waves[note].phase_inc = note_frequency / base_frequency;

        // Initialize physiological gain to unity (will be set below)
        waves[note].physiological_gain = 1.0f;
      }
    }
  }

  // 🔧 FIX: Initialize orphan notes when total_notes is not divisible by num_octaves
  // Example: 3456 notes / 10 octaves = 345.6 → 345 notes/octave → 3450 notes covered
  // Notes 3450-3455 would be uninitialized → CRASH!
  int covered_notes = notes_per_octave * (num_full_octaves + 1);
  if (covered_notes < total_notes && notes_per_octave > 0) {
    int orphan_count = total_notes - (int)note - 1;  // note is the last configured note index
    
    if (orphan_count > 0) {
      log_info("SYNTH", "Initializing %d orphan notes (indices %d-%d)", 
               orphan_count, (int)note + 1, total_notes - 1);
      
      // Use the last configured note's waveform for orphan notes
      uint32_t last_area_size = waves[note].area_size;
      volatile float* last_start_ptr = waves[note].start_ptr;
      // base_freq for last comma ≈ sample_rate / last_area_size
      float last_base_freq = (float)sample_rate / (float)last_area_size;

      for (int orphan_idx = (int)note + 1; orphan_idx < total_notes; orphan_idx++) {
        // Calculate frequency using logarithmic distribution (correct frequency)
        float orphan_freq = calculate_frequency_for_note(orphan_idx, total_notes, low_freq, high_freq);

        waves[orphan_idx].frequency  = orphan_freq;
        waves[orphan_idx].area_size  = last_area_size;
        waves[orphan_idx].start_ptr  = last_start_ptr;
        waves[orphan_idx].phase_acc  = 0.0f;
        waves[orphan_idx].phase_inc  = orphan_freq / last_base_freq;
        waves[orphan_idx].current_volume = 0.0f;
      }
    }
  }

  // Log first and last note info
  if (total_notes > 0) {
    log_info("SYNTH", "First note: %.2f Hz, area_size=%u, phase_inc=%.5f",
             waves[0].frequency, waves[0].area_size, (double)waves[0].phase_inc);
    log_info("SYNTH", "Last note:  %.2f Hz, area_size=%u, phase_inc=%.5f",
             waves[total_notes-1].frequency, waves[total_notes-1].area_size,
             (double)waves[total_notes-1].phase_inc);
  }

  // Sanity check - should now always pass
  if ((int)note < total_notes - 1) {
    log_debug("SYNTH", "Note coverage: last main note=%d, total=%d (orphans handled above)", 
                (int)note, total_notes);
  }

  // -----------------------------------------------------------------------
  // Per-note physiological gain (Option A):
  // Each note gets a gain based on its ACTUAL frequency (not the shared base
  // waveform frequency). This is the only correct approach since waveforms
  // are shared across octaves and cannot encode per-octave amplitude.
  //
  // Gains are RMS-normalized across ALL notes so that mean(gain^2) = 1.0,
  // preserving total energy when all oscillators are active simultaneously.
  // -----------------------------------------------------------------------
  if (phys_filter) {
    // Step 1: compute raw gains per note
    for (int n = 0; n < total_notes; n++)
      waves[n].physiological_gain = compute_physiological_gain(waves[n].frequency);

    // Step 2: compute RMS across all notes
    float sum_sq = 0.0f;
    for (int n = 0; n < total_notes; n++)
      sum_sq += waves[n].physiological_gain * waves[n].physiological_gain;
    float rms = (total_notes > 0) ? sqrtf(sum_sq / (float)total_notes) : 1.0f;

    // Step 3: normalize — mean(gain^2) = 1.0
    if (rms > 1e-9f) {
      for (int n = 0; n < total_notes; n++)
        waves[n].physiological_gain /= rms;
    }

    // Log gain range for diagnostics
    float g_min = waves[0].physiological_gain;
    float g_max = waves[0].physiological_gain;
    for (int n = 1; n < total_notes; n++) {
      if (waves[n].physiological_gain < g_min) g_min = waves[n].physiological_gain;
      if (waves[n].physiological_gain > g_max) g_max = waves[n].physiological_gain;
    }
    log_info("SYNTH", "Physiological gains: RMS=%.4f → normalized range [%.3f, %.3f]",
             rms, g_min, g_max);
  } else {
    // Filter disabled: unity gain for all notes
    for (int n = 0; n < total_notes; n++)
      waves[n].physiological_gain = 1.0f;
  }

  log_info("SYNTH", "-------------------------------");

  return buffer_len;
}

/**************************************************************************************
 * Hot-reload frequency range - Simplified Implementation
 * Global fade handles all transitions automatically
 **************************************************************************************/

/**
 * @brief Request frequency range reinit from UI thread
 * Immediately triggers fade-out via global fade, regeneration happens next buffer
 */
void request_frequency_reinit(void) {
    // Only request if not already in progress
    int expected = FREQ_REINIT_IDLE;
    if (atomic_compare_exchange_strong(&g_freq_reinit_state, &expected, FREQ_REINIT_PENDING)) {
        // Start fade-out immediately (global fade will handle smooth transition)
        g_global_fade_target = 0.0f;
        log_info("FREQ_REINIT", "Frequency reinit requested - starting global fade out");
    } else {
        log_warning("FREQ_REINIT", "Reinit already in progress, ignoring request");
    }
}

/**
 * @brief Check if frequency reinit is currently in progress
 * @return 1 if pending, 0 otherwise
 */
int is_frequency_reinit_fading_out(void) {
    return atomic_load(&g_freq_reinit_state) == FREQ_REINIT_PENDING;
}

/**
 * @brief Check and process pending frequency reinit
 * Called at the beginning of synth_IfftMode() BEFORE workers start
 * Regenerates immediately - global fade ensures smooth transitions
 * @return 1 if reinit was performed, 0 otherwise
 */
int check_and_process_frequency_reinit(void) {
    int state = atomic_load(&g_freq_reinit_state);
    
    if (state == FREQ_REINIT_IDLE) {
        return 0;  // Nothing to do
    }
    
    if (state == FREQ_REINIT_PENDING) {
        // CRITICAL: Workers are waiting on start_barrier, safe to regenerate!
        
        // Update wavesGeneratorParams with new frequency from config
        wavesGeneratorParams.startFrequency = (uint32_t)g_sp3ctra_config.start_frequency;
        wavesGeneratorParams.commaPerSemitone = g_sp3ctra_config.comma_per_semitone;
        
        log_info("FREQ_REINIT", "Regenerating waveforms for freq range %.1f - %.1f Hz",
                 g_sp3ctra_config.low_frequency, g_sp3ctra_config.high_frequency);
        
        // Regenerate all waveforms
        init_waves(unitary_waveform, waves, &wavesGeneratorParams);
        
        // Recompute GAP_LIMITER coefficients (depend on frequency)
        update_gap_limiter_coefficients();
        
        // CRITICAL: Randomize phase to avoid constructive interference
        // Without this, all oscillators start at phase 0 → harsh sound
        int num_notes = get_current_number_of_notes();
        for (int note = 0; note < num_notes; note++) {
#ifdef __APPLE__
            uint32_t aRandom32bit = arc4random();
#else
            uint32_t aRandom32bit = (uint32_t)rand();
#endif
            waves[note].phase_acc = (float)(aRandom32bit % waves[note].area_size);
            waves[note].current_volume = 0.0f;
            // target_volume will be set by image data naturally
        }
        
        // Set global fade target back to 1.0 for smooth fade-in
        g_global_fade_target = 1.0f;
        
        // Done!
        atomic_store(&g_freq_reinit_state, FREQ_REINIT_IDLE);
        log_info("FREQ_REINIT", "Frequency reinit complete - global fade will handle transition");
        
        return 1;  // Reinit was performed
    }
    
    return 0;
}

/**************************************************************************************
 * Global fade functions - Applied to output signal for smooth transitions
 **************************************************************************************/

/**
 * @brief Get and update global fade factor for a single sample
 * Call this for each output sample to apply smooth exponential fade
 * @return Current fade factor (0.0 to 1.0)
 */
float get_global_fade_factor_and_update(void) {
    // Exponential smoothing: current += alpha * (target - current)
    g_global_fade_current += GLOBAL_FADE_ALPHA * (g_global_fade_target - g_global_fade_current);
    
    // Clamp to avoid denormals
    if (g_global_fade_current < 0.0001f && g_global_fade_target == 0.0f) {
        g_global_fade_current = 0.0f;
    }
    if (g_global_fade_current > 0.9999f && g_global_fade_target == 1.0f) {
        g_global_fade_current = 1.0f;
    }
    
    return g_global_fade_current;
}

/**
 * @brief Get current global fade factor without updating
 * @return Current fade factor (0.0 to 1.0)
 */
float get_global_fade_factor(void) {
    return g_global_fade_current;
}
