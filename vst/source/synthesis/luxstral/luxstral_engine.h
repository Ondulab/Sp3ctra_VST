/*
 * luxstral_engine.h
 *
 * LuxStral engine instance state (M3 phase A — de-globalization)
 *
 * Groups ALL mutable state of the additive synthesis engine into a single
 * struct. Internal luxstral functions receive a LuxStralEngine* and operate
 * on its fields; the historical public entry points (synth_AudioProcess,
 * synth_IfftMode, init/shutdown/freeze/display APIs) keep their signatures
 * and forward to the single instance g_luxstral_engine.
 *
 * NOTE for external consumers (outside synthesis/luxstral/):
 *   Do NOT access struct fields directly — use the accessor functions
 *   declared at the bottom of this header (display buffers, freeze state).
 *
 * The sine/square tables (wave_generation.c) are READ-ONLY after init
 * and deliberately stay global.
 *
 * Author: zhonx
 */

#ifndef __LUXSTRAL_ENGINE_H__
#define __LUXSTRAL_ENGINE_H__

/* Includes ------------------------------------------------------------------*/
#include "synth_luxstral_threading.h"  /* synth_thread_worker_t, barrier_t */
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wave;   /* per-oscillator state (defined in wave_generation.h) */

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

  /* ===== Per-oscillator state ============================================== */
  /* Per-engine oscillator array. Holds MUTABLE runtime state (phase_acc,
   * current_volume, target_volume) advanced every frame. Points at the
   * historical global waves[]. Workers read it via worker->engine->waves.      */
  volatile struct wave *waves;

  /* ===== Phase management (auto-calibrated onset gate) ====================== */
  /* Slow-decaying max of per-note target volumes — the reference the onset
   * gate adapts to, so the user never tunes an absolute threshold against
   * invisible internal volume scales. Updated by the producer in the drain
   * block AFTER the end barrier; workers read phase_gate_abs the NEXT frame,
   * strictly ordered by the barriers (no race).                              */
  float phase_onset_ref;   /* rolling max note volume (decay ~10 s)          */
  float phase_onset_floor; /* rolling min note volume = the decode law's
                              resting bed (empty pixels decode to
                              10^(-range/20), never 0) — fast-follow down,
                              slow drift up                                  */
  float phase_gate_abs;    /* gate = max(sens_frac × ref, 4 × bed, 1e-3);
                              workers re-arm at gate/2 (≥ 2× bed, so the
                              hysteresis sees through the bed)              */

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

  /* ===== Output publish target (de-globalised) ============================= */
  /* Where synth_AudioProcess publishes its final stereo result — the historical
   * globals (luxstral_buffers_L/R + luxstral_buffer_index). Kept as opaque
   * pointers so the heavy vst_adapters header stays out of this widely-included
   * struct - cast to AudioImageBuffer (and volatile int) in synth_luxstral.c.   */
  void         *out_L;       /* AudioImageBuffer[2] */
  void         *out_R;       /* AudioImageBuffer[2] */
  volatile int *out_index;   /* publish double-buffer index */
  /* Buffer slot the last frame was written to (deferred index flip support in
   * synth_AudioProcess_impl). */
  int           last_write_index;
  /* StrokeForge waveform-morph snapshot for THIS engine's current frame.
   * Copied from the engine's DoubleBuffer in synth_precompute_wave_data().     */
  float         sf_morph;

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
  /* Last logged SRC-GATE state signature (edge-triggered diagnostic)         */
  int diag_last_sig;
  /* One-time "Float32 path active" log flag (worker threads)                 */
  _Atomic int f32_path_logged;

} LuxStralEngine;

/* Exported variables --------------------------------------------------------*/

/* The LuxStral engine (single instance since the P3 mix-pull migration). */
extern LuxStralEngine g_luxstral_engine;

/* Public accessors for external consumers ------------------------------------
 * These wrap the display buffers of g_luxstral_engine so external files
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
