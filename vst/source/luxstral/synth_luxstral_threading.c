/*
 * synth_luxstral_threading.c
 *
 * Thread pool management for additive synthesis
 * Contains persistent thread pool and parallel processing functionality
 *
 * Author: zhonx
 */

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters.h"
#include "synth_luxstral_threading.h"
#include "synth_luxstral_algorithms.h"
#include "synth_luxstral_math.h"
#include "pow_approx.h"
#include "wave_generation.h"
#include "strokeforge.h"
#include "../../utils/rt_profiler.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>  // For mlock() - prevent page faults in RT threads

#ifdef __linux__
#include <sched.h>
#endif

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

/* External RT Profiler */
extern RTProfiler g_rt_profiler;

/* Maximum buffer size for static allocation (industry standard) */
#define MAX_BUFFER_SIZE 4096

// Runtime-gated capture buffers: lazy allocation only when capture is enabled
static inline int synth_ensure_capture_buffers(synth_thread_worker_t *worker) {
  // Fast path: if capture disabled, do nothing
  if (!image_debug_is_oscillator_capture_enabled()) {
    return 0;
  }

  int buf = g_sp3ctra_config.audio_buffer_size;
  int notes_this = worker->end_note - worker->start_note;
  if (buf <= 0 || notes_this <= 0) return -1;

  size_t total = (size_t)buf * (size_t)notes_this;

  // If already allocated with the correct capacity, nothing to do
  if (worker->captured_current_volume && worker->captured_target_volume &&
      worker->capture_capacity_elements == total) {
    return 0;
  }

  // (Re)allocate with new capacity
  if (worker->captured_current_volume) {
    free(worker->captured_current_volume);
    worker->captured_current_volume = NULL;
  }
  if (worker->captured_target_volume) {
    free(worker->captured_target_volume);
    worker->captured_target_volume = NULL;
  }

  worker->captured_current_volume = (float *)calloc(total, sizeof(float));
  worker->captured_target_volume  = (float *)calloc(total, sizeof(float));
  if (!worker->captured_current_volume || !worker->captured_target_volume) {
    // Cleanup on partial failure
    if (worker->captured_current_volume) { free(worker->captured_current_volume); worker->captured_current_volume = NULL; }
    if (worker->captured_target_volume)  { free(worker->captured_target_volume);  worker->captured_target_volume  = NULL; }
    worker->capture_capacity_elements = 0;
    return -1;
  }
  worker->capture_capacity_elements = total;
  return 0;
}

// If capture is disabled at runtime, immediately release capture buffers to free memory
static inline void synth_release_capture_buffers_if_disabled(synth_thread_worker_t *worker) {
  if (worker->capture_capacity_elements && !image_debug_is_oscillator_capture_enabled()) {
    if (worker->captured_current_volume) { free(worker->captured_current_volume); worker->captured_current_volume = NULL; }
    if (worker->captured_target_volume)  { free(worker->captured_target_volume);  worker->captured_target_volume  = NULL; }
    worker->capture_capacity_elements = 0;
  }
}

/* External declarations -----------------------------------------------------*/
#ifdef DEBUG_OSC
extern debug_luxstral_osc_config_t g_debug_osc_config;
#endif

/* Global variables ----------------------------------------------------------*/

// Pool of persistent threads (dynamically allocated)
synth_thread_worker_t *thread_pool = NULL;
pthread_t *worker_threads = NULL;
int num_workers = 0;  // Actual number of workers from config
_Atomic int synth_pool_initialized = 0;  // RT-SAFE: C11 atomic instead of volatile
_Atomic int synth_pool_shutdown = 0;     // RT-SAFE: C11 atomic instead of volatile

// 🔧 CRITICAL FIX: Signal to unblock workers during prepareToPlay()
// When DAW changes buffer size, we need to wake up barrier-blocked workers
_Atomic int synth_workers_must_exit = 0;  // RT-SAFE: C11 atomic instead of volatile

// Barrier synchronization for deterministic execution
#ifdef __linux__
pthread_barrier_t g_worker_start_barrier;
pthread_barrier_t g_worker_end_barrier;
#else
barrier_t g_worker_start_barrier;
barrier_t g_worker_end_barrier;
#endif
_Atomic int g_use_barriers = 1;  // RT-SAFE: Enable barriers by default for deterministic execution

/* RT-safe double buffering system */
rt_safe_buffer_t g_rt_luxstral_buffer = {0};
rt_safe_buffer_t g_rt_stereo_L_buffer = {0};  
rt_safe_buffer_t g_rt_stereo_R_buffer = {0};

/* Private function implementations ------------------------------------------*/

/**
 * @brief  Initialize the persistent thread pool
 * @retval 0 on success, -1 on error
 */
