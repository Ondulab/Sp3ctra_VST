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
#include "luxstral_engine.h"
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

/* Global variables ----------------------------------------------------------*/

/* NOTE: the worker pool and barriers now live in LuxStralEngine
 * (luxstral_engine.h). The engine instance is defined in
 * synth_luxstral.c and passed down to every function in this file.          */

/* Private function prototypes -----------------------------------------------*/
static void synth_shutdown_thread_pool_impl(LuxStralEngine *eng);

/* Private function implementations ------------------------------------------*/

/**
 * @brief  Initialize the persistent thread pool
 * @param  eng Engine instance
 * @retval 0 on success, -1 on error
 */
int synth_init_thread_pool(LuxStralEngine *eng) {
  // If pool was shutdown but not fully cleaned, force cleanup first
  if (eng->pool_shutdown) {
    log_warning("SYNTH", "Pool was in shutdown state, forcing cleanup before re-init");
    synth_shutdown_thread_pool_impl(eng);
  }

  if (eng->pool_initialized)
    return 0;

  // Reset shutdown flags for new session
  eng->pool_shutdown = 0;
  eng->workers_must_exit = 0;

  // Get number of workers from config (with validation)
  eng->num_workers = g_sp3ctra_config.num_workers;
  if (eng->num_workers < 1 || eng->num_workers > MAX_WORKERS) {
    log_warning("SYNTH", "Invalid num_workers=%d, clamping to range [1, %d]", eng->num_workers, MAX_WORKERS);
    eng->num_workers = (eng->num_workers < 1) ? 1 : MAX_WORKERS;
  }

  log_info("SYNTH", "Initializing thread pool with %d workers", eng->num_workers);

  // Allocate thread pool and worker threads arrays
  eng->thread_pool = (synth_thread_worker_t*)calloc(eng->num_workers, sizeof(synth_thread_worker_t));
  eng->worker_threads = (pthread_t*)calloc(eng->num_workers, sizeof(pthread_t));

  if (!eng->thread_pool || !eng->worker_threads) {
    log_error("SYNTH", "Failed to allocate thread pool arrays");
    if (eng->thread_pool) { free(eng->thread_pool); eng->thread_pool = NULL; }
    if (eng->worker_threads) { free(eng->worker_threads); eng->worker_threads = NULL; }
    return -1;
  }

  // Initialize barrier synchronization (Phase 2: Deterministic execution)
  if (eng->use_barriers) {
    // num_workers + 1 for main thread
    if (synth_init_barriers(eng, eng->num_workers + 1) != 0) {
      log_warning("SYNTH", "Failed to initialize barriers, falling back to condition variables");
      eng->use_barriers = 0;
    }
  }

  int current_notes = get_current_number_of_notes();
  int notes_per_thread = current_notes / eng->num_workers;

  for (int i = 0; i < eng->num_workers; i++) {
    synth_thread_worker_t *worker = &eng->thread_pool[i];

    // Worker configuration
    worker->engine = eng;  // Back-pointer to owning engine
    worker->thread_id = i;
    worker->start_note = i * notes_per_thread;
    // Last worker handles all remaining notes (handles rounding)
    worker->end_note = (i == eng->num_workers - 1) ? current_notes : (i + 1) * notes_per_thread;

    // Phase-drift RNG stream — distinct per worker AND per engine so A/B
    // never draw correlated detunes. Any nonzero seed works (xorshift32).
    worker->rng_state = 0x9E3779B9u
                      ^ ((uint32_t)(worker->start_note + 1) * 2654435761u)
                      ^ ((eng == &g_luxstral_engine_b) ? 0xA5A5A5A5u : 0u);
    if (worker->rng_state == 0) worker->rng_state = 1;
    worker->min_target_volume = 1.0f;   // resting-bed tracker (drained per buffer)

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

  eng->pool_initialized = 1;
  return 0;
}

/**
 * @brief  Main function for persistent worker threads
 * @param  arg Pointer to synth_thread_worker_t structure
 * @retval NULL pointer
 */
void *synth_persistent_worker_thread(void *arg) {
  synth_thread_worker_t *worker = (synth_thread_worker_t *)arg;
  LuxStralEngine *eng = worker->engine;

  // 🔧 RT PRIORITY: Set QoS to USER_INTERACTIVE for this worker thread
  // Must be called from within the thread itself (pthread_set_qos_class_self_np)
#ifdef __APPLE__
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0) {
    log_startup_detail("SYNTH", "Worker %d: QoS set to USER_INTERACTIVE", worker->thread_id);
  }
#endif

  while (!eng->pool_shutdown && !eng->workers_must_exit) {
    // Deterministic execution with barriers
    // Wait at start barrier for all workers + main thread
    synth_barrier_wait(eng, &eng->worker_start_barrier);

    // 🔧 CRITICAL: Check exit flags immediately after barrier wakeup
    if (eng->pool_shutdown || eng->workers_must_exit) {
      // Rejoin the end barrier before exiting: the shutdown thread joins BOTH
      // barriers (synth_shutdown_thread_pool_impl). On Linux
      // pthread_barrier_wait has no exit-flag escape, so exiting without this
      // join left the shutdown thread blocked forever on the end barrier.
      // On macOS the custom barrier returns immediately (flags checked).
      synth_barrier_wait(eng, &eng->worker_end_barrier);
      break;
    }

    // Perform the work (Float32 path)
    synth_process_worker_range(worker);

    // Wait at end barrier for all workers to complete
    synth_barrier_wait(eng, &eng->worker_end_barrier);

    // 🔧 CRITICAL: Check exit flags after end barrier too
    if (eng->pool_shutdown || eng->workers_must_exit)
      break;
  }

  return NULL;
}

