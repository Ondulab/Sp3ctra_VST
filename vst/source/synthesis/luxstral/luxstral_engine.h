/*
 * luxstral_engine.h
 *
 * LuxStral engine instance state (M3 phase A — de-globalization)
 *
 * Groups ALL mutable state of the additive synthesis engine into a single
 * instantiable struct. Internal luxstral functions receive a LuxStralEngine*
 * and operate on its fields; the historical public entry points
 * (synth_AudioProcess, synth_IfftMode, init/shutdown/freeze/display APIs)
 * keep their signatures and forward to the single instance g_luxstral_engine_a.
 *
 * NOTE for external consumers (outside synthesis/luxstral/):
 *   Do NOT access struct fields directly — use the accessor functions
 *   declared at the bottom of this header (display buffers, freeze state).
 *
 * The shared sine/square tables (wave_generation.c) are READ-ONLY after init
 * and deliberately stay global — they are shared by all engine instances.
 *
 * Author: zhonx
 */

#ifndef __LUXSTRAL_ENGINE_H__
#define __LUXSTRAL_ENGINE_H__

/* Includes ------------------------------------------------------------------*/
#include "synth_luxstral_threading.h"  /* synth_thread_worker_t, rt_safe_buffer_t, barrier_t */
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types ------------------------------------------------------------*/

typedef struct LuxStralEngine {

  /* ===== DSP buffers (persistent, owned by the engine) ===================== */
  /* Final combination buffers, sized to audio_buffer_size (synth_IfftMode)   */
  float *additiveBuffer;
  float *sumVolumeBuffer;
  float *maxVolumeBuffer;
  float *tmp_audioData;
  /* Stereo temp accumulation buffers (persistently allocated)                */
  float *stereoBuffer_L;
  float *stereoBuffer_R;
  /* Reference image (normalized amplitude, micros scale)                     */
  int32_t *imageRef;
  /* Grayscale staging buffers, sized to nb_pixels (synth_AudioProcess)       */
  float *grayScale_live;      /* live grayscale input (normalized float [0,1]) */
  float *processed_grayScale; /* data passed to synth_IfftMode                 */

  /* ===== Worker pool ======================================================== */
  synth_thread_worker_t *thread_pool;  /* Dynamically allocated (num_workers)  */
  pthread_t *worker_threads;           /* Dynamically allocated (num_workers)  */
  int num_workers;                     /* Actual number of workers from config */
  _Atomic int pool_initialized;        /* RT-SAFE: C11 atomic                  */
  _Atomic int pool_shutdown;           /* RT-SAFE: C11 atomic                  */
  /* Signal to unblock workers during prepareToPlay() buffer size changes     */
  _Atomic int workers_must_exit;       /* RT-SAFE: C11 atomic                  */

  /* ===== Barriers (deterministic worker execution) ========================= */
#ifdef __linux__
  pthread_barrier_t worker_start_barrier;
  pthread_barrier_t worker_end_barrier;
#else
  barrier_t worker_start_barrier;
  barrier_t worker_end_barrier;
#endif
  _Atomic int use_barriers;            /* Enabled by default (set to 1 at def) */

  /* ===== RT-safe output double buffers ===================================== */
  rt_safe_buffer_t rt_luxstral_buffer;
  rt_safe_buffer_t rt_stereo_L_buffer;
  rt_safe_buffer_t rt_stereo_R_buffer;

  /* ===== Freeze / display state ============================================ */
  volatile int is_synth_data_frozen;
  float *frozen_grayscale_buffer;      /* Dynamic allocation (nb_pixels)       */
  volatile int is_synth_data_fading_out;
  double synth_data_fade_start_time;
  pthread_mutex_t synth_data_freeze_mutex;
  /* Edge-detection state for freeze/fade transitions (synth_AudioProcess)    */
  int prev_frozen_state;
  int prev_fading_state;
  /* Buffers for display to reflect synth data (mixed RGB)                    */
  uint8_t *displayable_synth_R;        /* Dynamic allocation (nb_pixels)       */
  uint8_t *displayable_synth_G;        /* Dynamic allocation (nb_pixels)       */
  uint8_t *displayable_synth_B;        /* Dynamic allocation (nb_pixels)       */
  pthread_mutex_t displayable_synth_mutex;

  /* ===== Sizes / counters / misc =========================================== */
  /* Mutex to ensure thread-safe synthesis processing for stereo channels     */
  pthread_mutex_t synth_process_mutex;
  /* Last calculated contrast factor (atomic for auto-volume access)          */
  _Atomic float last_contrast_factor;
  /* Track current audio buffer size for safe reallocation                    */
  int audio_buffer_size;
  /* Log limiting (periodic display)                                          */
  uint32_t log_counter;
  /* Source routing diagnostic counter (VST_MODE, synth_AudioProcess)         */
  int diag_ctr;
  /* One-time "Float32 path active" log flag (worker threads)                 */
  _Atomic int f32_path_logged;

} LuxStralEngine;

/* Exported variables --------------------------------------------------------*/

/* The single engine instance (M3 phase A). A second instance (M8) will be
 * added once the remaining shared subsystems (waves[], wave_generation
 * hot-reload state, runtime config) are made per-instance.                   */
extern LuxStralEngine g_luxstral_engine_a;

/* Public accessors for external consumers ------------------------------------
 * These wrap the display buffers of g_luxstral_engine_a so external files
 * (e.g. threading/multithreading.c) never touch the struct internals.        */
void luxstral_engine_displayable_lock(void);
void luxstral_engine_displayable_unlock(void);
uint8_t *luxstral_engine_displayable_R(void);
uint8_t *luxstral_engine_displayable_G(void);
uint8_t *luxstral_engine_displayable_B(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUXSTRAL_ENGINE_H__ */