int synth_init_thread_pool(void) {
  // If pool was shutdown but not fully cleaned, force cleanup first
  if (synth_pool_shutdown) {
    log_warning("SYNTH", "Pool was in shutdown state, forcing cleanup before re-init");
    synth_shutdown_thread_pool();
  }

  if (synth_pool_initialized)
    return 0;
    
  // Reset shutdown flags for new session
  synth_pool_shutdown = 0;
  synth_workers_must_exit = 0;

  // Get number of workers from config (with validation)
  num_workers = g_sp3ctra_config.num_workers;
  if (num_workers < 1 || num_workers > MAX_WORKERS) {
    log_warning("SYNTH", "Invalid num_workers=%d, clamping to range [1, %d]", num_workers, MAX_WORKERS);
    num_workers = (num_workers < 1) ? 1 : MAX_WORKERS;
  }
  
  log_info("SYNTH", "Initializing thread pool with %d workers", num_workers);
  
  // Allocate thread pool and worker threads arrays
  thread_pool = (synth_thread_worker_t*)calloc(num_workers, sizeof(synth_thread_worker_t));
  worker_threads = (pthread_t*)calloc(num_workers, sizeof(pthread_t));
  
  if (!thread_pool || !worker_threads) {
    log_error("SYNTH", "Failed to allocate thread pool arrays");
    if (thread_pool) { free(thread_pool); thread_pool = NULL; }
    if (worker_threads) { free(worker_threads); worker_threads = NULL; }
    return -1;
  }

  // Initialize barrier synchronization (Phase 2: Deterministic execution)
  if (g_use_barriers) {
    // num_workers + 1 for main thread
    if (synth_init_barriers(num_workers + 1) != 0) {
      log_warning("SYNTH", "Failed to initialize barriers, falling back to condition variables");
      g_use_barriers = 0;
    }
  }

  int current_notes = get_current_number_of_notes();
  int notes_per_thread = current_notes / num_workers;

  for (int i = 0; i < num_workers; i++) {
    synth_thread_worker_t *worker = &thread_pool[i];

    // Worker configuration
    worker->thread_id = i;
    worker->start_note = i * notes_per_thread;
    // Last worker handles all remaining notes (handles rounding)
    worker->end_note = (i == num_workers - 1) ? current_notes : (i + 1) * notes_per_thread;

    // ✅ STATIC ALLOCATION: Use MAX_BUFFER_SIZE for all per-sample buffers
    // Industry standard: allocate once for maximum buffer size (4096)
    // Memory cost: ~114 MB for 8 workers (negligible on modern systems)
    // Benefit: No reallocation needed when DAW changes buffer size
    {
      int notes_this = worker->end_note - worker->start_note;
      
      // Allocate per-sample buffers with MAX_BUFFER_SIZE (static allocation)
      worker->thread_luxstralBuffer = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->thread_sumVolumeBuffer = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->thread_maxVolumeBuffer = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->thread_luxstralBuffer_L = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->thread_luxstralBuffer_R = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->waveBuffer = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->volumeBuffer = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      
      // Work buffers (per-note, independent of buffer size)
      worker->imageBuffer_q31 = (int32_t*)calloc(notes_this, sizeof(int32_t));
      worker->imageBuffer_f32 = (float*)calloc(notes_this, sizeof(float));
      
      // Precomputed arrays: per note × MAX_BUFFER_SIZE (static allocation)
      size_t total_max = (size_t)notes_this * MAX_BUFFER_SIZE;
      // precomputed_new_idx removed: phase continuity is now tracked via float
      // phase_acc/phase_inc and committed in synth_precompute_wave_data().
      worker->precomputed_wave_data = (float*)calloc(total_max, sizeof(float));
      
      // Precomputed volume and pan data (per note, independent of buffer size)
      worker->precomputed_volume = (float*)calloc(notes_this, sizeof(float));
      worker->precomputed_pan_position = (float*)calloc(notes_this, sizeof(float));
      worker->precomputed_left_gain = (float*)calloc(notes_this, sizeof(float));
      worker->precomputed_right_gain = (float*)calloc(notes_this, sizeof(float));
      
      // Last applied gains for ramping (per note, independent of buffer size)
      worker->last_left_gain = (float*)calloc(notes_this, sizeof(float));
      worker->last_right_gain = (float*)calloc(notes_this, sizeof(float));
      
      // Capture buffers are lazy-allocated only when capture is enabled
      worker->captured_current_volume = NULL;
      worker->captured_target_volume = NULL;
      worker->capture_capacity_elements = 0;
      
      // Stereo temp buffers with MAX_BUFFER_SIZE (static allocation)
      worker->temp_waveBuffer_L = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      worker->temp_waveBuffer_R = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
      
      // Check all allocations
      if (!worker->thread_luxstralBuffer || !worker->thread_sumVolumeBuffer || !worker->thread_maxVolumeBuffer ||
          !worker->thread_luxstralBuffer_L || !worker->thread_luxstralBuffer_R || !worker->waveBuffer || !worker->volumeBuffer ||
          !worker->imageBuffer_q31 || !worker->imageBuffer_f32 ||
          !worker->precomputed_wave_data || !worker->precomputed_volume ||
          !worker->precomputed_pan_position || !worker->precomputed_left_gain || !worker->precomputed_right_gain ||
          !worker->last_left_gain || !worker->last_right_gain ||
          !worker->temp_waveBuffer_L || !worker->temp_waveBuffer_R) {
        log_error("SYNTH", "Error allocating worker buffers for thread %d", i);
        return -1;
      }
      
      // Initialize last pan gains for per-buffer ramping (center equal-power)
      // ALSO initialize precomputed gains as fallback in case stereo data is not copied
      for (int idx = 0; idx < notes_this; idx++) {
        worker->last_left_gain[idx] = 0.707f;
        worker->last_right_gain[idx] = 0.707f;
        worker->precomputed_left_gain[idx] = 0.707f;
        worker->precomputed_right_gain[idx] = 0.707f;
      }
    }

    // Initialize synchronization
    if (pthread_mutex_init(&worker->work_mutex, NULL) != 0) {
      log_error("SYNTH", "Error initializing mutex for thread %d", i);
      return -1;
    }
    if (pthread_cond_init(&worker->work_cond, NULL) != 0) {
      log_error("SYNTH", "Error initializing condition for thread %d", i);
      return -1;
    }
  }

  synth_pool_initialized = 1;
  return 0;
}

