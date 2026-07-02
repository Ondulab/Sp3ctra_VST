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

  /* ===== Per-oscillator state (M8 — dual-engine) ============================ */
  /* Per-engine oscillator array. Holds MUTABLE runtime state (phase_acc,
   * current_volume, target_volume) advanced every frame — it MUST be private to
   * each engine or two engines corrupt each other's phase/envelope (robotic
   * artefacts). Engine A points at the historical global waves[]; engine B owns
   * its own copy (same static timbre: frequency/phase_inc/coeffs). See
   * synth_luxstral_init_engine_b(). Workers read it via worker->engine->waves.  */
  volatile struct wave *waves;

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

  /* ===== Output publish target (de-globalised, M8 — dual engine A/B) ======= */
  /* Where synth_AudioProcess publishes its final stereo result. Engine A points
   * at the historical globals (luxstral_buffers_L/R + luxstral_buffer_index);
   * engine B points at its own second set. Kept as opaque pointers so the heavy
   * vst_adapters header stays out of this widely-included struct - cast to
   * AudioImageBuffer (and volatile int) in synth_luxstral.c.                    */
  void         *out_L;       /* AudioImageBuffer[2] */
  void         *out_R;       /* AudioImageBuffer[2] */
  volatile int *out_index;   /* publish double-buffer index */
  /* Buffer slot the last frame was written to. When synth_AudioProcess_impl is
   * called with commit_now = 0 (dual-engine A+B), it records the slot here and
   * defers the index flip so the caller can publish A and B with two ADJACENT
   * atomic stores — eliminating the A-published/B-not window that otherwise
   * duplicates/skips whole B frames (robotic artefact). */
  int           last_write_index;
  /* Source-type gating override: -1 = use the global luxstral_source_type
   * (engine A, exact legacy behaviour); otherwise a fixed value (engine B reads
   * its OWN DoubleBuffer which it fully controls, so it accepts either → 2).   */
  int           source_type_override;
  /* StrokeForge waveform-morph snapshot for THIS engine's current frame (M8).
   * Copied from the engine's DoubleBuffer in synth_precompute_wave_data();
   * workers read it instead of the global g_waveform_morph (which holds the
   * LAST pipeline call's frame -> cross-talk between engines A and B).         */
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
  /* One-time "Float32 path active" log flag (worker threads)                 */
  _Atomic int f32_path_logged;

} LuxStralEngine;

/* Exported variables --------------------------------------------------------*/

/* Engine A (M3 phase A). Shares the read-only waves[]/sine-table/runtime-config
 * with engine B — see synth_luxstral_init_engine_b().                         */
extern LuxStralEngine g_luxstral_engine_a;

/* Engine B (M8 — dual-engine). Independent DSP buffers, worker pool and output
 * target; reads its OWN chain's input from a second DoubleBuffer. It shares the
 * read-only waves[]/config with A (Option A: independent input, shared timbre).*/
extern LuxStralEngine g_luxstral_engine_b;

/* Per-instance init for engine B: allocates imageRef + inits the synth mutex
 * ONLY. The global waves[]/sine-table/runtime-config were already set up once
 * by synth_IfftInit() for engine A and must not be re-run. The worker pool, RT
 * output buffers and grayscale staging self-initialise lazily on first render.
 * Safe to call once, after synth_IfftInit(). Returns 0 on success.            */
int32_t synth_luxstral_init_engine_b(void);

/* M8 — recompute engine B's envelope coefficients from its OWN Attack/Release
 * params (luxstral_b_tau_*). Safe no-op before engine B is initialised.      */
void synth_luxstral_update_engine_b_envelope(void);

/* M8 — after a frequency hot-reload regenerated the global waves[] (engine A),
 * re-copy the shared static timbre into engine B's private array (preserving
 * B's dynamic state) and re-derive B's envelope coefficients.                 */
void synth_luxstral_resync_engine_b_timbre(void);

/* Render one frame on engine B, publishing to its own output buffers. Mirrors
 * synth_AudioProcess() but for g_luxstral_engine_b + its own DoubleBuffer.     */
void synth_AudioProcess_b(uint8_t *buffer_R, uint8_t *buffer_G,
                          uint8_t *buffer_B, struct DoubleBuffer *db);

/* Render BOTH engines and publish them atomically (adjacent index flips) — use
 * this instead of separate synth_AudioProcess()/_b() calls whenever engine B is
 * active, so the consumer never sees A's new frame paired with B's stale one. */
void synth_AudioProcess_ab(uint8_t *buffer_R, uint8_t *buffer_G, uint8_t *buffer_B,
                           struct DoubleBuffer *db_a, struct DoubleBuffer *db_b);

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
