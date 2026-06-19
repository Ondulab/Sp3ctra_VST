/*
 * synth_luxstral_threading.h
 *
 * Thread pool management for additive synthesis
 * Contains persistent thread pool and parallel processing functionality
 *
 * Author: zhonx
 */

#ifndef __SYNTH_LUXSTRAL_THREADING_H__
#define __SYNTH_LUXSTRAL_THREADING_H__

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters.h"
#include "config/config_instrument.h"  // For CIS_MAX_PIXELS_NB
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdatomic.h>  // For _Atomic variables (RT-safe with -O2)

/* Forward declarations ------------------------------------------------------*/
struct DoubleBuffer;
struct LuxStralEngine;  /* Engine instance state (defined in luxstral_engine.h) */

/* Exported types ------------------------------------------------------------*/

/**
 * @brief  Structure for persistent thread pool worker optimized for synthesis
 */
typedef struct synth_thread_worker_s {
  struct LuxStralEngine *engine; // Owning engine (back-pointer for pool/barrier state)
  int thread_id;      // Thread ID (0, 1, 2)
  int start_note;     // Start note for this thread
  int end_note;       // End note for this thread
  float *imageData;   // Input image data (shared, normalized float [0, 1])

  // Local output buffers per thread - Float32 (legacy)
  float *thread_luxstralBuffer;
  float *thread_sumVolumeBuffer;
  float *thread_maxVolumeBuffer;
  
  // Stereo buffers for direct L/R accumulation (always present) - Float32
  // In mono mode: L = R = duplicated signal
  // In stereo mode: L and R with per-oscillator panning
  float *thread_luxstralBuffer_L;
  float *thread_luxstralBuffer_R;


  // Local work buffers (avoids VLA on stack) - Float32 (dynamically allocated)
  int32_t *imageBuffer_q31; // Dynamically allocated based on notes_per_thread
  float *imageBuffer_f32;   // Dynamically allocated based on notes_per_thread
  float *waveBuffer;
  float *volumeBuffer;

  // Temporary stereo work buffers (persistently allocated to avoid VLAs)
  float *temp_waveBuffer_L;
  float *temp_waveBuffer_R;
  

  // Pre-computed waves[] data (read-only)
  // NOTE: precomputed_new_idx removed — phase is now a float (phase_acc/phase_inc)
  //       and is committed directly inside synth_precompute_wave_data() (single-threaded).
  float *precomputed_wave_data; // size: (notes_per_thread * g_sp3ctra_config.audio_buffer_size)
  float *precomputed_volume;    // Dynamically allocated based on notes_per_thread
  
  // Pre-computed pan positions and gains for each note (dynamically allocated)
  float *precomputed_pan_position;
  float *precomputed_left_gain;
  float *precomputed_right_gain;

  // Persistent last applied gains for per-buffer ramping (zipper-noise mitigation)
  float *last_left_gain;
  float *last_right_gain;

  // Debug capture: per-note per-sample volumes (current and target) for this buffer
  float *captured_current_volume; // size: (notes_per_thread * g_sp3ctra_config.audio_buffer_size)
  float *captured_target_volume; // size: (notes_per_thread * g_sp3ctra_config.audio_buffer_size)
  size_t capture_capacity_elements; // number of elements allocated across capture buffers; 0 when disabled

  // Per-worker timing instrumentation (captured inside worker thread)
  struct timeval worker_start_time;
  struct timeval worker_end_time;
  uint64_t worker_time_sum_us;
  uint64_t worker_time_max_us;
  int worker_timing_sample_count;

  // Synchronization (kept for shutdown signaling only)
  pthread_mutex_t work_mutex;
  pthread_cond_t work_cond;

} synth_thread_worker_t;

/* Barrier synchronization for deterministic execution (cross-platform) */
/* NOTE: the barrier instances live in LuxStralEngine (luxstral_engine.h) */
#ifndef __linux__
// macOS doesn't have pthread_barrier, use custom implementation
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int count;
  int waiting;
  int generation;
} barrier_t;

int barrier_init(barrier_t *barrier, int count);
int barrier_wait(struct LuxStralEngine *eng, barrier_t *barrier);
int barrier_destroy(barrier_t *barrier);
#endif

/* Exported function prototypes ----------------------------------------------*/

/* Thread pool management */
int synth_init_thread_pool(struct LuxStralEngine *eng);
int synth_start_worker_threads(struct LuxStralEngine *eng);
void synth_shutdown_thread_pool(void);  // Public entry point (atexit/shared core)

/* RT deterministic threading (Phase 1 & 2) */
int synth_init_barriers(struct LuxStralEngine *eng, int num_threads);
void synth_cleanup_barriers(struct LuxStralEngine *eng);
int synth_set_rt_priority(pthread_t thread, int priority);
int synth_barrier_wait(struct LuxStralEngine *eng, void *barrier);

/* Thread processing functions */
void *synth_persistent_worker_thread(void *arg);
void synth_process_worker_range(synth_thread_worker_t *worker);
void synth_precompute_wave_data(struct LuxStralEngine *eng, float *imageData, struct DoubleBuffer *db);

/* Thread pool limits */
#define MAX_WORKERS 16  // Maximum number of worker threads (M-series: 10–12 perf cores)

/* RT-safe double buffering system */
typedef struct {
  // Double buffers for RT-safe access
  float *buffers[2]; // [0] = current RT reads, [1] = workers write
  _Atomic int ready_buffer;  // RT-SAFE: Which buffer is ready for RT (atomic read)
  _Atomic int worker_buffer; // RT-SAFE: Which buffer workers are writing to
  pthread_mutex_t swap_mutex; // Protects buffer swapping (non-RT thread only)
} rt_safe_buffer_t;

/* RT-safe buffer management (buffers live in LuxStralEngine) */
int init_rt_safe_buffers(struct LuxStralEngine *eng);
void cleanup_rt_safe_buffers(struct LuxStralEngine *eng);
void rt_safe_swap_buffers(struct LuxStralEngine *eng); // Called by workers when done (non-RT)

#endif /* __SYNTH_LUXSTRAL_THREADING_H__ */