/**
 * @brief  Main function for persistent worker threads
 * @param  arg Pointer to synth_thread_worker_t structure
 * @retval NULL pointer
 */
void *synth_persistent_worker_thread(void *arg) {
  synth_thread_worker_t *worker = (synth_thread_worker_t *)arg;

  // 🔧 RT PRIORITY: Set QoS to USER_INTERACTIVE for this worker thread
  // Must be called from within the thread itself (pthread_set_qos_class_self_np)
#ifdef __APPLE__
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0) {
    log_startup_detail("SYNTH", "Worker %d: QoS set to USER_INTERACTIVE", worker->thread_id);
  }
#endif

  while (!synth_pool_shutdown && !synth_workers_must_exit) {
    // Deterministic execution with barriers
    // Wait at start barrier for all workers + main thread
    synth_barrier_wait(&g_worker_start_barrier);
    
    // 🔧 CRITICAL: Check exit flags immediately after barrier wakeup
    if (synth_pool_shutdown || synth_workers_must_exit)
      break;
    
    // Perform the work (Float32 path)
    synth_process_worker_range(worker);
    
    // Wait at end barrier for all workers to complete
    synth_barrier_wait(&g_worker_end_barrier);
    
    // 🔧 CRITICAL: Check exit flags after end barrier too
    if (synth_pool_shutdown || synth_workers_must_exit)
      break;
  }

  return NULL;
}


// Global atomic flag to log Float32 path only once across all workers
static _Atomic int g_f32_path_logged = 0;

/**
 * @brief  Process a range of notes for a given worker (Float32 version)
 * @param  worker Pointer to worker structure
 * @retval None
 */
