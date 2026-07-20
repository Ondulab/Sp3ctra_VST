/*
 * synth_luxstral.h
 *
 * Main header for additive synthesis engine
 * Includes all modular components of the refactored additive synthesis system
 *
 * Author: zhonx
 */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SYNTH_LUXSTRAL_H
#define __SYNTH_LUXSTRAL_H

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"  // VST adaptation layer C-only (provides config and types)

/**************************************************************************************
 * Buffer Management Configuration
 **************************************************************************************/

// Helper function to get current number of notes based on runtime configuration
static inline int get_current_number_of_notes(void) {
    extern sp3ctra_config_t g_sp3ctra_config;
    return get_cis_pixels_nb() / g_sp3ctra_config.pixels_per_note;
}

/**************************************************************************************
 * Debug Configuration
 **************************************************************************************/
// Debug configuration structure (runtime)
typedef struct {
    int enabled;                    // Runtime enable/disable flag
    int single_osc;                 // -1 if range, otherwise single oscillator number
    int start_osc;                  // Start of range (for ranges)
    int end_osc;                    // End of range (for ranges)
} debug_luxstral_osc_config_t;

/* Module Headers (included after constants definition) ----------------------*/
#include "synth_luxstral_math.h"
#include "synth_luxstral_stereo.h"
#include "synth_luxstral_state.h"
#include "synth_luxstral_threading.h"
#include "synth_luxstral_algorithms.h"

/* Core API function declarations -------------------------------------------*/

/**
 * @brief Initialize the additive synthesis engine
 * @return 0 on success, -1 on failure
 */
int32_t synth_IfftInit(void);

/**
 * @brief Process audio buffers for RGB channels
 * @param buffer_R Red channel buffer
 * @param buffer_G Green channel buffer  
 * @param buffer_B Blue channel buffer
 * @param db DoubleBuffer pointer for accessing preprocessed data
 */
void synth_AudioProcess(uint8_t *buffer_R, uint8_t *buffer_G, uint8_t *buffer_B, struct DoubleBuffer *db);

/**
 * @brief Zero-CPU idle commit — publish a SILENT output buffer.
 *
 * Used by audioProcessingThread when the LuxStral engine is gated (disabled
 * or no "→ LUXSTRAL" send placed in any chain): keeps the processBlock
 * producer/consumer handshake paced without running the additive render.
 */
void synth_AudioProcess_silence(void);

/**
 * @brief Main synthesis processing function
 * @param imageData Input image data array
 * @param audioLeft Output left channel audio buffer
 * @param audioRight Output right channel audio buffer
 * @param contrast Contrast adjustment parameter
 */
void synth_IfftMode(float *imageData, float *audioLeft, float *audioRight, float contrast, struct DoubleBuffer *db);
void synth_luxstral_cleanup(void);

/**
 * @brief Get the last calculated contrast factor (thread-safe)
 * @return Last contrast factor value (0.0-1.0 range typically)
 * @note Used by auto-volume system to detect audio intensity for adaptive thresholding
 */
float synth_get_last_contrast_factor(void);

/**
 * @brief Cumulative phase-management onset count (thread-safe, monotonic)
 * @param engine_idx must be 0 (single engine)
 * @note Polled by the LUXSTRAL page timer: a growing value between two polls
 *       lights the activity LED next to the phase-mode combo.
 */
uint32_t synth_luxstral_get_phase_onset_total(int engine_idx);

#endif /* __SYNTH_LUXSTRAL_H */
