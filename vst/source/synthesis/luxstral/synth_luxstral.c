/*
 * synth_luxstral.c - Main additive synthesis module (refactored)
 *
 * This file serves as the main entry point for the additive synthesis system.
 * The actual implementation has been split into specialized modules:
 * - synth_luxstral_algorithms.c: Centralized core algorithms
 * - synth_luxstral_math.c: Mathematical operations and utilities
 * - synth_luxstral_stereo.c: Stereo processing and panning
 * - synth_luxstral_state.c: State management and data freeze functionality
 * - synth_luxstral_threading.c: Multi-threading and worker management
 *
 * Created on: 24 avr. 2019
 * Author: zhonx
 * Refactored: 21 sep. 2025
 */

// VST Adapter layer (C-only version for pure C files)
#include "vst_adapters_c.h"

// Engine instance state (M3 phase A de-globalization)
#include "luxstral_engine.h"

// Include all the specialized modules
#include "synth_luxstral_algorithms.h"
#include "synth_luxstral_math.h"
#include "pow_approx.h"
#include "synth_luxstral_stereo.h"
#include "synth_luxstral_state.h"
#include "synth_luxstral_threading.h"
#include "synth_luxstral_runtime.h"

// Main header
#include "synth_luxstral.h"

// Wave generation
#include "wave_generation.h"

// Image preprocessing (fallback when no UDP data yet)
#include "../processing/image_preprocessor.h"
#include "../processing/image_pipeline.h"

#ifdef __APPLE__
#include <stdlib.h>
#else
#include <stdlib.h>
#endif


/* Engine instance -----------------------------------------------------------*/
/* The single LuxStral engine instance (M3 phase A). All mutable engine state
 * (DSP buffers, worker pool, barriers, RT output buffers, freeze/display
 * state, counters) lives in this struct — see luxstral_engine.h.
 * Zero-initialized except use_barriers (enabled by default, matching the
 * former `_Atomic int g_use_barriers = 1;` static initializer).             */
LuxStralEngine g_luxstral_engine_a = {
    .use_barriers = 1,
    .out_L = luxstral_buffers_L,          /* legacy globals — A behaviour unchanged */
    .out_R = luxstral_buffers_R,
    .out_index = &luxstral_buffer_index,
    .source_type_override = -1,           /* use global luxstral_source_type */
};

/* Phase-management activity counter (UI LED) — cumulative onsets. The
 * producer thread adds in the drain block; the LUXSTRAL page timer polls via
 * synth_luxstral_get_phase_onset_total() and lights the LED on deltas.
 * Monotonic, wraps harmlessly. */
static _Atomic uint32_t g_phase_onset_total[1];

uint32_t synth_luxstral_get_phase_onset_total(int engine_idx) {
  if (engine_idx != 0)
    return 0;
  return atomic_load_explicit(&g_phase_onset_total[0],
                              memory_order_relaxed);
}


/* RMS anti-saturation ceiling — the ONE dynamics stage, whatever the decode
 * law (gamma or SCORE dB). Replaces the legacy summation normalization
 * (peak-compensated 1/x^(1/sumExp) curve), which over-attenuated sparse
 * content and left the output too quiet.
 *
 * sumVolumeBuffer accumulates Σ a_i² — the volume weighting exponent is fixed
 * at 2 in synth_luxstral_threading.c — and the output RMS of a bank of
 * orthogonal partials is EXACTLY chain_gain·sqrt(Σ a_i²/2), whatever the
 * phases. chain_gain must be the FULL scale between oscillator amplitudes and
 * output samples (combine-stage safety scale × loop base gain). While the
 * predicted RMS stays under LUXSTRAL_RMS_MAX this returns unity: the chain is
 * strictly linear and dynamics are fully preserved. Above it (dense image,
 * hand-drawn strokes, black frame ⇒ Σa² up to 3456) the output is normalized
 * to a constant loud-but-clean RMS instead of slamming the soft limiter into
 * saturation. The transfer is continuous at the knee; the gain follows the
 * envelope-smoothed volumes, so it moves at millisecond rate (no zipper). */
static inline float rms_ceiling_gain(float energy_sum, float chain_gain) {
  if (energy_sum <= 0.0f)
    return 1.0f;
  const float predicted_rms = chain_gain * sqrtf(0.5f * energy_sum);
  return (predicted_rms > LUXSTRAL_RMS_MAX)
             ? (LUXSTRAL_RMS_MAX / predicted_rms)
             : 1.0f;
}

// Cleanup function to release persistent buffers
static void synth_luxstral_cleanup_impl(LuxStralEngine *eng) {
  if (eng->additiveBuffer)  { free(eng->additiveBuffer);  eng->additiveBuffer = NULL; }
  if (eng->sumVolumeBuffer) { free(eng->sumVolumeBuffer); eng->sumVolumeBuffer = NULL; }
  if (eng->maxVolumeBuffer) { free(eng->maxVolumeBuffer); eng->maxVolumeBuffer = NULL; }
  if (eng->tmp_audioData)   { free(eng->tmp_audioData);   eng->tmp_audioData = NULL; }
  if (eng->stereoBuffer_L)  { free(eng->stereoBuffer_L);  eng->stereoBuffer_L = NULL; }
  if (eng->stereoBuffer_R)  { free(eng->stereoBuffer_R);  eng->stereoBuffer_R = NULL; }
  if (eng->imageRef)        { free(eng->imageRef);        eng->imageRef = NULL; }
}

// Public wrapper (registered via atexit; called by Sp3ctraSharedCore)
void synth_luxstral_cleanup(void) {
  synth_luxstral_cleanup_impl(&g_luxstral_engine_a);
  // The engine only BORROWS the runtime's global waves[] (freed by
  // synth_runtime_free_buffers) — never free it here.
}

/* Public functions ----------------------------------------------------------*/