/**
 * @brief  Process a range of notes for a given worker (Float32 version)
 * @param  worker Pointer to worker structure
 * @retval None
 */
void synth_process_worker_range(synth_thread_worker_t *worker) {
  int32_t buff_idx, note, local_note_idx;

  // Thread-safe one-time log using atomic compare-exchange (per-engine flag)
  int expected = 0;
  if (atomic_compare_exchange_strong(&worker->engine->f32_path_logged, &expected, 1)) {
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
  // Per-engine stereo flag (M8): engine B has its own Stereo toggle.
  const int stereo_enabled = (worker->engine == &g_luxstral_engine_b)
      ? g_sp3ctra_config.luxstral_b_stereo_mode_enabled
      : g_sp3ctra_config.stereo_mode_enabled;
  // Volume weighting: FIXED exponent 2 — thread_sumVolumeBuffer accumulates
  // the physical energy Σa² required by the RMS ceiling (rms_ceiling_gain in
  // synth_luxstral.c, BOTH modes). The old user exponent fed the retired
  // summation normalization and is no longer read.
  const float volume_weighting_exp = 2.0f;
  // Phase management (mode + auto-calibrated gate) — see config_loader.h.
  // The absolute gate is computed by the producer (drain block, after the
  // end barrier) from the rolling max note volume × sensitivity: workers
  // read last frame's value, strictly ordered by the barriers.
  const int   phase_mode         = g_sp3ctra_config.luxstral_phase_mode;
  const float gate_raw           = worker->engine->phase_gate_abs;
  const float phase_reset_thresh = (gate_raw > 0.003f) ? gate_raw : 0.003f;
  const float phase_pos          = g_sp3ctra_config.luxstral_phase_position;
  const int   phase_active       = (phase_mode != LUXSTRAL_PHASE_MODE_FREE);
  // BELL impact seed — quantized from the position knob (16 distinct impacts).
  const uint32_t bell_seed = ((uint32_t)(phase_pos * 15.99f) + 1u) * 0x9E3779B9u;
  // Phase drift (companion of the modes): ±cents redrawn per onset, pre-scaled
  // to a phase_inc offset factor. ln(2)/1200 linearizes 2^(c/1200) — exact to
  // ~1e-6 over the ±3 cents range, no pow() in the RT path.
  const float drift_scale =
      g_sp3ctra_config.luxstral_phase_drift_cents * 5.7762265e-4f;
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

    /* ── Phase management ─────────────────────────────────────────────────
     * RE-ARM AT HALF THE GATE, NOT AT ABSOLUTE SILENCE. The always-on dB
     * decode law floors an EMPTY pixel at 10^(-range/20) (0.0032 @ 50 dB,
     * never 0), so between notes the envelope rests on that bed — 30 dB
     * ABOVE the old ε = 1e-4 re-arm point. With the old condition a note
     * that had sounded once could never re-arm: only WHITE (a forced true
     * 0.0) exposed the feature, which is why WHITE→PLAY was audible while
     * sampler hits changed nothing. The gate is floored at 4× the tracked
     * resting bed (see the producer drain), so rearm = gate/2 ≥ 2× bed:
     * notes re-arm as soon as their envelope falls back near the bed, and
     * an onset phase jump is ≥ 6 dB below the attack that masks it.
     * True idle decorrelation (fresh random phase, and what makes a mode
     * change forget the previous organization) still requires REAL silence
     * (≤ ε): scrambling phases every buffer at the bed level would sum to
     * broadband noise ≈ bed × √N. Evaluated before the sine precompute so
     * it takes effect from this buffer's first sample. Workers own
     * disjoint note ranges: no races (xorshift32 stream is worker-local). */
    if (phase_active &&
        worker->engine->waves[note].current_volume <=
            phase_reset_thresh * 0.5f) {
      uint32_t x = worker->rng_state;
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      worker->rng_state = x;
      float phase = (float)(x & (uint32_t)SINE_TABLE_MASK); /* random draw */

      if (worker->precomputed_volume[local_note_idx] >= phase_reset_thresh) {
        switch (phase_mode) {
          case LUXSTRAL_PHASE_MODE_STRIKE: {
            /* Struck string: the hammer imparts VELOCITY → sine start
             * (phase 0 or π). Sign alternates every 1/position notes
             * (floor parity ≈ sign of sin(n·π·p)); position 0 → the whole
             * band lands on 0 = the maximal coherent snap. */
            const int flip = ((int)((float)note * phase_pos)) & 1;
            phase = flip ? (float)(SINE_TABLE_SIZE / 2) : 0.0f;
            break;
          }
          case LUXSTRAL_PHASE_MODE_PLUCK: {
            /* Plucked string: initial SHAPE, zero velocity → every partial
             * starts at an extremum (cosine, ±π/2), same sign pattern. */
            const int flip = ((int)((float)note * phase_pos)) & 1;
            phase = flip ? (float)(3 * SINE_TABLE_SIZE / 4)
                         : (float)(SINE_TABLE_SIZE / 4);
            break;
          }
          case LUXSTRAL_PHASE_MODE_BELL: {
            /* Percussion: phases set by the impact point — arbitrary but
             * FIXED per (note, impact): every re-strike is bit-identical
             * (sampler-repeatable) yet the band never combs. */
            uint32_t h = (uint32_t)note * 2654435761u ^ bell_seed;
            h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13;
            phase = (float)(h & (uint32_t)SINE_TABLE_MASK);
            break;
          }
          default:
            /* BREATH (reed/flute): the attack grows out of turbulence —
             * keep the fresh random idle draw, different at every attack. */
            break;
        }

        /* Phase drift: redraw this note's micro-detune for the life of the
         * new note. Aligned onsets otherwise leave the active band beating
         * in lockstep (log-regular grid → same Δf everywhere) = flanger
         * comb; a random ±cents per note de-regularizes the beat rates so
         * the attack coherence melts into ensemble texture, higher notes
         * first. drift=0 writes offset 0 → exact grid pitch.              */
        uint32_t x2 = worker->rng_state;
        x2 ^= x2 << 13; x2 ^= x2 >> 17; x2 ^= x2 << 5;
        worker->rng_state = x2;
        const float r = (float)(int32_t)x2 * 4.6566129e-10f;  /* ∈ [-1, 1) */
        worker->engine->waves[note].detune_offset = r * drift_scale;
        worker->phase_reset_count++; /* diagnostics — drained by producer */
        worker->engine->waves[note].phase_acc = phase;   /* mode's phase law */
      } else if (worker->engine->waves[note].current_volume <=
                 LUXSTRAL_PHASE_RESET_SILENCE_EPS) {
        /* True silence only: idle decorrelation. Armed-but-resting notes
         * (bed level) keep their phase — scrambling them every buffer would
         * inject broadband noise ≈ bed × √N into the mix. */
        worker->engine->waves[note].phase_acc = phase;   /* fresh random */
      }
    }

    /* Per-engine morph snapshot (M8) — copied from THIS engine's db in
     * synth_precompute_wave_data(); the global g_waveform_morph holds the last
     * pipeline call's frame and would cross-talk between engines A and B. */
    const float morph = worker->engine->sf_morph;
    {
      float*      pre_wave_w = worker->precomputed_wave_data +
                               (size_t)local_note_idx * audio_buffer_size;
      float       phase = worker->engine->waves[note].phase_acc;
      /* Phase drift applied here — hoisted per note/buffer, zero per-sample
       * cost. detune_offset = 0 → bit-exact legacy increment.               */
      const float inc   = worker->engine->waves[note].phase_inc *
                          (1.0f + worker->engine->waves[note].detune_offset);
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
      worker->engine->waves[note].phase_acc = phase;  /* per-engine + disjoint per-worker ranges */
    }

    // ✅ OPTIMIZATION: Prefetch next iteration data (improves cache hit rate)
    if (note + 1 < worker->end_note) {
      __builtin_prefetch(&worker->precomputed_volume[local_note_idx + 1], 0, 3);
      __builtin_prefetch(&worker->precomputed_wave_data[(size_t)(local_note_idx + 1) * audio_buffer_size], 0, 3);
    }
    
    // Use preprocessed volume data (already has: RGB → Grayscale → Inversion → Gamma → Averaging)
    float target_volume = worker->precomputed_volume[local_note_idx];

    // Phase management telemetry: max feeds the auto-gate reference, min
    // tracks the decode law's resting bed (the gate is floored above it so
    // the re-arm hysteresis can see through the bed). Gated on the feature
    // flag — short-circuits to nothing when off.
    if (phase_active) {
      if (target_volume > worker->max_target_volume)
        worker->max_target_volume = target_volume;
      if (target_volume < worker->min_target_volume)
        worker->min_target_volume = target_volume;
    }

    // ✅ OPTIMIZATION: Compute pointers once (avoid repeated address calculations)
    const float* pre_wave = worker->precomputed_wave_data + (size_t)local_note_idx * audio_buffer_size;
    float* wave_buf = worker->waveBuffer;
    float* vol_buf = worker->volumeBuffer;
    
    // Generate waveform samples
    generate_waveform_samples(note, wave_buf, pre_wave);

    // Apply GAP_LIMITER envelope
    apply_gap_limiter_ramp(worker->engine->waves, note, target_volume, pre_wave, vol_buf);

    // Debug capture (fast path when disabled)
    if (capture_enabled) {
      if (synth_ensure_capture_buffers(worker) == 0) {
        memcpy(worker->captured_current_volume + (size_t)local_note_idx * audio_buffer_size,
               vol_buf,
               sizeof(float) * (size_t)audio_buffer_size);
        fill_float(worker->engine->waves[note].target_volume,
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
 * @param  eng Engine instance
 * @param  imageData Input image data
 * @param  db DoubleBuffer for accessing preprocessed stereo data
 * @retval None
 */
void synth_precompute_wave_data(LuxStralEngine *eng, float *imageData, DoubleBuffer *db) {
  // ✅ CRITICAL OPTIMIZATION: Batch read all preprocessed data in ONE mutex lock
  // BEFORE: 6912 mutex locks per buffer (2 locks × 3456 notes) = massive contention!
  // AFTER: 1 mutex lock per buffer = 6912x reduction in lock overhead

  // Phase 1: Image data assignment (thread-safe, read-only)
  for (int i = 0; i < eng->num_workers; i++) {
    eng->thread_pool[i].imageData = imageData;
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
  
  // Per-engine stereo flag (M8): engine B has its own Stereo toggle, and its
  // pan data comes from ITS pipeline config (cfg_b.stereo_enabled matches).
  const int eng_stereo_enabled = (eng == &g_luxstral_engine_b)
      ? g_sp3ctra_config.luxstral_b_stereo_mode_enabled
      : g_sp3ctra_config.stereo_mode_enabled;

  // StrokeForge morph is per-frame, per-engine data (M8): snapshot this db's
  // analysed morph under the mutex; workers read eng->sf_morph (the global
  // g_waveform_morph would leak the LAST pipeline call's frame across engines).
  eng->sf_morph = db->preprocessed_data.strokeforge.morph;

  // Copy all preprocessed data for all workers in one shot
  for (int i = 0; i < eng->num_workers; i++) {
    synth_thread_worker_t *worker = &eng->thread_pool[i];
    int notes_this_worker = worker->end_note - worker->start_note;

    // Batch copy volume data
    memcpy(worker->precomputed_volume,
           &db->preprocessed_data.additive.notes[worker->start_note],
           notes_this_worker * sizeof(float));

    // Batch copy stereo data if enabled
    if (eng_stereo_enabled) {
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
    
    // StrokeForge: Gaussian focus attenuation.
    //
    // note_attenuation[n] is pre-computed by strokeforge_analyze_frame():
    //   Outside all blobs : 1.0  (spectral passthrough, unchanged)
    //   Inside a blob     : exp( -(n - center)² / (2 × focus_sigma²) )
    //                       center_note → attenuation = 1.0 (full volume)
    //                       neighbors  → attenuated   (fewer active oscillators)
    //
    // This concentrates the energy at the drawn frequency; only the center
    // of each stroke plays at full volume.  The waveform morph (sine→square)
    // is controlled separately via g_waveform_morph (written by strokeforge.c).
    //
    // The Gaussian focus applies whenever StrokeForge is enabled (the master).
    // Focus Only is a modifier that only toggles the morph (handled in
    // strokeforge.c), so BOTH ON-modes apply the focus; when OFF, blob_count is
    // 0 (strokeforge.c early-returns) so nothing is applied here.
    if (g_sp3ctra_config.strokeforge_enabled) {
      const StrokeForgeFrameData *sf = &db->preprocessed_data.strokeforge;
      if (sf->blob_count > 0) {
        for (int n = 0; n < notes_this_worker; n++) {
          int global_note = worker->start_note + n;
          if (global_note >= STROKEFORGE_MAX_NOTES) break;
          worker->precomputed_volume[n] *= sf->note_attenuation[global_note];
        }
      }
      /* No blobs → no attenuation, pure spectral passthrough */
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
 * @param  eng Engine instance
 * @retval 0 on success, -1 on error
 */
int synth_start_worker_threads(LuxStralEngine *eng) {
  int rt_success_count = 0;  // Track how many workers got RT priority

  for (int i = 0; i < eng->num_workers; i++) {
    if (pthread_create(&eng->worker_threads[i], NULL, synth_persistent_worker_thread,
                       &eng->thread_pool[i]) != 0) {
      log_error("SYNTH", "Error creating worker thread %d", i);
      return -1;
    }

    // ✅ PHASE 1: Set RT priority for deterministic execution
#if defined(__linux__) || defined(__APPLE__)
    if (synth_set_rt_priority(eng->worker_threads[i], 80) == 0) {
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
    int cpu_id = (eng->num_workers <= 7) ? (i + 1) : i;
    CPU_SET(cpu_id, &cpuset);

    int result =
        pthread_setaffinity_np(eng->worker_threads[i], sizeof(cpu_set_t), &cpuset);
    if (result == 0) {
      log_startup_detail("SYNTH", "Worker thread %d assigned to CPU %d", i, cpu_id);
    } else {
      log_warning("SYNTH", "Cannot assign thread %d to CPU %d (error: %d)", i, cpu_id, result);
    }
#endif
  }

  // Condensed summary log (always shown in NORMAL mode)
#if defined(__linux__) || defined(__APPLE__)
  if (rt_success_count == eng->num_workers) {
    log_info("SYNTH", "RT priority enabled for all %d worker threads", eng->num_workers);
  } else if (rt_success_count > 0) {
    log_info("SYNTH", "RT priority enabled for %d/%d worker threads", rt_success_count, eng->num_workers);
  } else {
    log_info("SYNTH", "RT priority not available (continuing without RT for %d workers)", eng->num_workers);
  }
#endif

  return 0;
}

/**
 * @brief  Stop the persistent thread pool
 * @param  eng Engine instance
 * @retval None
 */
static void synth_shutdown_thread_pool_impl(LuxStralEngine *eng) {
  if (!eng->pool_initialized)
    return;

  log_info("SYNTH", "Initiating thread pool shutdown...");

  // 🔧 CRITICAL FIX: Set shutdown flags FIRST
  eng->pool_shutdown = 1;
  eng->workers_must_exit = 1;

  // 🔧 ULTRA-CRITICAL FIX: If workers are blocked on barriers, we need to JOIN them
  // to unblock them. This simulates the main thread rejoining the barriers one last time.
  if (eng->use_barriers) {
    log_info("SYNTH", "Performing final barrier sync to unblock workers...");

    // Join the start barrier: workers parked there wake up, see the exit flags
    // and rejoin the END barrier before exiting (see synth_worker_thread) —
    // so ALWAYS join the end barrier too, regardless of the start result.
    // On Linux pthread_barrier_wait returns PTHREAD_BARRIER_SERIAL_THREAD for
    // one arbitrary thread; the old `== 0 || == -1` guard skipped the end join
    // in that case, leaving every worker (and this thread on the next call)
    // blocked forever — frozen DAW on unload.
    int start_result = synth_barrier_wait(eng, &eng->worker_start_barrier);
    (void)start_result;
    log_info("SYNTH", "Joined start barrier, workers can proceed to exit check");
    int end_result = synth_barrier_wait(eng, &eng->worker_end_barrier);
    (void)end_result;
    log_info("SYNTH", "Joined end barrier, workers should exit now");

    // Additional broadcast to catch any edge cases
#ifndef __linux__
    // macOS: Broadcast on barrier condition variables
    pthread_mutex_lock(&eng->worker_start_barrier.mutex);
    eng->worker_start_barrier.generation++;
    eng->worker_start_barrier.waiting = 0;
    pthread_cond_broadcast(&eng->worker_start_barrier.cond);
    pthread_mutex_unlock(&eng->worker_start_barrier.mutex);

    pthread_mutex_lock(&eng->worker_end_barrier.mutex);
    eng->worker_end_barrier.generation++;
    eng->worker_end_barrier.waiting = 0;
    pthread_cond_broadcast(&eng->worker_end_barrier.cond);
    pthread_mutex_unlock(&eng->worker_end_barrier.mutex);
#else
    // Linux: Destroy and recreate with count=1
    pthread_barrier_destroy(&eng->worker_start_barrier);
    pthread_barrier_destroy(&eng->worker_end_barrier);
    pthread_barrier_init(&eng->worker_start_barrier, NULL, 1);
    pthread_barrier_init(&eng->worker_end_barrier, NULL, 1);
#endif
  }

  // Wake up all threads via condition variables (legacy/fallback)
  for (int i = 0; i < eng->num_workers; i++) {
    pthread_mutex_lock(&eng->thread_pool[i].work_mutex);
    pthread_cond_signal(&eng->thread_pool[i].work_cond);
    pthread_mutex_unlock(&eng->thread_pool[i].work_mutex);
  }

  // 🔧 CRITICAL: Give workers a moment to process the exit signal
  usleep(50000);  // 50ms grace period for clean exit

  // Wait for all threads to terminate
  log_info("SYNTH", "Waiting for worker threads to terminate...");
  for (int i = 0; i < eng->num_workers; i++) {
    pthread_join(eng->worker_threads[i], NULL);
    log_info("SYNTH", "Worker thread %d terminated", i);

    // Free dynamically allocated worker buffers
    free(eng->thread_pool[i].thread_luxstralBuffer);    eng->thread_pool[i].thread_luxstralBuffer = NULL;
    free(eng->thread_pool[i].thread_sumVolumeBuffer);   eng->thread_pool[i].thread_sumVolumeBuffer = NULL;
    free(eng->thread_pool[i].thread_maxVolumeBuffer);   eng->thread_pool[i].thread_maxVolumeBuffer = NULL;
    free(eng->thread_pool[i].thread_luxstralBuffer_L);  eng->thread_pool[i].thread_luxstralBuffer_L = NULL;
    free(eng->thread_pool[i].thread_luxstralBuffer_R);  eng->thread_pool[i].thread_luxstralBuffer_R = NULL;
    free(eng->thread_pool[i].waveBuffer);               eng->thread_pool[i].waveBuffer = NULL;
    free(eng->thread_pool[i].volumeBuffer);             eng->thread_pool[i].volumeBuffer = NULL;
    free(eng->thread_pool[i].imageBuffer_q31);          eng->thread_pool[i].imageBuffer_q31 = NULL;
    free(eng->thread_pool[i].imageBuffer_f32);          eng->thread_pool[i].imageBuffer_f32 = NULL;
    free(eng->thread_pool[i].precomputed_wave_data);    eng->thread_pool[i].precomputed_wave_data = NULL;
    free(eng->thread_pool[i].precomputed_volume);       eng->thread_pool[i].precomputed_volume = NULL;
    free(eng->thread_pool[i].precomputed_pan_position); eng->thread_pool[i].precomputed_pan_position = NULL;
    free(eng->thread_pool[i].precomputed_left_gain);    eng->thread_pool[i].precomputed_left_gain = NULL;
    free(eng->thread_pool[i].precomputed_right_gain);   eng->thread_pool[i].precomputed_right_gain = NULL;
    free(eng->thread_pool[i].last_left_gain);           eng->thread_pool[i].last_left_gain = NULL;
    free(eng->thread_pool[i].last_right_gain);          eng->thread_pool[i].last_right_gain = NULL;
    free(eng->thread_pool[i].captured_current_volume);  eng->thread_pool[i].captured_current_volume = NULL;
    free(eng->thread_pool[i].captured_target_volume);   eng->thread_pool[i].captured_target_volume = NULL;
    free(eng->thread_pool[i].temp_waveBuffer_L);        eng->thread_pool[i].temp_waveBuffer_L = NULL;
    free(eng->thread_pool[i].temp_waveBuffer_R);        eng->thread_pool[i].temp_waveBuffer_R = NULL;

    pthread_mutex_destroy(&eng->thread_pool[i].work_mutex);
    pthread_cond_destroy(&eng->thread_pool[i].work_cond);
  }

  // Free the dynamically allocated arrays
  if (eng->thread_pool) {
    free(eng->thread_pool);
    eng->thread_pool = NULL;
  }
  if (eng->worker_threads) {
    free(eng->worker_threads);
    eng->worker_threads = NULL;
  }
  eng->num_workers = 0;

  // Cleanup barrier synchronization
  if (eng->use_barriers) {
    synth_cleanup_barriers(eng);
    log_info("SYNTH", "Barrier synchronization cleaned up");
  }

  eng->pool_initialized = 0;
  log_info("SYNTH", "Thread pool shutdown complete");
}

/**
 * @brief  Stop the persistent thread pool (public entry point)
 * @note   Signature kept for external callers (Sp3ctraSharedCore, atexit)
 * @retval None
 */
void synth_shutdown_thread_pool(void) {
  synth_shutdown_thread_pool_impl(&g_luxstral_engine_a);
  // M8 — engine B (no-op if its pool never initialised)
  if (g_luxstral_engine_b.pool_initialized)
    synth_shutdown_thread_pool_impl(&g_luxstral_engine_b);
}