void synth_process_worker_range(synth_thread_worker_t *worker) {
  int32_t buff_idx, note, local_note_idx;
  
  // Thread-safe one-time log using atomic compare-exchange
  int expected = 0;
  if (atomic_compare_exchange_strong(&g_f32_path_logged, &expected, 1)) {
    log_info("SYNTH", "Float32 path active in worker threads");
  }

  // Release capture buffers if capture was disabled since last buffer
  synth_release_capture_buffers_if_disabled(worker);

  // Initialize output buffers to zero
  fill_float(0, worker->thread_luxstralBuffer, g_sp3ctra_config.audio_buffer_size);
  fill_float(0, worker->thread_sumVolumeBuffer, g_sp3ctra_config.audio_buffer_size);
  fill_float(0, worker->thread_maxVolumeBuffer, g_sp3ctra_config.audio_buffer_size);

  // Initialize stereo buffers - CRITICAL FIX: must zero these buffers! (always present)
  fill_float(0, worker->thread_luxstralBuffer_L, g_sp3ctra_config.audio_buffer_size);
  fill_float(0, worker->thread_luxstralBuffer_R, g_sp3ctra_config.audio_buffer_size);

  // DEPRECATED: Old preprocessing removed - now using preprocessed_data.additive.notes[]
  // The preprocessing is done centrally in image_preprocessor.c
  // Data is already: RGB → Grayscale → Inversion → Gamma → Averaging → Contrast

  // ✅ OPTIMIZATION: Hoist invariant calculations and improve cache locality
  const int audio_buffer_size = g_sp3ctra_config.audio_buffer_size;
  const int stereo_enabled = g_sp3ctra_config.stereo_mode_enabled;
  const float volume_weighting_exp = g_sp3ctra_config.volume_weighting_exponent;
  const int capture_enabled = image_debug_is_oscillator_capture_enabled();
  
  // Main note processing loop - optimized for cache efficiency
  for (note = worker->start_note; note < worker->end_note; note++) {
    local_note_idx = note - worker->start_note;

    // -----------------------------------------------------------------------
    // Sine precompute — PARALLEL (moved from synth_precompute_wave_data Phase 3).
    //
    // Each worker fills its own disjoint precomputed_wave_data range; no mutex
    // or lock needed.  waves[note].phase_acc write is safe because workers
    // process disjoint note ranges (no two workers share the same note index).
    //
    // Shared g_sine_table[1024] + g_square_table[1024] both stay in L1 cache.
    // g_waveform_morph: 0.0=pure sine, 1.0=pure square — single volatile read
    // before the note loop avoids repeated volatile dereferences in the hot path.
    // -----------------------------------------------------------------------

    /* One volatile read per synthesis cycle — safe, relaxed ordering sufficient */
    const float morph = g_waveform_morph;
    {
      float*      pre_wave_w = worker->precomputed_wave_data +
                               (size_t)local_note_idx * audio_buffer_size;
      float       phase = waves[note].phase_acc;
      const float inc   = waves[note].phase_inc;
      const float fsize = (float)SINE_TABLE_SIZE;
      for (int s = 0; s < audio_buffer_size; s++) {
        phase += inc;
        if (phase >= fsize) phase -= fsize;
        const int   i0   = (int)phase;
        const float frac = phase - (float)i0;
        const int   i1   = (i0 + 1) & SINE_TABLE_MASK;
        /* Waveform morph: lerp between sine and square at the same phase position.
         * morph=0 → pure sine  |  morph=1 → pure square (bandlimited, WAVETABLE_HARMONICS odd harmonics)
         * Linear interpolation inside each table preserves sub-sample accuracy. */
        {
            float sine_s   = g_sine_table[i0]   + frac * (g_sine_table[i1]   - g_sine_table[i0]);
            float square_s = g_square_table[i0] + frac * (g_square_table[i1] - g_square_table[i0]);
            pre_wave_w[s]  = sine_s + morph * (square_s - sine_s);
        }
      }
      waves[note].phase_acc = phase;  /* safe: disjoint per-worker ranges */
    }

    // ✅ OPTIMIZATION: Prefetch next iteration data (improves cache hit rate)
    if (note + 1 < worker->end_note) {
      __builtin_prefetch(&worker->precomputed_volume[local_note_idx + 1], 0, 3);
      __builtin_prefetch(&worker->precomputed_wave_data[(size_t)(local_note_idx + 1) * audio_buffer_size], 0, 3);
    }
    
    // Use preprocessed volume data (already has: RGB → Grayscale → Inversion → Gamma → Averaging)
    float target_volume = worker->precomputed_volume[local_note_idx];

    // ✅ OPTIMIZATION: Compute pointers once (avoid repeated address calculations)
    const float* pre_wave = worker->precomputed_wave_data + (size_t)local_note_idx * audio_buffer_size;
    float* wave_buf = worker->waveBuffer;
    float* vol_buf = worker->volumeBuffer;
    
    // Generate waveform samples
    generate_waveform_samples(note, wave_buf, pre_wave);

    // Apply GAP_LIMITER envelope
    apply_gap_limiter_ramp(note, target_volume, pre_wave, vol_buf);

    // Debug capture (fast path when disabled)
    if (capture_enabled) {
      if (synth_ensure_capture_buffers(worker) == 0) {
        memcpy(worker->captured_current_volume + (size_t)local_note_idx * audio_buffer_size,
               vol_buf,
               sizeof(float) * (size_t)audio_buffer_size);
        fill_float(waves[note].target_volume,
                   worker->captured_target_volume + (size_t)local_note_idx * audio_buffer_size,
                   (size_t)audio_buffer_size);
      }
    }

    // Apply volume scaling to the current note waveform
    mult_float(wave_buf, vol_buf, wave_buf, audio_buffer_size);

    // ✅ OPTIMIZATION: Update max volume buffer inline (better cache locality)
    for (buff_idx = audio_buffer_size; --buff_idx >= 0;) {
      if (vol_buf[buff_idx] > worker->thread_maxVolumeBuffer[buff_idx]) {
        worker->thread_maxVolumeBuffer[buff_idx] = vol_buf[buff_idx];
      }
    }

    // ✅ OPTIMIZATION: Conditional stereo/mono processing (hoisted check)
    if (stereo_enabled) {
      // Stereo mode: Apply per-oscillator panning with per-buffer ramp
      const float start_left  = worker->last_left_gain[local_note_idx];
      const float start_right = worker->last_right_gain[local_note_idx];
      const float end_left    = worker->precomputed_left_gain[local_note_idx];
      const float end_right   = worker->precomputed_right_gain[local_note_idx];

      // Use optimized stereo panning function (NEON-accelerated on ARM)
      apply_stereo_pan_ramp(wave_buf, 
                           worker->temp_waveBuffer_L, 
                           worker->temp_waveBuffer_R,
                           start_left, start_right, end_left, end_right,
                           audio_buffer_size);

      // Persist end-gains for next buffer ramp
      worker->last_left_gain[local_note_idx]  = end_left;
      worker->last_right_gain[local_note_idx] = end_right;
      
      // ✅ OPTIMIZATION: Direct pointer usage (avoid temp variables)
      add_float(worker->temp_waveBuffer_L, worker->thread_luxstralBuffer_L,
                worker->thread_luxstralBuffer_L, audio_buffer_size);
      add_float(worker->temp_waveBuffer_R, worker->thread_luxstralBuffer_R,
                worker->thread_luxstralBuffer_R, audio_buffer_size);
    } else {
      // Mono mode: Duplicate mono signal to both L/R channels (center panning)
      add_float(wave_buf, worker->thread_luxstralBuffer_L,
                worker->thread_luxstralBuffer_L, audio_buffer_size);
      add_float(wave_buf, worker->thread_luxstralBuffer_R,
                worker->thread_luxstralBuffer_R, audio_buffer_size);
    }

    // LuxStral summation for mono or combined processing
    add_float(wave_buf, worker->thread_luxstralBuffer,
              worker->thread_luxstralBuffer, audio_buffer_size);
    
    // Intelligent volume weighting: strong oscillators dominate over weak background noise
    // ✅ OPTIMIZATION: Use hoisted constant for weighting exponent
    apply_volume_weighting(worker->thread_sumVolumeBuffer, vol_buf,
                          volume_weighting_exp, audio_buffer_size);

    // NOTE: phase_acc is committed at the top of this loop (sine precompute block).
    //       Each worker owns a disjoint note range → no concurrent write conflict.
  }

  // NOTE: RT-safe buffer writing removed - causes audio corruption
  // Workers only write to their local buffers, main thread combines them
}

/**
 * @brief  Pre-compute waves[] data in parallel to avoid contention
 * @param  imageData Input image data
 * @param  db DoubleBuffer for accessing preprocessed stereo data
 * @retval None
 */
