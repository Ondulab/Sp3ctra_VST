/* config_synth_luxstral.h */

#ifndef __CONFIG_SYNTH_LUXSTRAL_H__
#define __CONFIG_SYNTH_LUXSTRAL_H__

#include "config_loader.h"  // For g_luxstral_config
#include "config_audio.h"   // For AUDIO_BUFFER_SIZE and SAMPLING_FREQUENCY
#include "config_instrument.h"  // For CIS_MAX_PIXELS_NB

/**************************************************************************************
 * Stereo Configuration
 **************************************************************************************/
// Pan law configuration
#define STEREO_PAN_LAW_CONSTANT_POWER    1      // 1 = cos/sin law (constant power), 0 = linear law

// Center compensation parameters (technical constants)
#define STEREO_CENTER_COMPENSATION_THRESHOLD  0.1f   // Threshold for center compensation detection
#define STEREO_CENTER_BOOST_FACTOR           1.02f   // Center boost factor to maintain perceived loudness

/**************************************************************************************
 * Summation Normalization and Volume Weighting
 **************************************************************************************/
// Intelligent volume weighting to prioritize strong oscillators over weak background noise
// INCREASED: Higher base level provides more headroom for compression without saturation
// REFACTORED: With VOLUME_AMP_RESOLUTION = 1.0, simply use the normalized value directly
#define SUMMATION_BASE_LEVEL        (0.2f)  // Base level to avoid division issues (normalized scale)

// Soft Limiter Configuration (prevents hard clipping while preserving dynamics)
// Runtime configurable values are loaded from sp3ctra.ini via g_sp3ctra_config
#define SOFT_LIMIT_THRESHOLD_DEFAULT  0.8f    // Default: 80% before soft compression
#define SOFT_LIMIT_KNEE_DEFAULT       0.1f    // Default: knee width for smooth transition

/**************************************************************************************
 * Wavetable Multi-Resolution Configuration
 *
 * Reference octave R determines the virtual sample rate of each comma table:
 *   Fs_virtual = Fs_audio × 2^R  (table is generated at this higher virtual rate)
 *   area_size per comma  = Fs_audio / (f_oct0 × 2^R)
 *   phase_inc  per note  = 2^(octave − R)   (float, enables sub-sample interpolation)
 *
 * Quality vs memory tradeoff (Fs_audio = 96 kHz, f_oct0 ≈ 65 Hz):
 *   R=0  →  area_size ≈ 1468  entries, THD_interp < −113 dB  (baseline, 8× more memory)
 *   R=3  →  area_size ≈  184  entries, THD_interp <  −76 dB  (8× less memory, optimal)
 *   R=4  →  area_size ≈   92  entries, THD_interp <  −70 dB  (16× less memory)
 *
 * Linear interpolation in the precompute loop eliminates the staircase artefact
 * present with integer step addressing, improving THD by 50–60 dB for high octaves.
 **************************************************************************************/
/* Reference octave for table generation (0 = lowest note, 3 = ~C5 ≈ 523 Hz)        */
#define WAVE_REF_OCTAVE          3

/* Safety floor: prevents degenerate tables for extreme parameters                   */
#define WAVE_TABLE_MIN_ENTRIES   16

/**************************************************************************************
 * Adaptive Slew/Decay Configuration
 * Gap Limiter is always enabled with progressive attack/release envelope
 **************************************************************************************/
// Frequency-dependent release weighting (stabilizes highs vs lows)
#define DECAY_FREQ_MIN 0.001f
#define DECAY_FREQ_MAX 1000.0f

// Numerical safety and bounds (to avoid underflow/denormals and instability)
#define TAU_UP_MAX_MS 10000.0f     // Cap extremely long attacks
#define TAU_DOWN_MAX_MS 10000.0f   // Cap extremely long releases
#define ALPHA_MIN 1e-5f           // Minimum effective alpha to ensure progress and avoid denormals

/**************************************************************************************
 * Debug Configuration
 **************************************************************************************/
// Enable debug traces for additive oscillators (compile-time flag)
#define DEBUG_OSC

// Logging Parameters
#define LOG_FREQUENCY                100  // Approximate logging interval (in callbacks), decoupled from buffer size

/**************************************************************************************
 * Auto-volume Configuration
 **************************************************************************************/
// IMU detection thresholds (hardware-dependent constants)
#define IMU_ACTIVE_THRESHOLD_X       0.01f     // Minimum IMU movement to detect activity
#define IMU_FILTER_ALPHA_X           0.25f     // Low-pass filter coefficient for IMU smoothing

// Auto-volume timing (performance-critical constant)
#define AUTO_VOLUME_POLL_MS          10        // Polling interval for auto-volume thread (ms)

/**************************************************************************************
 * Debug Auto-Freeze (for development)
 * After N received images, freeze synth data (keep reception and pipeline running).
 **************************************************************************************/
#ifndef LUXSTRAL_DEBUG_AUTOFREEZE_ENABLE
#define LUXSTRAL_DEBUG_AUTOFREEZE_ENABLE 0
#endif

#ifndef LUXSTRAL_DEBUG_AUTOFREEZE_AFTER_IMAGES
#define LUXSTRAL_DEBUG_AUTOFREEZE_AFTER_IMAGES 5000
#endif

#endif // __CONFIG_SYNTH_LUXSTRAL_H__