static int32_t synth_IfftInit_impl(LuxStralEngine *eng) {
  int32_t buffer_len = 0;

  log_info("SYNTH", "---------- SYNTH INIT ---------");
  log_info("SYNTH", "-------------------------------");

  // Initialize runtime configuration
  if (synth_runtime_init(get_cis_pixels_nb(), g_sp3ctra_config.pixels_per_note) != 0) {
    log_error("SYNTH", "Failed to initialize runtime configuration");
    return -1;
  }

  // Allocate dynamic buffers
  if (synth_runtime_allocate_buffers() != 0) {
    log_error("SYNTH", "Failed to allocate dynamic buffers");
    return -1;
  }

  // Set global waves pointer (allocated in synth_runtime_allocate_buffers)
  waves = synth_runtime_get_waves();
  eng->waves = waves;   /* engine A uses the historical global oscillator array */

  // Register cleanup functions
  atexit(synth_runtime_free_buffers);
  atexit(synth_shutdown_thread_pool);
  atexit(synth_luxstral_cleanup);

  // Initialize the shared sine table (4 KB, L1-resident, must run before init_waves)
  init_sine_table();

  // Initialize default parameters (kept for hot-reload compatibility)
  wavesGeneratorParams.commaPerSemitone = g_sp3ctra_config.comma_per_semitone;
  wavesGeneratorParams.startFrequency = (uint32_t)g_sp3ctra_config.start_frequency;
  wavesGeneratorParams.harmonization = MAJOR;
  wavesGeneratorParams.harmonizationLevel = 100;
  wavesGeneratorParams.waveformOrder = 1;

  init_waves(waves, &wavesGeneratorParams);
  buffer_len = 0; /* No waveform buffer needed — shared sine table is global */

  // Precompute GAP_LIMITER envelope coefficients for all oscillators
  update_gap_limiter_coefficients();

  /* Randomize initial phase for each oscillator to avoid constructive interference
   * (all oscillators at phase 0 simultaneously would produce a harsh transient).
   * Range: [0, SINE_TABLE_SIZE) — matches the new shared table indexing.        */
  for (int i = 0; i < get_current_number_of_notes(); i++)
  {
#ifdef __APPLE__
    uint32_t aRandom32bit = arc4random();
#else
    uint32_t aRandom32bit = (uint32_t)rand();
#endif
    waves[i].phase_acc     = (float)(aRandom32bit % (uint32_t)SINE_TABLE_SIZE);
    waves[i].current_volume = 0.0f;
  }

  /* buffer_len is always 0 with the shared sine table — no overflow possible */
  (void)buffer_len;

  log_info("SYNTH", "Note number = %d", (int)get_current_number_of_notes());
  log_info("SYNTH", "Using Float32 path + shared sine table (%d entries, 4 KB)",
           SINE_TABLE_SIZE);

  if (get_current_number_of_notes() > 0)
  {
    log_info("SYNTH", "First note: %.2f Hz, phase_inc=%.5f",
             waves[0].frequency, (double)waves[0].phase_inc);
    log_info("SYNTH", "Last  note: %.2f Hz, phase_inc=%.5f",
             waves[get_current_number_of_notes() - 1].frequency,
             (double)waves[get_current_number_of_notes() - 1].phase_inc);
  }

  log_info("SYNTH", "-------------------------------");

#ifdef PRINT_IFFT_FREQUENCY
  for (uint32_t pix = 0; pix < (uint32_t)get_current_number_of_notes(); pix++)
  {
    printf("FREQ = %0.2f, PHASE_INC = %.5f\n",
           waves[pix].frequency, (double)waves[pix].phase_inc);
  }
  printf("-------------------------------\n");
#endif

  // Allocate imageRef dynamically
  eng->imageRef = (int32_t*)calloc(get_current_number_of_notes(), sizeof(int32_t));
  if (!eng->imageRef) {
    log_error("SYNTH", "Failed to allocate imageRef");
    return -1;
  }
  // REFACTORED: Initialize with 1.0 in micros scale (normalized amplitude)
  // This matches the new preprocessing that stores values as (normalized * 1000000)
  fill_int32(1000000, eng->imageRef, get_current_number_of_notes());

  // Initialize image debug system
  image_debug_init();

  // Initialize the engine synthesis mutex
  if (pthread_mutex_init(&eng->synth_process_mutex, NULL) != 0) {
      perror("Failed to initialize synth process mutex");
      die("synth init failed");
      return -1;
  }

  // (Dead lock_free_pan subsystem removed: its data API had no callers — the
  // pan values flow through db->preprocessed_data.stereo instead.)

  return 0;
}

// Public wrapper (signature unchanged for external callers)
int32_t synth_IfftInit(void) {
  return synth_IfftInit_impl(&g_luxstral_engine_a);
}

/**
 * @brief  Optimized version of the LuxStral synthesis with a persistent thread pool
 * @param  eng Engine instance
 * @param  imageData Grayscale input data
 * @param  audioDataLeft Left channel audio output buffer (stereo mode)
 * @param  audioDataRight Right channel audio output buffer (stereo mode)
 * @param  contrast_factor Contrast factor for volume modulation
 * @retval None
 */
