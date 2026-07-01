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

  // ── Modulated snapshot (written by the synthesis thread) ─────────────────
  // Holds the last frame after the full insert chain.  Actual runtime order:
  //     Live ► [LuxPitch ⇄ LuxMask, order = chainInsertOrder] ► LuxSampler
  // (the sampler records the post-insert frame; during playback the sampler
  // frame IS the modulated output — inserts are already "printed").
  // This is the buffer consumed by the synth engines when their source is
  // set to MODULATED, and the buffer mirrored by the video waterfall.
  uint8_t *modulated_R;
  uint8_t *modulated_G;
  uint8_t *modulated_B;

  // ── Per-insert visual taps (written by the synthesis thread) ─────────────
  // tap[i] holds the last output frame of insert i (see IMAGE_CHAIN_INSERT_*
  // in processing/image_chain.h).  Only snapshotted when a visual consumer
  // declared demand (image_chain_set_tap_demand) — zero cost otherwise.
  // Single producer (synthesis thread) / multi reader (UI visualizers).
#define AUDIO_IMAGE_NUM_INSERT_TAPS 2
  uint8_t *insert_tap_R[AUDIO_IMAGE_NUM_INSERT_TAPS];
  uint8_t *insert_tap_G[AUDIO_IMAGE_NUM_INSERT_TAPS];
  uint8_t *insert_tap_B[AUDIO_IMAGE_NUM_INSERT_TAPS];


  // Statistics and monitoring
  uint64_t lines_received;
  uint64_t lines_processed;
  uint64_t buffer_swaps;

  // Number of snapshots published to modulated_R/G/B by the synthesis thread.
  // Incremented exactly once per audio_image_buffers_snapshot_modulated() call,
  // letting downstream consumers (e.g. waterfall capture) detect a fresh
  // modulated frame even when the UDP write bus is idle (sampler playback
  // suppresses lines_received).
  // Single producer (synthesis thread) → atomic load on the reader side.
  uint64_t lines_modulated;

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

// Release the write_mutex WITHOUT swapping buffers — used by the live publish
// path when the acquisition gate holds the current frame (sample-and-hold).
// Pairs with start_write() exactly like complete_write(), but performs no buffer
// rotation, so the read buffer (and every consumer) keeps the last published
// line.  UDP thread only.
void audio_image_buffers_abort_write(AudioImageBuffers *buffers);

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

// ── Modulated snapshot (written by the synthesis thread) ──────────────────
// Called once per synthesis cycle after the full insert chain runs
// (Live ► LuxSampler ► LuxPitch ► LuxMask).  Single-producer / multi-reader.
void audio_image_buffers_snapshot_modulated(AudioImageBuffers *buffers,
                                            const uint8_t *srcR,
                                            const uint8_t *srcG,
                                            const uint8_t *srcB,
                                            int nb_pixels);

// Lock-free read of the last modulated frame.  Mirrors what the synth
// engines consume when their source is set to MODULATED.
void audio_image_buffers_get_modulated_pointers(const AudioImageBuffers *buffers,
                                                uint8_t **out_R,
                                                uint8_t **out_G,
                                                uint8_t **out_B);

// ── Per-insert visual taps (written by the synthesis thread) ──────────────
// Snapshot the output of insert `tap` (IMAGE_CHAIN_INSERT_*).  Called by the
// chain executor only when a visual consumer declared demand.
void audio_image_buffers_snapshot_insert_tap(AudioImageBuffers *buffers,
                                             int tap,
                                             const uint8_t *srcR,
                                             const uint8_t *srcG,
                                             const uint8_t *srcB,
                                             int nb_pixels);

// Lock-free read of the last published tap frame.  Returns 0 on success,
// -1 if the tap index is invalid or buffers are not initialized.
int audio_image_buffers_get_insert_tap_pointers(const AudioImageBuffers *buffers,
                                                int tap,
                                                uint8_t **out_R,
                                                uint8_t **out_G,
                                                uint8_t **out_B);


// Utility functions
void audio_image_buffers_get_stats(AudioImageBuffers *buffers,
                                   uint64_t *lines_received,
                                   uint64_t *lines_processed,
                                   uint64_t *buffer_swaps);

#endif /* AUDIO_IMAGE_BUFFERS_H */
