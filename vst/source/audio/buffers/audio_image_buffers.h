#ifndef AUDIO_IMAGE_BUFFERS_H
#define AUDIO_IMAGE_BUFFERS_H

#include "config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/**
 * @brief Dual buffer system for audio-image data with atomic rotation
 *
 * This system provides continuous access to complete image lines for LuxStral
 * synthesis while allowing UDP thread to write new data without blocking audio
 * processing.
 *
 * Key features:
 * - Separate R, G, B memory areas for stereo effects
 * - Atomic buffer rotation (no mutex on read side)
 * - Real-time audio processing guaranteed
 * - Graceful handling of scanner disconnection
 */
typedef struct AudioImageBuffers {
  // Buffer 0 - RGB channels separated for memory contiguity
  uint8_t *buffer0_R;
  uint8_t *buffer0_G;
  uint8_t *buffer0_B;

  // Buffer 1 - RGB channels separated for memory contiguity
  uint8_t *buffer1_R;
  uint8_t *buffer1_G;
  uint8_t *buffer1_B;

  // Atomic buffer selection (0 or 1)
  // - read_buffer_index: which buffer LuxStral synth should read from
  // - write_buffer_index: which buffer UDP should write to
  atomic_int read_buffer_index;
  atomic_int write_buffer_index;

  // Write protection mutex (only for UDP thread)
  pthread_mutex_t write_mutex;

  // ── Raw UDP snapshot (written only by UDP thread, never by sampler) ──────
  // These hold the last pure UDP frame before any sampler mixing.
  uint8_t *raw_R;
  uint8_t *raw_G;
  uint8_t *raw_B;

  // ── Sampler snapshot (written only by FramePlayerThread) ─────────────────
  // These hold the last pure sampler frame before any live mixing.
  uint8_t *sampler_R;
  uint8_t *sampler_G;
  uint8_t *sampler_B;

  // ── Selection tap (contextual visualizer) ─────────────────────────────────
  // Holds the stream frame AT THE SELECTED MODULE'S POSITION in ITS chain,
  // published by whichever chain executor hosts the selection (plan-driven:
  // SynthChainPlan.viz_tap_insert). Written by the executor thread (udpThread
  // or feeder tick), read lock-free by the head visualizer (SELECTED_TAP).
  uint8_t *selection_tap_R;
  uint8_t *selection_tap_G;
  uint8_t *selection_tap_B;

  // ── Per-engine input taps (per-chain display, 2026-07-10) ─────────────────
  // engine_tap[e] holds the EXACT RGB frame engine e's pipeline consumed on
  // its last committed cycle — published by whichever thread owned that
  // engine's preprocessed commit (udpThread / feeder tick / FramePlayerThread)
  // under the same source-routing arbitration. White = engine unfed (chain
  // with no signal, module absent, playback silence). The head panels
  // (SPCTR_* / SYNTH_*) and the video waterfall read these instead of the
  // legacy luxstral/luxsynth_source_type → RAW/SAMPLER/MODULATED switch, so
  // the display follows each engine's OWN chain.
#define AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A 0
#define AUDIO_IMAGE_ENGINE_TAP_PATHB      1  /* LuxSynth + LuxWave shared input */
#define AUDIO_IMAGE_NUM_ENGINE_TAPS       2
  uint8_t *engine_tap_R[AUDIO_IMAGE_NUM_ENGINE_TAPS];
  uint8_t *engine_tap_G[AUDIO_IMAGE_NUM_ENGINE_TAPS];
  uint8_t *engine_tap_B[AUDIO_IMAGE_NUM_ENGINE_TAPS];
  // Statistics and monitoring
  uint64_t lines_received;
  uint64_t lines_processed;
  uint64_t buffer_swaps;

  // Frame-freshness counter (P4-M3): bumped on EVERY engine-input tap
  // publish (audio_image_buffers_publish_engine_input) — the taps are what
  // the waterfall renders, so their publishes ARE the "new frame" signal,
  // whichever thread produced them (udpThread / feeder / FramePlayerThread).
  // Replaces the dead modulated-bus counter; consumers atomic-load it.
  uint64_t frame_seq;

  // ── Acquisition gate (frame-advance brake / "vitesse d'acquisition") ───────
  // Throttles the LIVE UDP publish path: when enabled, a freshly assembled line
  // is swapped into the read buffer only if a permit is pending — otherwise the
  // line is dropped and the read buffer (hence every consumer: synth + visual)
  // holds the last published frame.  Sample-and-hold at the gate clock rate.
  //   gate_enabled / gate_permit : produced by the audio thread (AcquisitionGate
  //                                clock in processBlock), consumed by the UDP
  //                                thread at the live publish site.
  //   gate_holds                 : diagnostic count of lines dropped while gated.
  // Default disabled → full-rate, byte-for-byte legacy behaviour.
  atomic_int gate_enabled;
  atomic_int gate_permit;
  uint64_t   gate_holds;

  // Initialization flag
  uint8_t initialized;

} AudioImageBuffers;

// Function prototypes
int audio_image_buffers_init(AudioImageBuffers *buffers);
void audio_image_buffers_cleanup(AudioImageBuffers *buffers);

// UDP thread functions (with write protection)
int audio_image_buffers_start_write(AudioImageBuffers *buffers, uint8_t **out_R,
                                    uint8_t **out_G, uint8_t **out_B);
void audio_image_buffers_complete_write(AudioImageBuffers *buffers);

