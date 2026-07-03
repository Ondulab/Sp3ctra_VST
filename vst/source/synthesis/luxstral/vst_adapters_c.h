/*
 * vst_adapters_c.h
 *
 * C-only adaptation layer for LuxStral engine
 * This header can be included from pure C files
 *
 * Author: zhonx
 * Created: January 2026
 */

#ifndef __VST_ADAPTERS_C_H__
#define __VST_ADAPTERS_C_H__

/* Standard C includes -------------------------------------------*/
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

/* Configuration - include actual definition ---------------------*/
// No forward declaration - include the real definition
// (will be included via doublebuffer.h anyway)

/* Configuration Macros ------------------------------------------*/

// Audio configuration from VST context
#define AUDIO_BUFFER_SIZE (g_sp3ctra_config.audio_buffer_size)

// Note: CIS_MAX_PIXELS_NB is already defined in config_instrument.h
// Note: SUMMATION_BASE_LEVEL is already defined in config_synth_luxstral.h
// We use the config values, not the hardcoded defines

// LuxStral synthesis configuration from VST context
// VOLUME_AMP_RESOLUTION is 1.0f (normalized waveforms)
#define VOLUME_AMP_RESOLUTION (1.0f)

// Log frequency for periodic messages
#define LOG_FREQUENCY 100

/* Include existing type definitions from source ----------------*/
#include "doublebuffer.h"          // DoubleBuffer
#include "context.h"                // Context
#include "config_instrument.h"      // get_cis_pixels_nb()
#include "config_loader.h"          // sp3ctra_config_t

/* Logging Functions ---------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

void vst_log_info(const char* message);
void vst_log_warning(const char* message);
void vst_log_error(const char* message);

#ifdef __cplusplus
}
#endif

/* Error Handling ------------------------------------------------*/
/* die() is declared in utils/error.h and implemented in utils/error.c  */
/* The VST edition logs the error without calling exit().                */
#include "utils/error.h"

/* Image Debug Stubs ---------------------------------------------*/
/* All image_debug_* stubs are defined in utils/image_debug_stubs.h      */
#include "utils/image_debug_stubs.h"

/* Audio Buffers -------------------------------------------------*/
/* 🔧 FIX: All accesses to ready/buffer_index MUST use __atomic_*_n builtins
 * with ACQUIRE/RELEASE ordering to ensure correct memory ordering on ARM64
 * (Apple Silicon). Plain volatile does NOT provide acquire/release semantics
 * on weakly-ordered architectures, causing stale data reads → crackling.
 * 
 * We keep volatile int (not _Atomic) for C/C++ cross-compatibility,
 * but mandate __atomic_load_n/__atomic_store_n for ALL accesses.           */
typedef struct {
    float *data;
    volatile int ready;         /* ACCESS ONLY via __atomic_*_n builtins! */
    uint64_t write_timestamp_us;
} AudioImageBuffer;

// RENAMED to avoid conflicts with audio_c_api.h
extern AudioImageBuffer luxstral_buffers_L[2];
extern AudioImageBuffer luxstral_buffers_R[2];
extern volatile int luxstral_buffer_index;  /* ACCESS ONLY via __atomic_*_n! */

/* M8 — LuxStral engine B: second independent publish target (dual-engine A/B). */
extern AudioImageBuffer luxstral_b_buffers_L[2];
extern AudioImageBuffer luxstral_b_buffers_R[2];
extern volatile int luxstral_b_buffer_index;  /* ACCESS ONLY via __atomic_*_n! */

/* VST Audio Callback Synchronization ----------------------------*/
// 🔧 LOCK-FREE: Replaced pthread_cond with atomic flag polling
// pthread_cond_signal() without mutex caused lost signals → 200ms audio gaps
extern volatile int g_vst_callback_consumed_buffer;  /* ACCESS via __atomic_*_n! */

// Compatibility macros for LuxStral code
#define buffers_L luxstral_buffers_L
#define buffers_R luxstral_buffers_R
#define current_buffer_index luxstral_buffer_index

// Audio buffer initialization functions (C-compatible declarations)
#ifdef __cplusplus
extern "C" {
#endif

int luxstral_init_audio_buffers(int buffer_size);
int luxstral_get_audio_buffer_size(void); // allocated size in samples (0 = not yet)
void luxstral_cleanup_audio_buffers(void);
void luxstral_init_callback_sync(void);
void luxstral_cleanup_callback_sync(void);
void luxstral_signal_buffer_consumed(void);  // Called by processBlock()
void luxstral_wait_for_buffer_consumed(void); // Called by audioProcessingThread()

#ifdef __cplusplus
}
// C++ only: bool return type
extern "C" bool luxstral_are_audio_buffers_ready(void);
#else
int luxstral_are_audio_buffers_ready(void);  // bool not available in C89
#endif

/* RT Profiler - use real definition from rt_profiler.h --------*/
// No stubs needed - rt_profiler.h is already included via includes chain

/* Wave Generation Types -----------------------------------------*/
// Note: wave_generation.h defines these types properly
// DO NOT redefine here to avoid conflicts

#endif /* __VST_ADAPTERS_C_H__ */