void synth_precompute_wave_data(float *imageData, DoubleBuffer *db) {
  // ✅ CRITICAL OPTIMIZATION: Batch read all preprocessed data in ONE mutex lock
  // BEFORE: 6912 mutex locks per buffer (2 locks × 3456 notes) = massive contention!
  // AFTER: 1 mutex lock per buffer = 6912x reduction in lock overhead
  
  // Phase 1: Image data assignment (thread-safe, read-only)
  for (int i = 0; i < num_workers; i++) {
    thread_pool[i].imageData = imageData;
  }

  // Phase 2: Batch copy ALL preprocessed data with a SINGLE mutex lock
  // RT PROFILER: Measure mutex contention
  struct timeval mutex_start, mutex_end;
  gettimeofday(&mutex_start, NULL);
  rt_profiler_mutex_lock_start(&g_rt_profiler);
  
  pthread_mutex_lock(&db->mutex);
  
  gettimeofday(&mutex_end, NULL);
  int64_t sec_diff = (int64_t)(mutex_end.tv_sec - mutex_start.tv_sec);
  int64_t usec_diff = (int64_t)(mutex_end.tv_usec - mutex_start.tv_usec);
  uint64_t wait_us = (uint64_t)(sec_diff * 1000000LL + usec_diff);
  rt_profiler_mutex_lock_end(&g_rt_profiler, wait_us);
  
  // Copy all preprocessed data for all workers in one shot
  for (int i = 0; i < num_workers; i++) {
    synth_thread_worker_t *worker = &thread_pool[i];
    int notes_this_worker = worker->end_note - worker->start_note;
    
    // Batch copy volume data
    memcpy(worker->precomputed_volume,
           &db->preprocessed_data.additive.notes[worker->start_note],
           notes_this_worker * sizeof(float));
    
    // Batch copy stereo data if enabled
    if (g_sp3ctra_config.stereo_mode_enabled) {
      memcpy(worker->precomputed_pan_position,
             &db->preprocessed_data.stereo.pan_positions[worker->start_note],
             notes_this_worker * sizeof(float));
      memcpy(worker->precomputed_left_gain,
             &db->preprocessed_data.stereo.left_gains[worker->start_note],
             notes_this_worker * sizeof(float));
      memcpy(worker->precomputed_right_gain,
             &db->preprocessed_data.stereo.right_gains[worker->start_note],
             notes_this_worker * sizeof(float));
    }
    
    // StrokeForge: Mode-aware amplitude override.
    //
    // Three exclusive modes (set by strokeforge_analyze_frame in preprocessor):
    //
    //   BYPASS       — blob_count != 1 (zero or multiple blobs).
    //                  Complex spectral scene: StrokeForge harmonics from blob A
    //                  would collide with blob B → bell-like beating artifacts.
    //                  Solution: skip entirely, pure spectral passthrough.
    //
    //   PHASE_SMOOTH — Single thin blob (width < STROKEFORGE_WAVETABLE_MIN_WIDTH).
    //                  Phase hints only: note_target_phase[] is set for notes
    //                  inside the blob, but NO amplitude changes.
    //                  Spectral synthesis runs unmodified.
    //
    //   WAVETABLE    — Single wide isolated blob.
    //                  Full amplitude control: spectral amplitude suppressed
    //                  inside the blob range, replaced by pulse-wave harmonic
    //                  recipe.  Notes outside the blob are untouched.
    if (g_sp3ctra_config.strokeforge_enabled) {
      const StrokeForgeFrameData *sf = &db->preprocessed_data.strokeforge;

      if (sf->frame_mode == STROKEFORGE_MODE_WAVETABLE) {
        // WAVETABLE: suppress spectral inside blob, inject harmonic amplitudes
        for (int n = 0; n < notes_this_worker; n++) {
          int global_note = worker->start_note + n;
          if (global_note >= STROKEFORGE_MAX_NOTES) break;

          float sf_amp = sf->note_harmonic_amplitude[global_note];
          if (sf_amp > 0.0f) {
            // Harmonic target → use pulse-wave amplitude
            worker->precomputed_volume[n] = sf_amp;
          } else if (sf->note_to_blob[global_note] != STROKEFORGE_NO_BLOB) {
            // Inside blob but not a harmonic target → mute spectral
            // (blob range is fully controlled by the wavetable recipe)
            worker->precomputed_volume[n] = 0.0f;
          }
          // Outside blob → pure spectral passthrough (unchanged)
        }
      }
      // BYPASS or PHASE_SMOOTH: no amplitude changes — spectral runs clean
    }
  }
  
  pthread_mutex_unlock(&db->mutex);

  // DEBUG: Log first worker's precomputed volume and stereo gains (disabled for production)
  // static int debug_log_counter = 0;
  // if ((debug_log_counter++ % 500) == 0 && num_workers > 0) {
  //   float vol_sum = 0.0f;
  //   float vol_max = 0.0f;
  //   float lg_sum = 0.0f, rg_sum = 0.0f;  // Stereo gain sums
  //   int notes_this = thread_pool[0].end_note - thread_pool[0].start_note;
  //   for (int k = 0; k < notes_this && k < 100; k++) {
  //     float v = thread_pool[0].precomputed_volume[k];
  //     vol_sum += v;
  //     if (v > vol_max) vol_max = v;
  //     lg_sum += thread_pool[0].precomputed_left_gain[k];
  //     rg_sum += thread_pool[0].precomputed_right_gain[k];
  //   }
  //   log_info("SYNTH_DBG", "precomputed_volume: sum=%.6f max=%.6f notes=%d", vol_sum, vol_max, notes_this);
  //   log_info("SYNTH_DBG", "precomputed_gains: stereo=%d leftSum=%.6f rightSum=%.6f", 
  //            g_sp3ctra_config.stereo_mode_enabled, lg_sum, rg_sum);
  // }

  // Phase 3 (REMOVED): sine precompute loop was single-threaded here.
  // It is now executed IN PARALLEL inside each worker's synth_process_worker_range()
  // at the top of the per-note loop, using each worker's disjoint note range.
  //
  // Impact: 3456 × buffer_len sine interpolations go from O(1 thread) to O(N workers).
  //   Before: all 3456 notes computed serially on the audio thread → bottleneck at 96 kHz
  //   After:  each worker computes ceil(3456/N) notes → ~N× speedup on the precompute phase
  //
  // Thread-safety: each worker writes to disjoint waves[start..end-1].phase_acc ranges.
  //
  // ✅ PERFORMANCE: 1 mutex lock per buffer (volume/pan copy above) — unchanged.
}

