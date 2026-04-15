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

  // Statistics and monitoring
  uint64_t lines_received;
  uint64_t lines_processed;
  uint64_t buffer_swaps;

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

// LuxStral synth thread functions (lock-free read)
void audio_image_buffers_get_read_pointers(AudioImageBuffers *buffers,
                                           uint8_t **out_R, uint8_t **out_G,
                                           uint8_t **out_B);

// ── Raw UDP snapshot (written only by UDP thread, never by sampler) ────────
// Call snapshot_raw() from the UDP receive path AFTER complete_write()
// to capture the pure UDP frame before the sampler can overwrite it.
void audio_image_buffers_snapshot_raw(AudioImageBuffers *buffers);

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

// Utility functions
void audio_image_buffers_get_stats(AudioImageBuffers *buffers,
                                   uint64_t *lines_received,
                                   uint64_t *lines_processed,
                                   uint64_t *buffer_swaps);

#endif /* AUDIO_IMAGE_BUFFERS_H */