static void synth_IfftMode_impl(LuxStralEngine *eng, float *imageData, float *audioDataLeft, float *audioDataRight, float contrast_factor, DoubleBuffer *db) {

  // LuxStral mode (limited logs)
  if (eng->log_counter % LOG_FREQUENCY == 0) {
    // printf("===== LuxStral Mode called (optimized) =====\n");
  }

  int buff_idx;

  // Persistent dynamically-sized buffers live in the engine struct

  // Initialize thread pool and RT-safe buffers if not initialized
  // This handles both first start AND restart after buffer size change
  if (!eng->pool_initialized) {
    log_info("SYNTH", "Initializing synthesis system (pool_init=%d, shutdown=%d)",
             eng->pool_initialized, eng->pool_shutdown);

    // (init_rt_safe_buffers removed: the "RT-safe double buffering" subsystem
    // had no consumer — it only calloc'd + mlock'd 96 KB per engine on every
    // pool init and never freed them.)
    if (synth_init_thread_pool(eng) == 0) {
      if (synth_start_worker_threads(eng) == 0) {
        log_info("SYNTH", "RT-safe synthesis system initialized successfully");
      } else {
        log_error("SYNTH", "Failed to start worker threads, synthesis will fail");
        eng->pool_initialized = 0;
      }
    } else {
      log_error("SYNTH", "Failed to initialize thread pool, synthesis will fail");
      eng->pool_initialized = 0;
    }
  }

  // Reallocate persistent buffers if buffer size changed
  int bs = g_sp3ctra_config.audio_buffer_size;
  if (bs <= 0) {
    log_error("SYNTH", "Invalid audio buffer size");
    return;
  }

  if (eng->audio_buffer_size != bs) {
    // Free old buffers if size changed
    free(eng->additiveBuffer);  eng->additiveBuffer = NULL;
    free(eng->sumVolumeBuffer); eng->sumVolumeBuffer = NULL;
    free(eng->maxVolumeBuffer); eng->maxVolumeBuffer = NULL;
    free(eng->tmp_audioData);   eng->tmp_audioData = NULL;
    free(eng->stereoBuffer_L);  eng->stereoBuffer_L = NULL;
    free(eng->stereoBuffer_R);  eng->stereoBuffer_R = NULL;

    eng->audio_buffer_size = bs;
  }

  if (!eng->additiveBuffer) {
    eng->additiveBuffer   = (float*)calloc(bs, sizeof(float));
    eng->sumVolumeBuffer  = (float*)calloc(bs, sizeof(float));
    eng->maxVolumeBuffer  = (float*)calloc(bs, sizeof(float));
    eng->tmp_audioData    = (float*)calloc(bs, sizeof(float));
    if (!eng->additiveBuffer || !eng->sumVolumeBuffer || !eng->maxVolumeBuffer || !eng->tmp_audioData) {
      log_error("SYNTH", "Failed to allocate additive persistent buffers");
      return;
    }
  }

  // Debug marker: start of new image (yellow line)
  image_debug_mark_new_image_boundary();

  // Final buffers for combined results
  // buffers allocated at first call above

  // Reset final buffers
  fill_float(0, eng->additiveBuffer, g_sp3ctra_config.audio_buffer_size);
  fill_float(0, eng->sumVolumeBuffer, g_sp3ctra_config.audio_buffer_size);
  fill_float(0, eng->maxVolumeBuffer, g_sp3ctra_config.audio_buffer_size);


  if (eng->pool_initialized && !eng->pool_shutdown) {
    // === OPTIMIZED VERSION WITH THREAD POOL ===

    // HOT-RELOAD CHECK: Process pending frequency reinit BEFORE workers start
    // This is safe because workers are waiting on start_barrier
    // (check_and_process_frequency_reinit() mutates the GLOBAL waves[]).
    check_and_process_frequency_reinit();

    // Phase 1: Pre-compute data in single-thread (avoids contention)
    synth_precompute_wave_data(eng, imageData, db);

    // Phase 2: Start workers in parallel
    // Deterministic execution with barriers
    // Signal all workers to start via barrier
    synth_barrier_wait(eng, &eng->worker_start_barrier);

    // Wait for all workers to complete via barrier
    synth_barrier_wait(eng, &eng->worker_end_barrier);

    /* ── Phase management: auto-gate + diagnostics ────────────────────────
     * Drain the per-worker counters every buffer (producer thread, after
     * the end barrier — never in the RT workers).
     *  1. phase_onset_ref  = slow-decaying max of note volumes (~10 s), the
     *     self-calibrating reference: the user never tunes an absolute
     *     threshold against invisible internal volume scales.
     *  2. phase_gate_abs   = sensitivity_fraction × ref (floored) — read by
     *     the workers NEXT frame (barrier-ordered, no race).
     *  3. onset totals feed the UI activity LED (atomic, polled by the
     *     LUXSTRAL page timer) and a ~5 s log line.                        */
    if (g_sp3ctra_config.luxstral_phase_mode != LUXSTRAL_PHASE_MODE_FREE) {
      const int ei = 0;
      static uint32_t rst_acc[2];
      static uint32_t burst_max[2]; /* peak resets in a SINGLE buffer: the
                                       flanger comb needs COLLECTIVE alignment
                                       (a whole band resetting together);
                                       scattered onsets never build the comb */
      static float    vol_max[2];
      static uint32_t frames[2];
      uint32_t buf_resets  = 0;     /* this buffer's resets                  */
      float    buf_volmax  = 0.0f;  /* this buffer's loudest note target     */
      float    buf_volmin  = 1.0f;  /* this buffer's quietest note target    */
      for (int wi = 0; wi < eng->num_workers; wi++) {
        synth_thread_worker_t *w = &eng->thread_pool[wi];
        buf_resets += w->phase_reset_count;
        w->phase_reset_count = 0;
        if (w->max_target_volume > buf_volmax) buf_volmax = w->max_target_volume;
        w->max_target_volume = 0.0f;
        if (w->min_target_volume < buf_volmin) buf_volmin = w->min_target_volume;
        w->min_target_volume = 1.0f;
      }

      /* Rolling reference (peak) + resting bed (floor) + absolute gate for
       * next frame. Peak decays 0.999/buf ≈ 10 s at 86 buf/s; the bed
       * follows drops instantly and drifts up with ~1 s time constant.
       * The bed is the decode law's floor for EMPTY pixels (10^(-range/20),
       * e.g. 0.0032 @ 50 dB — never 0): the gate must stand clear of it
       * (4×) so the workers' re-arm hysteresis (gate/2 = 2× bed) can see
       * through it. sensitivity 1 → gate at 3 % of recent peak (catches
       * soft onsets), 0 → ~50 % (only the hardest attacks). */
      float ref = eng->phase_onset_ref * 0.999f;
      if (buf_volmax > ref) ref = buf_volmax;
      eng->phase_onset_ref = ref;
      float bed = eng->phase_onset_floor;
      bed += (buf_volmin - bed) * ((buf_volmin < bed) ? 1.0f : 0.01f);
      eng->phase_onset_floor = bed;
      const float sens = g_sp3ctra_config.luxstral_phase_sensitivity;
      const float frac = 0.03f + (1.0f - sens) * (1.0f - sens) * 0.47f;
      float gate = frac * ref;
      if (gate < 4.0f * bed) gate = 4.0f * bed;
      if (gate < 0.001f) gate = 0.001f;
      eng->phase_gate_abs = gate;

      /* UI activity feed: cumulative onset count, polled by the page timer */
      if (buf_resets > 0)
        atomic_fetch_add_explicit(&g_phase_onset_total[ei], buf_resets,
                                  memory_order_relaxed);

      rst_acc[ei] += buf_resets;
      if (buf_resets > burst_max[ei]) burst_max[ei] = buf_resets;
      if (buf_volmax > vol_max[ei]) vol_max[ei] = buf_volmax;
      const int bs = g_sp3ctra_config.audio_buffer_size > 0
                         ? g_sp3ctra_config.audio_buffer_size : 512;
      if (++frames[ei] >= (uint32_t)(5 * g_sp3ctra_config.sampling_frequency / bs)) {
        log_info("SYNTH",
                 "PHASE[%c] mode=%d: %u onsets/5s (peak %u/buf) | ref %.4f bed %.4f gate %.4f | drift +/-%.2f ct",
                 ei ? 'B' : 'A', g_sp3ctra_config.luxstral_phase_mode,
                 rst_acc[ei], burst_max[ei], (double)eng->phase_onset_ref,
                 (double)eng->phase_onset_floor, (double)eng->phase_gate_abs,
                 (double)g_sp3ctra_config.luxstral_phase_drift_cents);
        rst_acc[ei] = 0; burst_max[ei] = 0; vol_max[ei] = 0.0f; frames[ei] = 0;
      }
    }

    // Capture per-sample (per buffer) volumes across all notes to ensure 1 image line = 1 audio sample
  if (image_debug_is_oscillator_capture_enabled()) {
    // Iterate over each sample inside this audio buffer
    for (int s = 0; s < g_sp3ctra_config.audio_buffer_size; s++) {
      // Visit notes in ascending order across workers to keep strict note order
      for (int wi = 0; wi < eng->num_workers; wi++) {
        synth_thread_worker_t *w = &eng->thread_pool[wi];
        // Safety: ensure captured buffers are allocated for this worker
        if (!w->captured_current_volume || !w->captured_target_volume) {
          continue;
        }
        int notes_this = w->end_note - w->start_note;
        if (notes_this <= 0) continue;

        for (int note = w->start_note; note < w->end_note; note++) {
          int local_note_idx = note - w->start_note;
          if (local_note_idx < 0 || local_note_idx >= notes_this) continue;

          size_t base = (size_t)local_note_idx * (size_t)g_sp3ctra_config.audio_buffer_size;
          float cur = w->captured_current_volume[base + (size_t)s];
          float tgt = w->captured_target_volume[base + (size_t)s];
          image_debug_capture_volume_sample_fast(note, cur, tgt);
        }
      }
    }
  }

    // Thread buffers combination completed

    // Float32 version: combine float buffers directly
    for (int i = 0; i < eng->num_workers; i++) {
      synth_thread_worker_t *w = &eng->thread_pool[i];
      if (w->thread_luxstralBuffer) {
        add_float(w->thread_luxstralBuffer, eng->additiveBuffer,
                  eng->additiveBuffer, g_sp3ctra_config.audio_buffer_size);
      }
      if (w->thread_sumVolumeBuffer) {
        add_float(w->thread_sumVolumeBuffer, eng->sumVolumeBuffer,
                  eng->sumVolumeBuffer, g_sp3ctra_config.audio_buffer_size);
      }

      // For maxVolumeBuffer, take the maximum
      if (w->thread_maxVolumeBuffer) {
        for (buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
          if (w->thread_maxVolumeBuffer[buff_idx] >
              eng->maxVolumeBuffer[buff_idx]) {
            eng->maxVolumeBuffer[buff_idx] =
                w->thread_maxVolumeBuffer[buff_idx];
          }
        }
      }
    }

    // SATURATION PREVENTION: Apply pre-scaling to keep headroom before normalization
    // (shared define — the RMS ceiling predictor accounts for this scale)
    const float safety_scale = LUXSTRAL_SUM_SAFETY_SCALE;
    scale_float(eng->additiveBuffer, safety_scale, g_sp3ctra_config.audio_buffer_size);

    // CORRECTION: Conditional normalization by platform
#ifdef __linux__
    // Pi/Linux: Divide by 3 (BossDAC/ALSA amplifies naturally)
    //scale_float(additiveBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
    //scale_float(sumVolumeBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
    //scale_float(maxVolumeBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
#else
    //scale_float(additiveBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
    //scale_float(sumVolumeBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
    //scale_float(maxVolumeBuffer, 1.0f / 3.0f, g_sp3ctra_config.audio_buffer_size);
#endif

  } else {
    // === ERROR: Thread pool not available ===
    log_error("SYNTH", "Thread pool not available");
    // Fill buffers with silence
    fill_float(0, audioDataLeft, g_sp3ctra_config.audio_buffer_size);
    fill_float(0, audioDataRight, g_sp3ctra_config.audio_buffer_size);
    return;
  }

    // Final processing phase

    // Intelligent normalization with exponential response curve (REACTIVATED)
    // ANTI-TAC PROTECTION: Fade-in over first few callbacks to eliminate startup "tac"

    // DISABLED: Anti-tac fade-in temporarily for debugging
    float fade_in_factor = 1.0f;  // Force full volume immediately

    // RMS dynamics (see rms_ceiling_gain). base_gain is applied
    // in the gain loops; the worker-combine stage already pre-scaled the mono
    // AND stereo buffers by LUXSTRAL_SUM_SAFETY_SCALE, so the predictor uses
    // the full chain gain (safety × base) between oscillator amplitudes and
    // output samples.
    const float base_gain  = LUXSTRAL_RMS_BASE_GAIN;
    const float chain_gain = LUXSTRAL_RMS_BASE_GAIN * LUXSTRAL_SUM_SAFETY_SCALE;

    const float soft_limit_threshold = g_sp3ctra_config.soft_limit_threshold;
    const float soft_limit_knee      = g_sp3ctra_config.soft_limit_knee;
    const int   stereo_enabled       = g_sp3ctra_config.stereo_mode_enabled;

    for (buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
        // ONE gain implementation for mono AND stereo: linear below the RMS
        // ceiling, constant-RMS above (sumVolumeBuffer holds Σa²).
        const float gain = base_gain
            * rms_ceiling_gain(eng->sumVolumeBuffer[buff_idx], chain_gain);
        eng->tmp_audioData[buff_idx] =
            eng->additiveBuffer[buff_idx] * gain * fade_in_factor; // anti-tac fade-in
    }

    // SOFT LIMITER: Prevent hard clipping while preserving dynamics (applied AFTER normalization)
    for (buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
        float abs_signal = fabsf(eng->tmp_audioData[buff_idx]);
        if (abs_signal > soft_limit_threshold) {
          // Soft compression using tanh for smooth saturation
          float excess = abs_signal - soft_limit_threshold;
          float compressed = tanhf(excess / soft_limit_knee) * soft_limit_knee;
          eng->tmp_audioData[buff_idx] = copysignf(soft_limit_threshold + compressed, eng->tmp_audioData[buff_idx]);
        }
    }

  // The contrast factor is now passed as parameter from synth_AudioProcess

  // Apply contrast modulation and unified stereo output
  if (eng->pool_initialized && !eng->pool_shutdown) {
    if (stereo_enabled) {
    // STEREO MODE: Use actual stereo buffers from threads
    // Combine stereo buffers from all threads (held in the engine struct)

    // Initialize stereo buffers (allocate once)
    if (!eng->stereoBuffer_L) {
      eng->stereoBuffer_L = (float*)calloc(g_sp3ctra_config.audio_buffer_size, sizeof(float));
      eng->stereoBuffer_R = (float*)calloc(g_sp3ctra_config.audio_buffer_size, sizeof(float));
      if (!eng->stereoBuffer_L || !eng->stereoBuffer_R) {
        log_error("SYNTH", "Failed to allocate stereo buffers");
      }
    }
    fill_float(0, eng->stereoBuffer_L, g_sp3ctra_config.audio_buffer_size);
    fill_float(0, eng->stereoBuffer_R, g_sp3ctra_config.audio_buffer_size);

    // Float32 version: combine float stereo buffers directly
    for (int i = 0; i < eng->num_workers; i++) {
      add_float(eng->thread_pool[i].thread_luxstralBuffer_L, eng->stereoBuffer_L,
                eng->stereoBuffer_L, g_sp3ctra_config.audio_buffer_size);
      add_float(eng->thread_pool[i].thread_luxstralBuffer_R, eng->stereoBuffer_R,
                eng->stereoBuffer_R, g_sp3ctra_config.audio_buffer_size);
    }

    // SATURATION PREVENTION: Apply same safety scaling to stereo buffers
    // (shared define — same as mono; the RMS predictor accounts for it)
    const float safety_scale_stereo = LUXSTRAL_SUM_SAFETY_SCALE;
    scale_float(eng->stereoBuffer_L, safety_scale_stereo, g_sp3ctra_config.audio_buffer_size);
    scale_float(eng->stereoBuffer_R, safety_scale_stereo, g_sp3ctra_config.audio_buffer_size);

    // DEBUG: Check if stereo buffers have data (disabled for production)
    // static int stereo_dbg_cnt = 0;
    // if ((stereo_dbg_cnt++ % 500) == 0) {
    //   float sum_stereo_l = 0.0f, sum_stereo_r = 0.0f;
    //   float sum_vol = 0.0f;
    //   for (int dd = 0; dd < g_sp3ctra_config.audio_buffer_size; dd++) {
    //     sum_stereo_l += fabsf(stereoBuffer_L[dd]);
    //     sum_stereo_r += fabsf(stereoBuffer_R[dd]);
    //     sum_vol += sumVolumeBuffer[dd];
    //   }
    //   log_info("SYNTH_DBG", "stereoL_sum=%.6f stereoR_sum=%.6f sumVol=%.6f", sum_stereo_l, sum_stereo_r, sum_vol);
    // }
    
    // Apply final processing and contrast
    // Pre-limit clipping telemetry (once per second, low overhead)
    float peakPreL = 0.0f, peakPreR = 0.0f;

    for (buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
      float left_signal, right_signal;

      {
        // SHARED RMS gain — identical to the mono loop (mono and stereo are
        // pre-scaled by the same safety scale). One gain for L and R:
        // constant-power panning splits the same total energy, and a shared
        // gain preserves the stereo image.
        const float gain = base_gain
            * rms_ceiling_gain(eng->sumVolumeBuffer[buff_idx], chain_gain);
        left_signal  = eng->stereoBuffer_L[buff_idx] * gain * fade_in_factor;
        right_signal = eng->stereoBuffer_R[buff_idx] * gain * fade_in_factor;
      }

      // Track pre-limit peaks
      float aL = fabsf(left_signal), aR = fabsf(right_signal);
      if (aL > peakPreL) peakPreL = aL;
      if (aR > peakPreR) peakPreR = aR;

      // Apply contrast factor AND global fade for smooth transitions
      float global_fade = get_global_fade_factor_and_update();
      audioDataLeft[buff_idx] = left_signal * contrast_factor * global_fade;
      audioDataRight[buff_idx] = right_signal * contrast_factor * global_fade;

      // Apply final hard limiting
      if (audioDataLeft[buff_idx] > 1.0f) audioDataLeft[buff_idx] = 1.0f;
      if (audioDataLeft[buff_idx] < -1.0f) audioDataLeft[buff_idx] = -1.0f;
      if (audioDataRight[buff_idx] > 1.0f) audioDataRight[buff_idx] = 1.0f;
      if (audioDataRight[buff_idx] < -1.0f) audioDataRight[buff_idx] = -1.0f;
    }

    // DEBUG: Check final output values (disabled for production)
    // static int final_dbg_cnt = 0;
    // if ((final_dbg_cnt++ % 500) == 0) {
    //   float out_sum_l = 0.0f, out_sum_r = 0.0f;
    //   for (int dd = 0; dd < g_sp3ctra_config.audio_buffer_size; dd++) {
    //     out_sum_l += fabsf(audioDataLeft[dd]);
    //     out_sum_r += fabsf(audioDataRight[dd]);
    //   }
    //   log_info("SYNTH_DBG", "FINAL audioL_sum=%.6f audioR_sum=%.6f peakPreL=%.6f peakPreR=%.6f fade=%.2f", 
    //            out_sum_l, out_sum_r, peakPreL, peakPreR, fade_in_factor);
    // }

    // Clipping telemetry disabled in production
    // if (log_counter % LOG_FREQUENCY == 0) {
    //   if (preClipCount > 0) {
    //     printf("[CLIP] pre=%d/%d (%.1f%%) peakPreL=%.3f peakPreR=%.3f\n",
    //            preClipCount, AUDIO_BUFFER_SIZE,
    //            (preClipCount * 100.0f) / (float)AUDIO_BUFFER_SIZE,
    //            peakPreL, peakPreR);
    //   } else {
    //     printf("[CLIP] pre=0 peakPreL=%.3f peakPreR=%.3f\n", peakPreL, peakPreR);
    //   }
    // }
    } else {
      // MONO MODE: Use original simple processing and duplicate output
      float peakPre = 0.0f;

      for (buff_idx = 0; buff_idx < g_sp3ctra_config.audio_buffer_size; buff_idx++) {
        float mono_pre = eng->tmp_audioData[buff_idx];
        float a = fabsf(mono_pre);
        if (a > peakPre) peakPre = a;

        // Apply contrast factor AND global fade for smooth transitions
        float global_fade = get_global_fade_factor_and_update();
        float mono_sample = mono_pre * contrast_factor * global_fade;

        // Duplicate mono sample to both channels
        audioDataLeft[buff_idx] = mono_sample;
        audioDataRight[buff_idx] = mono_sample;

        // Apply final hard limiting
        if (audioDataLeft[buff_idx] > 1.0f) audioDataLeft[buff_idx] = 1.0f;
        if (audioDataLeft[buff_idx] < -1.0f) audioDataLeft[buff_idx] = -1.0f;
        if (audioDataRight[buff_idx] > 1.0f) audioDataRight[buff_idx] = 1.0f;
        if (audioDataRight[buff_idx] < -1.0f) audioDataRight[buff_idx] = -1.0f;
      }

      // Clipping telemetry disabled in production (mono mode)
      // if (log_counter % LOG_FREQUENCY == 0) {
      //   if (preClipCount > 0) {
      //     printf("[CLIP] pre=%d/%d (%.1f%%) peakPre=%.3f (mono)\n",
      //            preClipCount, g_sp3ctra_config.audio_buffer_size,
      //            (preClipCount * 100.0f) / (float)g_sp3ctra_config.audio_buffer_size,
      //            peakPre);
      //   } else {
      //     printf("[CLIP] pre=0 peakPre=%.3f (mono)\n", peakPre);
      //   }
      // }
    }
  } else {
    // Error case: fill with silence
    fill_float(0, audioDataLeft, g_sp3ctra_config.audio_buffer_size);
    fill_float(0, audioDataRight, g_sp3ctra_config.audio_buffer_size);
  }

  // Increment global counter for log frequency limitation
  eng->log_counter++;
  // (write-only shared_var.synth_process_cnt counter removed — never read)
}