/**
 * @brief  Start worker threads with CPU affinity and RT priorities
 * @retval 0 on success, -1 on error
 */
int synth_start_worker_threads(void) {
  int rt_success_count = 0;  // Track how many workers got RT priority
  
  for (int i = 0; i < num_workers; i++) {
    if (pthread_create(&worker_threads[i], NULL, synth_persistent_worker_thread,
                       &thread_pool[i]) != 0) {
      log_error("SYNTH", "Error creating worker thread %d", i);
      return -1;
    }

    // ✅ PHASE 1: Set RT priority for deterministic execution
#if defined(__linux__) || defined(__APPLE__)
    if (synth_set_rt_priority(worker_threads[i], 80) == 0) {
      rt_success_count++;
    }
    // Note: Individual failure warnings are logged in synth_set_rt_priority()
#endif

    // ✅ OPTIMIZATION: CPU affinity to balance load on Pi5
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    // Distribute threads across available CPUs (leave CPU 0 for system if possible)
    // For num_workers <= 7, use CPUs 1-7; for num_workers == 8, use CPUs 0-7
    int cpu_id = (num_workers <= 7) ? (i + 1) : i;
    CPU_SET(cpu_id, &cpuset);

    int result =
        pthread_setaffinity_np(worker_threads[i], sizeof(cpu_set_t), &cpuset);
    if (result == 0) {
      log_startup_detail("SYNTH", "Worker thread %d assigned to CPU %d", i, cpu_id);
    } else {
      log_warning("SYNTH", "Cannot assign thread %d to CPU %d (error: %d)", i, cpu_id, result);
    }
#endif
  }
  
  // Condensed summary log (always shown in NORMAL mode)
#if defined(__linux__) || defined(__APPLE__)
  if (rt_success_count == num_workers) {
    log_info("SYNTH", "RT priority enabled for all %d worker threads", num_workers);
  } else if (rt_success_count > 0) {
    log_info("SYNTH", "RT priority enabled for %d/%d worker threads", rt_success_count, num_workers);
  } else {
    log_info("SYNTH", "RT priority not available (continuing without RT for %d workers)", num_workers);
  }
#endif
  
  return 0;
}

/**
 * @brief  Stop the persistent thread pool
 * @retval None
 */
