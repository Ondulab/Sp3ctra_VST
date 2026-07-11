// Temporary stubs to let the minimal VST compile
// These variables will be replaced by an instantiated architecture in the final version

#include "core/context.h"
#include "config/config_loader.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// CRITICAL: Fixed buffer size - ALWAYS 400 DPI (3456 pixels)
// This allows runtime switching between 200/400 DPI without buffer reallocation
#define FIXED_BUFFER_SIZE_400DPI 3456

// 🔧 VST Global buffers for synthesis (used by processBlock)
// These are stub pointers - actual allocation in Sp3ctraCore
#include "audio/buffers/audio_image_buffers.h"
#include "threading/multithreading.h"

AudioImageBuffers *g_audioImageBuffers = NULL;
DoubleBuffer *g_doubleBuffer = NULL;

// Configuration globale - Now initialized from APVTS (AudioProcessorValueTreeState)
// These values will be set by PluginProcessor based on user preferences
sp3ctra_config_t g_sp3ctra_config = {
    // Audio settings (will be overridden by prepareToPlay)
    .sampling_frequency = 48000,
    .audio_buffer_size = 512,
    
    // Logging
    .log_level = 2, // LOG_LEVEL_INFO (default, overridden by APVTS)
    
    // Network
    .udp_address = "239.100.100.100",
    .udp_port = 55151,
    .multicast_interface = "",
    
    // Sensor
    .sensor_dpi = 400,  // DEFAULT: 400 DPI (can be 200, but buffers stay 3456 pixels)
    
    // 🔧 CRITICAL: LuxStral synthesis parameters (from sp3ctra.ini [synth_luxstral])
    .low_frequency = 65.41f,      // C2
    .high_frequency = 16744.04f,  // ~8 octaves above C2
    .start_frequency = 65.41f,    // Backward compatibility
    .pixels_per_note = 1,         // Maximum resolution
    .num_workers = 8,             // Thread pool workers
    
    // Musical scale
    .semitone_per_octave = 12,
    .num_octaves = 8,  // Fixed number of octaves (no dynamic calculation)
    .comma_per_semitone = 36,
    
    // Envelope parameters (very fast response)
    .tau_up_base_ms = 0.5f,
    .tau_down_base_ms = 0.5f,

    // Release frequency weighting — the ONLY writer of these fields (the INI
    // loader was dead code): unset they were 0.0, and synth_luxstral_algorithms
    // computed powf(f/0, -0.0) = powf(inf, -0) = 1.0 — accidentally neutral.
    // beta = 0 keeps that neutrality EXPLICIT (identical sound); raise beta
    // (e.g. -1.2 with ref 440 Hz) to actually enable the weighting.
    .decay_freq_ref_hz = 440.0f,
    .decay_freq_beta   = 0.0f,
    
    // 🔧 CRITICAL: Image processing parameters (from sp3ctra.ini [image_processing_luxstral])
    .invert_intensity = 1,                       // Dark pixels louder
    .additive_enable_non_linear_mapping = 1,     // Gamma enabled
    .additive_gamma_value = 4.8f,                // Gamma exponent
    .additive_contrast_min = 0.21f,              // Min volume for blurred images
    .additive_contrast_adjustment_power = 0.5f,  // Contrast curve exponent
    
    // 🔧 CRITICAL: Volume and dynamics (from sp3ctra.ini [summation_normalization])
    .volume_weighting_exponent = 0.1f,           // Strong oscillator domination
    .summation_response_exponent = 2.0f,         // Compression exponent
    .noise_gate_threshold = 0.005f,              // Noise suppression
    
    // 🔧 CRITICAL: Soft limiter (prevents hard clipping)
    .soft_limit_threshold = 0.8f,                // Start soft limit at 80%
    .soft_limit_knee = 0.2f,                     // Smooth transition

    // Inverse-dB decode law (always on) — encoder dB window
    .luxstral_db_decode_range_db = 50.0f,  // = SCORE_DEFAULT_DYNAMIC_RANGE_DB

    // M4 — core-side LuxSynth engine feed (mirrors lxFftBins/lxFftSmoothing)
    .lx_fft_bins_choice = 2,               // 128 harmonics (param default)
    .lx_fft_smoothing   = 0.3f,            // param default

    // Per-OUT conditioning banks (synth-split P1) — unity defaults so the
    // pipeline is sane before the first applyConfigurationToCore() sync.
    .luxstral_out = {[0 ... LUX_OUT_MAX_SLOTS - 1] =
        {.negative = 1, .dc_blocking = 1, .gamma = 1.0f,
         .contrast_min = 0.21f, .range_db = 50.0f, .intensity = 1.0f,
         .enabled = 1}},
    .luxsynth_out = {[0 ... LUX_OUT_MAX_SLOTS - 1] =
        {.negative = 1, .dc_blocking = 1, .gamma = 1.0f,
         .contrast_min = 0.21f, .range_db = 50.0f, .intensity = 1.0f,
         .enabled = 1}},
    .luxwave_out = {[0 ... LUX_OUT_MAX_SLOTS - 1] =
        {.negative = 1, .dc_blocking = 1, .gamma = 1.0f,
         .contrast_min = 0.21f, .range_db = 50.0f, .intensity = 1.0f,
         .enabled = 1}},

    // Phase management — mode FREE = legacy free-running phases
    .luxstral_phase_mode = 0,               // LUXSTRAL_PHASE_MODE_FREE
    .luxstral_phase_sensitivity = 0.7f,     // onset sensitivity (relative to recent peak)
    .luxstral_phase_position = 0.0f,        // strike/pluck position / BELL impact
    // Phase drift — per-onset random micro-detune (±cents), 0 = off
    .luxstral_phase_drift_cents = 0.0f,

    // 🔧 CRITICAL: Stereo processing
    .stereo_mode_enabled = 1,
    .stereo_blue_red_weight = 0.7f,           // Primary color axis weight
    .stereo_cyan_yellow_weight = 0.3f,        // Secondary color axis weight
    .stereo_temperature_amplification = 2.5f, // Temperature effect amplification
    .stereo_temperature_curve_exponent = 0.7f // Non-linear curve exponent (must be > 0!)
};

// NOTE: g_displayable_synth_R/G/B are now defined in synth_luxstral_state.c
// Removed stubs - using real LuxStral implementation

// NOTE: the legacy ImageSequencer ghost (g_image_sequencer, always NULL, plus
// its passthrough stub) and the empty synth_luxwave_set_image_line() stub were
// removed together with their call sites in multithreading.c — the REAL
// LuxWave feed is luxwave_engine_set_image_line() via the image pipeline.

// NOTE: synth_AudioProcess is now defined in synth_luxstral.c
// Removed stub - using real LuxStral implementation

// Note: get_cis_pixels_nb, logger_*, and load_luxstral_config are already defined in:
// - config_instrument.h (get_cis_pixels_nb - inline)
// - logger.c (logger_*)
// - config_loader.c (load_luxstral_config)
// No need to redefine them here