// Public wrapper (signature unchanged for external callers)
void synth_IfftMode(float *imageData, float *audioDataLeft, float *audioDataRight, float contrast_factor, DoubleBuffer *db) {
  synth_IfftMode_impl(&g_luxstral_engine_a, imageData, audioDataLeft, audioDataRight, contrast_factor, db);
}

// Synth process function
static void synth_AudioProcess_impl(LuxStralEngine *eng, uint8_t *buffer_R, uint8_t *buffer_G,
                                    uint8_t *buffer_B, DoubleBuffer *db, int commit_now) {
  // Audio processing (limited logs)
  if (eng->log_counter % LOG_FREQUENCY == 0) {
    // printf("===== Audio Process called =====\n"); // Removed or commented
  }


  // Check that input buffers are not NULL
  if (!buffer_R || !buffer_G || !buffer_B) {
    log_error("SYNTH", "One of the input buffers is NULL");
    return;
  }
  /* De-globalised output target: the global luxstral_buffers. Cast here to
   * keep the heavy vst_adapters header out of the widely-included engine
   * struct. */
  AudioImageBuffer *obL   = (AudioImageBuffer*)eng->out_L;
  AudioImageBuffer *obR   = (AudioImageBuffer*)eng->out_R;
  volatile int     *obIdx = eng->out_index;
  int index = __atomic_load_n(obIdx, __ATOMIC_RELAXED);
  int nb_pixels = get_cis_pixels_nb();
  // Grayscale staging buffers live in the engine struct (allocated on first call)

  // Allocate buffers on first call
  if (!eng->grayScale_live) {
    eng->grayScale_live = (float *)malloc(nb_pixels * sizeof(float));
    eng->processed_grayScale = (float *)malloc(nb_pixels * sizeof(float));
    if (!eng->grayScale_live || !eng->processed_grayScale) {
      log_error("SYNTH", "Failed to allocate grayscale buffers");
      return;
    }
  }

  // LOCK-FREE DOUBLE BUFFERING with proper alternation:
  // Use the OTHER buffer if current one is still being read by processBlock.
  // This prevents overwriting data that hasn't been consumed yet.
  if (__atomic_load_n(&obL[index].ready, __ATOMIC_ACQUIRE) != 0 ||
      __atomic_load_n(&obR[index].ready, __ATOMIC_ACQUIRE) != 0) {
    // Current buffer still in use - try the other one
    int alt_index = 1 - index;
    if (__atomic_load_n(&obL[alt_index].ready, __ATOMIC_ACQUIRE) == 0 &&
        __atomic_load_n(&obR[alt_index].ready, __ATOMIC_ACQUIRE) == 0) {
      // Other buffer is free, use it
      index = alt_index;
    }
    // If both buffers are in use, we'll overwrite current one (glitch, but no deadlock)
  }

  // 🎯 REMOVED: Color temperature calculation - now done in preprocessing (image_preprocessor.c)
  // The stereo pan positions and gains are already calculated and stored in preprocessed data
  // TODO: Use db->preprocessed_active.stereo.pan_positions[] and gains[] when implementing preprocessed data usage

  // Use preprocessed data when available; fallback to local preprocessing
  float contrast_factor = 0.0f;
  int has_preprocessed = 0;

  /* SRC-GATE diagnostic snapshot — captured under db->mutex, logged after
   * unlock (never log while holding a mutex shared with the UDP thread). */
  int      _diag_print = 0, _diag_src = -1, _diag_tag = -1;
  float    _diag_gray_sum = 0.0f, _diag_notes_sum = 0.0f, _diag_cf = 0.0f;
  uint64_t _diag_ts = 0;

  pthread_mutex_lock(&db->mutex);
  has_preprocessed = (db->dataReady != 0) && (db->preprocessed_data.timestamp_us != 0);
#ifdef VST_MODE
  {
    /* Source-tag gating: dataReady=1 means live, dataReady=2 means sampler.
     * Reject preprocessed data that came from the wrong source.
     *   Source=S (0): accept only tag 2 (sampler)
     *   Source=L (1): accept only tag 1 (live)
     *   Source=M (2): accept either tag */
    int src = (eng->source_type_override >= 0)
                ? eng->source_type_override
                : g_sp3ctra_config.luxstral_source_type;
    int tag = db->dataReady;

    /* Diagnostic: print source routing state every ~500 synth calls (~0.5s).
     * Values are captured under db->mutex; the log_info itself runs AFTER the
     * unlock below — logging (logger mutex + fprintf + fflush) while holding
     * db->mutex stalled the UDP thread and the FramePlayerThread on every
     * slow stderr flush (periodic crackle synced to the log cadence). */
    _diag_print = ((eng->diag_ctr++ % 500) == 0);

    if (has_preprocessed) {
      if ((src == 0 && tag != 2) || (src == 1 && tag != 1)) {
        has_preprocessed = 0;
      }
    }

    if (_diag_print) {
      if (has_preprocessed) {
        for (int _d = 0; _d < nb_pixels && _d < 3456; _d++)
          _diag_gray_sum += db->preprocessed_data.additive.grayscale[_d];
        for (int _d = 0; _d < 3456; _d++)
          _diag_notes_sum += db->preprocessed_data.additive.notes[_d];
        _diag_cf = db->preprocessed_data.additive.contrast_factor;
      }
      _diag_src = src;
      _diag_tag = tag;
      _diag_ts  = db->preprocessed_data.timestamp_us;
    }
  }
#endif
  if (has_preprocessed) {
    memcpy(eng->grayScale_live, db->preprocessed_data.additive.grayscale,
           nb_pixels * sizeof(float));
    contrast_factor = db->preprocessed_data.additive.contrast_factor;
  }
  pthread_mutex_unlock(&db->mutex);

#ifdef VST_MODE
  if (_diag_print)
    log_info("SRC-GATE", "src=%d tag=%d has_pre=%d cf=%.4f gray_sum=%.2f notes_sum=%.2f ts=%llu",
             _diag_src, _diag_tag, has_preprocessed, _diag_cf,
             _diag_gray_sum, _diag_notes_sum, (unsigned long long)_diag_ts);
#endif

  if (!has_preprocessed) {
#ifdef VST_MODE
    /* Source-aware fallback: buffer_R/G/B comes from AudioImageBuffers which
     * always contains LIVE data.  Only use this fallback when the selected
     * source includes live (L or M).  For Source=S, produce silence and wait
     * for FramePlayerThread to provide sampler preprocessed data. */
    int fallback_src = (eng->source_type_override >= 0)
                         ? eng->source_type_override
                         : g_sp3ctra_config.luxstral_source_type;
    if (fallback_src == 0 /* IMAGE_SOURCE_SAMPLER */) {
      /* Source=S: silence until sampler data arrives */
      memset(eng->grayScale_live, 0, nb_pixels * sizeof(float));
      contrast_factor = 0.0f;
    } else
#endif
    {
      PreprocessedImageData preprocessed_temp;
      PipelineConfig fallback_cfg = pipeline_build_config_live();
      if (pipeline_process_frame(buffer_R, buffer_G, buffer_B, &fallback_cfg, &preprocessed_temp) == 0) {
        memcpy(eng->grayScale_live, preprocessed_temp.additive.grayscale,
               nb_pixels * sizeof(float));
        contrast_factor = preprocessed_temp.additive.contrast_factor;

        pthread_mutex_lock(&db->mutex);
        db->preprocessed_data = preprocessed_temp;
        db->dataReady = 1;
        pthread_mutex_unlock(&db->mutex);
      } else {
        memset(eng->grayScale_live, 0, nb_pixels * sizeof(float));
        contrast_factor = 0.0f;
      }
    }
  }

  // Capture raw scanner line for debug visualization
  image_debug_capture_raw_scanner_line(buffer_R, buffer_G, buffer_B);

  // --- Synth Data Freeze/Fade Logic ---
  pthread_mutex_lock(&eng->synth_data_freeze_mutex);
  int local_is_frozen = eng->is_synth_data_frozen;
  int local_is_fading = eng->is_synth_data_fading_out;

  if (local_is_frozen && !eng->prev_frozen_state && !local_is_fading) {
    memcpy(eng->frozen_grayscale_buffer, eng->grayScale_live,
           nb_pixels * sizeof(float));
  }
  eng->prev_frozen_state = local_is_frozen;

  if (local_is_fading && !eng->prev_fading_state) {
    eng->synth_data_fade_start_time = synth_getCurrentTimeInSeconds();
  }
  eng->prev_fading_state = local_is_fading;
  pthread_mutex_unlock(&eng->synth_data_freeze_mutex);

  float alpha_blend = 1.0f; // For cross-fade

  if (local_is_fading) {
    double elapsed_time =
        synth_getCurrentTimeInSeconds() - eng->synth_data_fade_start_time;
    if (elapsed_time >= G_SYNTH_DATA_FADE_DURATION_SECONDS) {
      pthread_mutex_lock(&eng->synth_data_freeze_mutex);
      eng->is_synth_data_fading_out = 0;
      eng->is_synth_data_frozen = 0;
      pthread_mutex_unlock(&eng->synth_data_freeze_mutex);
      memcpy(eng->processed_grayScale, eng->grayScale_live,
             nb_pixels * sizeof(float)); // Use live data
    } else {
      alpha_blend =
          (float)(elapsed_time /
                  G_SYNTH_DATA_FADE_DURATION_SECONDS); // Alpha from 0
                                                       // (frozen) to 1 (live)
      alpha_blend = (alpha_blend < 0.0f)
                        ? 0.0f
                        : ((alpha_blend > 1.0f) ? 1.0f : alpha_blend);
      for (int i = 0; i < nb_pixels; ++i) {
        eng->processed_grayScale[i] =
            eng->frozen_grayscale_buffer[i] * (1.0f - alpha_blend) +
            eng->grayScale_live[i] * alpha_blend;
      }
    }
  } else if (local_is_frozen) {
    memcpy(eng->processed_grayScale, eng->frozen_grayscale_buffer,
           nb_pixels * sizeof(float)); // Use frozen data
  } else {
    memcpy(eng->processed_grayScale, eng->grayScale_live,
           nb_pixels * sizeof(float)); // Use live data
  }
  // --- End Synth Data Freeze/Fade Logic ---

  // Store contrast factor atomically for auto-volume system (using memcpy for float)
  // Note: Single float write is atomic on most platforms, but we use explicit atomic for clarity
  eng->last_contrast_factor = contrast_factor;

  // Launch synthesis with potentially frozen/faded data
  // Unified mode: always pass both left and right buffers
  synth_IfftMode_impl(eng,
                 eng->processed_grayScale,
                 obL[index].data,
                 obR[index].data,
                 contrast_factor,
                 db);

  // Diagnostics (non-RT thread): log signal statistics occasionally (disabled for production)
  // static uint32_t diag_counter = 0;
  // if ((diag_counter++ % 500) == 0) {
  //   float sum_l = 0.0f;
  //   float sum_r = 0.0f;
  //   float max_l = 0.0f;
  //   float max_r = 0.0f;
  //   int bs = g_sp3ctra_config.audio_buffer_size;

  //   for (int i = 0; i < bs; ++i) {
  //     float lv = buffers_L[index].data[i];
  //     float rv = buffers_R[index].data[i];
  //     sum_l += lv * lv;
  //     sum_r += rv * rv;
  //     float al = fabsf(lv);
  //     float ar = fabsf(rv);
  //     if (al > max_l) max_l = al;
  //     if (ar > max_r) max_r = ar;
  //   }

  //   float rms_l = (bs > 0) ? sqrtf(sum_l / (float)bs) : 0.0f;
  //   float rms_r = (bs > 0) ? sqrtf(sum_r / (float)bs) : 0.0f;

  //   float gray_sum = 0.0f;
  //   for (int i = 0; i < nb_pixels; ++i) {
  //     gray_sum += g_grayScale_live[i];
  //   }
  //   float gray_avg = (nb_pixels > 0) ? (gray_sum / (float)nb_pixels) : 0.0f;

  //   log_info("SYNTH", "Diagnostics: readyL=%d readyR=%d contrast=%.6f gray_avg=%.6f rmsL=%.6f rmsR=%.6f peakL=%.6f peakR=%.6f",
  //            buffers_L[index].ready,
  //            buffers_R[index].ready,
  //            contrast_factor,
  //            gray_avg,
  //            rms_l,
  //            rms_r,
  //            max_l,
  //            max_r);
  // }

  // NOTE: g_displayable_synth_R/G/B buffers are now updated in multithreading.c
  // with the MIXED RGB colors from the sequencer (not grayscale conversion)
  // LuxStral synthesis finished

  // RT-SAFE: Record timestamp and mark buffers as ready using atomic stores (no mutex needed)
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t timestamp_us = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
  
  obL[index].write_timestamp_us = timestamp_us;
  obR[index].write_timestamp_us = timestamp_us;

  __atomic_store_n(&obL[index].ready, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&obR[index].ready, 1, __ATOMIC_RELEASE);
  // pthread_cond_signal removed - RT callback polls atomically

  // Record the written slot, then publish the flip.
  eng->last_write_index = index;
  if (commit_now)
    __atomic_store_n(obIdx, 1 - index, __ATOMIC_RELEASE);
}

// Public wrapper (signature unchanged for external callers, e.g. multithreading.c)
void synth_AudioProcess(uint8_t *buffer_R, uint8_t *buffer_G,
                        uint8_t *buffer_B, DoubleBuffer *db) {
  synth_AudioProcess_impl(&g_luxstral_engine_a, buffer_R, buffer_G, buffer_B, db, /*commit_now*/1);
}

/**
 * @brief Get the last calculated contrast factor (thread-safe)
 * @return Last contrast factor value (0.0-1.0 range typically)
 * @note Used by auto-volume system to detect audio intensity for adaptive thresholding
 */
float synth_get_last_contrast_factor(void) {
  return g_luxstral_engine_a.last_contrast_factor;
}