void synth_shutdown_thread_pool(void) {
  if (!synth_pool_initialized)
    return;

  log_info("SYNTH", "Initiating thread pool shutdown...");
  
  // 🔧 CRITICAL FIX: Set shutdown flags FIRST
  synth_pool_shutdown = 1;
  synth_workers_must_exit = 1;

  // 🔧 ULTRA-CRITICAL FIX: If workers are blocked on barriers, we need to JOIN them
  // to unblock them. This simulates the main thread rejoining the barriers one last time.
  if (g_use_barriers) {
    log_info("SYNTH", "Performing final barrier sync to unblock workers...");
    
    // Try to join start barrier (if workers are waiting there)
    // This will either:
    // - Succeed if workers are waiting (unblocks them)
    // - Return immediately if no one is waiting
    // - Return error if workers already passed (safe to ignore)
    int start_result = synth_barrier_wait(&g_worker_start_barrier);
    if (start_result == 0 || start_result == -1) {
      log_info("SYNTH", "Joined start barrier, workers can proceed to exit check");
      
      // Now join end barrier if they proceeded to work phase
      int end_result = synth_barrier_wait(&g_worker_end_barrier);
      if (end_result == 0 || end_result == -1) {
        log_info("SYNTH", "Joined end barrier, workers should exit now");
      }
    }
    
    // Additional broadcast to catch any edge cases
#ifndef __linux__
    // macOS: Broadcast on barrier condition variables
    pthread_mutex_lock(&g_worker_start_barrier.mutex);
    g_worker_start_barrier.generation++;
    g_worker_start_barrier.waiting = 0;
    pthread_cond_broadcast(&g_worker_start_barrier.cond);
    pthread_mutex_unlock(&g_worker_start_barrier.mutex);
    
    pthread_mutex_lock(&g_worker_end_barrier.mutex);
    g_worker_end_barrier.generation++;
    g_worker_end_barrier.waiting = 0;
    pthread_cond_broadcast(&g_worker_end_barrier.cond);
    pthread_mutex_unlock(&g_worker_end_barrier.mutex);
#else
    // Linux: Destroy and recreate with count=1
    pthread_barrier_destroy(&g_worker_start_barrier);
    pthread_barrier_destroy(&g_worker_end_barrier);
    pthread_barrier_init(&g_worker_start_barrier, NULL, 1);
    pthread_barrier_init(&g_worker_end_barrier, NULL, 1);
#endif
  }

  // Wake up all threads via condition variables (legacy/fallback)
  for (int i = 0; i < num_workers; i++) {
    pthread_mutex_lock(&thread_pool[i].work_mutex);
    pthread_cond_signal(&thread_pool[i].work_cond);
    pthread_mutex_unlock(&thread_pool[i].work_mutex);
  }

  // 🔧 CRITICAL: Give workers a moment to process the exit signal
  usleep(50000);  // 50ms grace period for clean exit

  // Wait for all threads to terminate
  log_info("SYNTH", "Waiting for worker threads to terminate...");
  for (int i = 0; i < num_workers; i++) {
    pthread_join(worker_threads[i], NULL);
    log_info("SYNTH", "Worker thread %d terminated", i);

    // Free dynamically allocated worker buffers
    free(thread_pool[i].thread_luxstralBuffer);    thread_pool[i].thread_luxstralBuffer = NULL;
    free(thread_pool[i].thread_sumVolumeBuffer);   thread_pool[i].thread_sumVolumeBuffer = NULL;
    free(thread_pool[i].thread_maxVolumeBuffer);   thread_pool[i].thread_maxVolumeBuffer = NULL;
    free(thread_pool[i].thread_luxstralBuffer_L);  thread_pool[i].thread_luxstralBuffer_L = NULL;
    free(thread_pool[i].thread_luxstralBuffer_R);  thread_pool[i].thread_luxstralBuffer_R = NULL;
    free(thread_pool[i].waveBuffer);               thread_pool[i].waveBuffer = NULL;
    free(thread_pool[i].volumeBuffer);             thread_pool[i].volumeBuffer = NULL;
    free(thread_pool[i].imageBuffer_q31);          thread_pool[i].imageBuffer_q31 = NULL;
    free(thread_pool[i].imageBuffer_f32);          thread_pool[i].imageBuffer_f32 = NULL;
    free(thread_pool[i].precomputed_wave_data);    thread_pool[i].precomputed_wave_data = NULL;
    free(thread_pool[i].precomputed_volume);       thread_pool[i].precomputed_volume = NULL;
    free(thread_pool[i].precomputed_pan_position); thread_pool[i].precomputed_pan_position = NULL;
    free(thread_pool[i].precomputed_left_gain);    thread_pool[i].precomputed_left_gain = NULL;
    free(thread_pool[i].precomputed_right_gain);   thread_pool[i].precomputed_right_gain = NULL;
    free(thread_pool[i].last_left_gain);           thread_pool[i].last_left_gain = NULL;
    free(thread_pool[i].last_right_gain);          thread_pool[i].last_right_gain = NULL;
    free(thread_pool[i].captured_current_volume);  thread_pool[i].captured_current_volume = NULL;
    free(thread_pool[i].captured_target_volume);   thread_pool[i].captured_target_volume = NULL;
    free(thread_pool[i].temp_waveBuffer_L);        thread_pool[i].temp_waveBuffer_L = NULL;
    free(thread_pool[i].temp_waveBuffer_R);        thread_pool[i].temp_waveBuffer_R = NULL;

    pthread_mutex_destroy(&thread_pool[i].work_mutex);
    pthread_cond_destroy(&thread_pool[i].work_cond);
  }

  // Free the dynamically allocated arrays
  if (thread_pool) {
    free(thread_pool);
    thread_pool = NULL;
  }
  if (worker_threads) {
    free(worker_threads);
    worker_threads = NULL;
  }
  num_workers = 0;

  if (g_sp3ctra_config.stereo_mode_enabled) {
    // Cleanup lock-free pan gains system
    lock_free_pan_cleanup();
    log_info("SYNTH", "Lock-free pan system cleaned up");
  }

  // Cleanup barrier synchronization
  if (g_use_barriers) {
    synth_cleanup_barriers();
    log_info("SYNTH", "Barrier synchronization cleaned up");
  }

  synth_pool_initialized = 0;
  log_info("SYNTH", "Thread pool shutdown complete");
}

/**
 * @brief  Initialize RT-safe double buffering system
 * @retval 0 on success, -1 on error
 */