// ── Acquisition gate (frame-advance brake) ─────────────────────────────────
// Enable/disable the gate (audio thread).  Disabled ⇒ should_publish() always
// returns 1 (legacy full-rate behaviour).
void audio_image_buffers_gate_set_enabled(AudioImageBuffers *buffers, int enabled);
// Grant exactly one frame advance (audio thread, on each gate clock tick).
void audio_image_buffers_gate_grant(AudioImageBuffers *buffers);
// Decide whether the live publish path may swap now (UDP thread).  Returns 1 if
// the gate is disabled, or consumes one pending permit (1 once per grant); 0 to
// hold.  Permits never accumulate (max 1) — the gate only brakes, never
// fast-forwards beyond the real acquisition rate.
int  audio_image_buffers_gate_should_publish(AudioImageBuffers *buffers);

// LuxStral synth thread functions (lock-free read)
void audio_image_buffers_get_read_pointers(AudioImageBuffers *buffers,
                                           uint8_t **out_R, uint8_t **out_G,
                                           uint8_t **out_B);

// ── Raw UDP snapshot (written only by UDP thread, never by sampler) ────────
// DEPRECATED: snapshot_raw() reads from the read buffer AFTER complete_write(),
// which is racy — FramePlayerThread may swap the buffer between complete_write()
// and snapshot_raw(), writing sampler data into raw_R/G/B.
// Use snapshot_raw_before_swap() instead (see below).
void audio_image_buffers_snapshot_raw(AudioImageBuffers *buffers);

// FIX(routing): Snapshot raw from the WRITE buffer, BEFORE complete_write().
// Must be called while write_mutex is still held (between start_write and
// complete_write).  This guarantees raw_R/G/B always contains pure UDP data —
// immune to FramePlayerThread's buffer swaps.
void audio_image_buffers_snapshot_raw_before_swap(AudioImageBuffers *buffers);

// FIX(raw): Snapshot raw from external data (no write_mutex required).
// Used when the AudioImageBuffers write bus was not started (e.g. during
// LuxSampler playback) but the UDP thread still needs to update raw_R/G/B
// so the RAW visualizer and Source=L pipeline stay live.
void audio_image_buffers_snapshot_raw_external(AudioImageBuffers *buffers,
                                               const uint8_t *srcR,
                                               const uint8_t *srcG,
                                               const uint8_t *srcB,
                                               int nb_pixels);

// Lock-free read of the last pure UDP frame (no sampler contamination).
void audio_image_buffers_get_raw_pointers(const AudioImageBuffers *buffers,
                                          uint8_t **out_R, uint8_t **out_G,
                                          uint8_t **out_B);

// ── Sampler snapshot (written only by FramePlayerThread) ──────────────────
// Call snapshot_sampler() from FramePlayerThread BEFORE blending with live
// to capture the pure sampler frame.
void audio_image_buffers_snapshot_sampler(AudioImageBuffers *buffers,
                                          const uint8_t *srcR,
                                          const uint8_t *srcG,
                                          const uint8_t *srcB,
                                          int nb_pixels);

// Lock-free read of the last pure sampler frame (no live contamination).
void audio_image_buffers_get_sampler_pointers(const AudioImageBuffers *buffers,
                                              uint8_t **out_R, uint8_t **out_G,
                                              uint8_t **out_B);

// ── Selection tap (contextual visualizer) ──────────────────────────────────
// Publish the stream frame at the SELECTED module's chain position.  Called by
// the chain executor (udpThread / feeder) when the plan carries a viz tap
// (SynthChainPlan.viz_tap_insert >= 0).  Single producer / multi reader.
void audio_image_buffers_publish_selection_tap(AudioImageBuffers *buffers,
                                               const uint8_t *srcR,
                                               const uint8_t *srcG,
                                               const uint8_t *srcB,
                                               int nb_pixels);

// Lock-free read of the last published selection-tap frame.
void audio_image_buffers_get_selection_tap_pointers(const AudioImageBuffers *buffers,
                                                    uint8_t **out_R,
                                                    uint8_t **out_G,
                                                    uint8_t **out_B);

// Reset the selection tap to white — called (message thread) when the selected
// module changes so the view never shows the PREVIOUS selection's frame while
// the new target's chain is silent/unfed.
void audio_image_buffers_clear_selection_tap(AudioImageBuffers *buffers);

// ── Per-engine input taps (per-chain display) ──────────────────────────────
// Publish the RGB frame `engine`'s pipeline consumes this cycle. Pass
// srcR == NULL to publish WHITE (engine unfed: no-signal chain, absent
// module, playback silence). Must be called by the thread that owns the
// engine's preprocessed commit this cycle (udpThread / feeder /
// FramePlayerThread — mutually exclusive per the source-routing arbitration).
void audio_image_buffers_publish_engine_input(AudioImageBuffers *buffers,
                                              int engine,
                                              const uint8_t *srcR,
                                              const uint8_t *srcG,
                                              const uint8_t *srcB,
                                              int nb_pixels);

// Lock-free read of the last published engine-input frame.
// Returns 0 on success, -1 on invalid engine / uninitialized buffers.
int audio_image_buffers_get_engine_input_pointers(const AudioImageBuffers *buffers,
                                                  int engine,
                                                  uint8_t **out_R,
                                                  uint8_t **out_G,
                                                  uint8_t **out_B);

#endif /* AUDIO_IMAGE_BUFFERS_H */