int init_rt_safe_buffers(void) {
  // ✅ STATIC ALLOCATION: Use MAX_BUFFER_SIZE for RT-safe buffers
  // No reallocation needed when DAW changes buffer size
  
  // Initialize additive buffer with MAX_BUFFER_SIZE
  g_rt_luxstral_buffer.buffers[0] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  g_rt_luxstral_buffer.buffers[1] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  if (!g_rt_luxstral_buffer.buffers[0] || !g_rt_luxstral_buffer.buffers[1]) {
    log_error("SYNTH", "Failed to allocate RT additive buffers");
    return -1;
  }
  g_rt_luxstral_buffer.ready_buffer = 0;  // RT reads from buffer 0 initially
  g_rt_luxstral_buffer.worker_buffer = 1; // Workers write to buffer 1 initially
  pthread_mutex_init(&g_rt_luxstral_buffer.swap_mutex, NULL);

  // Initialize stereo L buffer with MAX_BUFFER_SIZE
  g_rt_stereo_L_buffer.buffers[0] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  g_rt_stereo_L_buffer.buffers[1] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  if (!g_rt_stereo_L_buffer.buffers[0] || !g_rt_stereo_L_buffer.buffers[1]) {
    log_error("SYNTH", "Failed to allocate RT stereo L buffers");
    return -1;
  }
  g_rt_stereo_L_buffer.ready_buffer = 0;
  g_rt_stereo_L_buffer.worker_buffer = 1;
  pthread_mutex_init(&g_rt_stereo_L_buffer.swap_mutex, NULL);

  // Initialize stereo R buffer with MAX_BUFFER_SIZE
  g_rt_stereo_R_buffer.buffers[0] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  g_rt_stereo_R_buffer.buffers[1] = (float*)calloc(MAX_BUFFER_SIZE, sizeof(float));
  if (!g_rt_stereo_R_buffer.buffers[0] || !g_rt_stereo_R_buffer.buffers[1]) {
    log_error("SYNTH", "Failed to allocate RT stereo R buffers");
    return -1;
  }
  g_rt_stereo_R_buffer.ready_buffer = 0;
  g_rt_stereo_R_buffer.worker_buffer = 1;
  pthread_mutex_init(&g_rt_stereo_R_buffer.swap_mutex, NULL);

  // ========================================================================
  // 🔧 RT OPTIMIZATION: Lock RT-safe buffers in memory to prevent page faults
  // ========================================================================
  size_t buffer_bytes = (size_t)MAX_BUFFER_SIZE * sizeof(float);
  int mlock_success = 0;
  int mlock_total = 6;  // 3 buffer types × 2 double-buffer slots
  
  if (mlock(g_rt_luxstral_buffer.buffers[0], buffer_bytes) == 0) mlock_success++;
  if (mlock(g_rt_luxstral_buffer.buffers[1], buffer_bytes) == 0) mlock_success++;
  if (mlock(g_rt_stereo_L_buffer.buffers[0], buffer_bytes) == 0) mlock_success++;
  if (mlock(g_rt_stereo_L_buffer.buffers[1], buffer_bytes) == 0) mlock_success++;
  if (mlock(g_rt_stereo_R_buffer.buffers[0], buffer_bytes) == 0) mlock_success++;
  if (mlock(g_rt_stereo_R_buffer.buffers[1], buffer_bytes) == 0) mlock_success++;
  
  if (mlock_success == mlock_total) {
    log_info("SYNTH", "RT-safe buffers locked in memory (mlock) - page faults prevented");
  } else if (mlock_success > 0) {
    log_warning("SYNTH", "Partial mlock: %d/%d RT-safe buffers locked", mlock_success, mlock_total);
  }
  
  log_info("SYNTH", "RT-safe double buffering system initialized (MAX_BUFFER_SIZE=%d)", MAX_BUFFER_SIZE);
  return 0;
}

/**
 * @brief  Cleanup RT-safe double buffering system
 * @retval None
 */
void cleanup_rt_safe_buffers(void) {
  // Cleanup additive buffer
  if (g_rt_luxstral_buffer.buffers[0]) { free(g_rt_luxstral_buffer.buffers[0]); g_rt_luxstral_buffer.buffers[0] = NULL; }
  if (g_rt_luxstral_buffer.buffers[1]) { free(g_rt_luxstral_buffer.buffers[1]); g_rt_luxstral_buffer.buffers[1] = NULL; }
  pthread_mutex_destroy(&g_rt_luxstral_buffer.swap_mutex);

  // Cleanup stereo L buffer
  if (g_rt_stereo_L_buffer.buffers[0]) { free(g_rt_stereo_L_buffer.buffers[0]); g_rt_stereo_L_buffer.buffers[0] = NULL; }
  if (g_rt_stereo_L_buffer.buffers[1]) { free(g_rt_stereo_L_buffer.buffers[1]); g_rt_stereo_L_buffer.buffers[1] = NULL; }
  pthread_mutex_destroy(&g_rt_stereo_L_buffer.swap_mutex);

  // Cleanup stereo R buffer
  if (g_rt_stereo_R_buffer.buffers[0]) { free(g_rt_stereo_R_buffer.buffers[0]); g_rt_stereo_R_buffer.buffers[0] = NULL; }
  if (g_rt_stereo_R_buffer.buffers[1]) { free(g_rt_stereo_R_buffer.buffers[1]); g_rt_stereo_R_buffer.buffers[1] = NULL; }
  pthread_mutex_destroy(&g_rt_stereo_R_buffer.swap_mutex);

  log_info("SYNTH", "RT-safe double buffering system cleaned up");
}

/**
 * @brief  Swap RT-safe buffers when workers are done (called from non-RT thread)
 * @retval None
 */
void rt_safe_swap_buffers(void) {
  // Swap additive buffer (non-blocking for non-RT thread)
  pthread_mutex_lock(&g_rt_luxstral_buffer.swap_mutex);
  int old_ready = g_rt_luxstral_buffer.ready_buffer;
  g_rt_luxstral_buffer.ready_buffer = g_rt_luxstral_buffer.worker_buffer;
  g_rt_luxstral_buffer.worker_buffer = old_ready;
  pthread_mutex_unlock(&g_rt_luxstral_buffer.swap_mutex);

  // Swap stereo L buffer  
  pthread_mutex_lock(&g_rt_stereo_L_buffer.swap_mutex);
  old_ready = g_rt_stereo_L_buffer.ready_buffer;
  g_rt_stereo_L_buffer.ready_buffer = g_rt_stereo_L_buffer.worker_buffer;
  g_rt_stereo_L_buffer.worker_buffer = old_ready;
  pthread_mutex_unlock(&g_rt_stereo_L_buffer.swap_mutex);

  // Swap stereo R buffer
  pthread_mutex_lock(&g_rt_stereo_R_buffer.swap_mutex);
  old_ready = g_rt_stereo_R_buffer.ready_buffer;
  g_rt_stereo_R_buffer.ready_buffer = g_rt_stereo_R_buffer.worker_buffer;
  g_rt_stereo_R_buffer.worker_buffer = old_ready;
  pthread_mutex_unlock(&g_rt_stereo_R_buffer.swap_mutex);
}
