/* audio_image_buffers.c */

#include "audio_image_buffers.h"
#include "config.h"
#include "config_instrument.h"
#include "error.h"
#include "logger.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Initialize the dual buffer system for audio-image data
 * @param buffers Pointer to AudioImageBuffers structure
 * @return 0 on success, -1 on error
 */
int audio_image_buffers_init(AudioImageBuffers *buffers) {
  int nb_pixels;
  int i;
  float phase;
  uint8_t test_value;

  if (!buffers) {
    fprintf(stderr, "ERROR: AudioImageBuffers pointer is NULL\n");
    return -1;
  }

  // Initialize all pointers to NULL for safe cleanup
  memset(buffers, 0, sizeof(AudioImageBuffers));

  // Get runtime pixel count
  nb_pixels = get_cis_pixels_nb();

  // Allocate Buffer 0 - RGB channels separated for memory contiguity
  buffers->buffer0_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->buffer0_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->buffer0_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate Buffer 1 - RGB channels separated for memory contiguity
  buffers->buffer1_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->buffer1_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->buffer1_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate raw UDP snapshot buffers
  buffers->raw_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->raw_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->raw_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate sampler snapshot buffers
  buffers->sampler_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->sampler_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->sampler_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate modulated snapshot buffers (post-Sampler/Pitch/Mask chain)
  buffers->modulated_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->modulated_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->modulated_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Check all allocations
  if (!buffers->buffer0_R   || !buffers->buffer0_G   || !buffers->buffer0_B   ||
      !buffers->buffer1_R   || !buffers->buffer1_G   || !buffers->buffer1_B   ||
      !buffers->raw_R       || !buffers->raw_G       || !buffers->raw_B       ||
      !buffers->sampler_R   || !buffers->sampler_G   || !buffers->sampler_B   ||
      !buffers->modulated_R || !buffers->modulated_G || !buffers->modulated_B) {

    fprintf(stderr, "ERROR: Failed to allocate audio image buffers\n");
    audio_image_buffers_cleanup(buffers);
    return -1;
  }

  // Initialize buffers with test pattern to ensure audio synthesis works
  // This provides immediate audio feedback even without scanner data
  for (i = 0; i < nb_pixels; i++) {
    // Create a simple test pattern: sine wave for audio testing
    phase = (float)i / nb_pixels * 2.0f * M_PI * 4.0f; // 4 cycles
    test_value = (uint8_t)(127.0f + 127.0f * sin(phase));

    buffers->buffer0_R[i] = test_value;
    buffers->buffer0_G[i] = test_value / 2; // Different pattern for G
    buffers->buffer0_B[i] = test_value / 4; // Different pattern for B

    buffers->buffer1_R[i] = test_value;
    buffers->buffer1_G[i] = test_value / 2;
    buffers->buffer1_B[i] = test_value / 4;
  }

  // Initialize raw snapshot with white (no UDP data yet)
  memset(buffers->raw_R, 255, nb_pixels);
  memset(buffers->raw_G, 255, nb_pixels);
  memset(buffers->raw_B, 255, nb_pixels);

  // Initialize sampler snapshot with white (no sampler data yet)
  memset(buffers->sampler_R, 255, nb_pixels);
  memset(buffers->sampler_G, 255, nb_pixels);
  memset(buffers->sampler_B, 255, nb_pixels);

  // Initialize modulated snapshot with white (no synthesis cycle yet)
  memset(buffers->modulated_R, 255, nb_pixels);
  memset(buffers->modulated_G, 255, nb_pixels);
  memset(buffers->modulated_B, 255, nb_pixels);


  log_info("BUFFERS", "Audio image buffers initialized with test pattern for immediate audio feedback");

  // Initialize atomic indices
  // Buffer 0 starts as read buffer, Buffer 1 starts as write buffer
  atomic_init(&buffers->read_buffer_index, 0);
  atomic_init(&buffers->write_buffer_index, 1);

  // Initialize write mutex
  if (pthread_mutex_init(&buffers->write_mutex, NULL) != 0) {
    fprintf(stderr, "ERROR: Failed to initialize write mutex\n");
    audio_image_buffers_cleanup(buffers);
    return -1;
  }

  // Initialize statistics
  buffers->lines_received = 0;
  buffers->lines_processed = 0;
  buffers->buffer_swaps = 0;
  buffers->initialized = 1;

  log_info("BUFFERS", "Dual buffer system initialized: 2 x %d pixels x 3 channels", nb_pixels);
  log_info("BUFFERS", "Initial state: Buffer 0 = read, Buffer 1 = write");

  return 0;
}

/**
 * @brief Cleanup the dual buffer system
 * @param buffers Pointer to AudioImageBuffers structure
 */
void audio_image_buffers_cleanup(AudioImageBuffers *buffers) {
  if (!buffers) {
    return;
  }

  // Free all allocated memory
  if (buffers->buffer0_R) {
    free(buffers->buffer0_R);
    buffers->buffer0_R = NULL;
  }
  if (buffers->buffer0_G) {
    free(buffers->buffer0_G);
    buffers->buffer0_G = NULL;
  }
  if (buffers->buffer0_B) {
    free(buffers->buffer0_B);
    buffers->buffer0_B = NULL;
  }
  if (buffers->buffer1_R) {
    free(buffers->buffer1_R);
    buffers->buffer1_R = NULL;
  }
  if (buffers->buffer1_G) {
    free(buffers->buffer1_G);
    buffers->buffer1_G = NULL;
  }
  if (buffers->buffer1_B) {
    free(buffers->buffer1_B);
    buffers->buffer1_B = NULL;
  }
  if (buffers->raw_R) {
    free(buffers->raw_R);
    buffers->raw_R = NULL;
  }
  if (buffers->raw_G) {
    free(buffers->raw_G);
    buffers->raw_G = NULL;
  }
  if (buffers->raw_B) {
    free(buffers->raw_B);
    buffers->raw_B = NULL;
  }
  if (buffers->sampler_R) {
    free(buffers->sampler_R);
    buffers->sampler_R = NULL;
  }
  if (buffers->sampler_G) {
    free(buffers->sampler_G);
    buffers->sampler_G = NULL;
  }
  if (buffers->sampler_B) {
    free(buffers->sampler_B);
    buffers->sampler_B = NULL;
  }
  if (buffers->modulated_R) {
    free(buffers->modulated_R);
    buffers->modulated_R = NULL;
  }
  if (buffers->modulated_G) {
    free(buffers->modulated_G);
    buffers->modulated_G = NULL;
  }
  if (buffers->modulated_B) {
    free(buffers->modulated_B);
    buffers->modulated_B = NULL;
  }

  // Destroy mutex if initialized

  if (buffers->initialized) {
    pthread_mutex_destroy(&buffers->write_mutex);
  }

  // Reset structure
  memset(buffers, 0, sizeof(AudioImageBuffers));

  log_info("BUFFERS", "Audio image buffers cleanup completed");
}

/**
 * @brief Start writing to the current write buffer (UDP thread)
 * @param buffers Pointer to AudioImageBuffers structure
 * @param out_R Pointer to receive R channel write buffer
 * @param out_G Pointer to receive G channel write buffer
 * @param out_B Pointer to receive B channel write buffer
 * @return 0 on success, -1 on error
 */
int audio_image_buffers_start_write(AudioImageBuffers *buffers, uint8_t **out_R,
                                    uint8_t **out_G, uint8_t **out_B) {
  if (!buffers || !buffers->initialized) {
    fprintf(stderr, "ERROR: AudioImageBuffers not initialized\n");
    return -1;
  }

  if (!out_R || !out_G || !out_B) {
    fprintf(stderr, "ERROR: Output pointers are NULL\n");
    return -1;
  }

  // Lock write mutex to protect against concurrent UDP writes
  pthread_mutex_lock(&buffers->write_mutex);

  // Get current write buffer index atomically
  int write_idx = atomic_load(&buffers->write_buffer_index);

  // Return pointers to the current write buffer
  if (write_idx == 0) {
    *out_R = buffers->buffer0_R;
    *out_G = buffers->buffer0_G;
    *out_B = buffers->buffer0_B;
  } else {
    *out_R = buffers->buffer1_R;
    *out_G = buffers->buffer1_G;
    *out_B = buffers->buffer1_B;
  }

  // Note: Mutex remains locked until complete_write() is called
  return 0;
}

/**
 * @brief Complete writing and perform atomic buffer swap (UDP thread)
 * @param buffers Pointer to AudioImageBuffers structure
 */
void audio_image_buffers_complete_write(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized) {
    fprintf(stderr, "ERROR: AudioImageBuffers not initialized\n");
    return;
  }

  // Perform atomic buffer swap
  // The write buffer becomes the new read buffer
  // The old read buffer becomes the new write buffer
  int old_write_idx = atomic_load(&buffers->write_buffer_index);
  int old_read_idx = atomic_load(&buffers->read_buffer_index);

  // Atomic swap: exchange read and write indices
  atomic_store(&buffers->read_buffer_index, old_write_idx);
  atomic_store(&buffers->write_buffer_index, old_read_idx);

  // Update statistics
  buffers->lines_received++;
  buffers->buffer_swaps++;

  // Unlock write mutex
  pthread_mutex_unlock(&buffers->write_mutex);

  // Debug log (can be disabled for production)
#ifdef DEBUG_BUFFERS
  static uint64_t log_counter = 0;
  if ((log_counter++ % 1000) == 0) { // Log every 1000 swaps
    printf("AudioImageBuffers: Swapped buffers (read=%d, write=%d) - %llu "
           "lines received\n",
           atomic_load(&buffers->read_buffer_index),
           atomic_load(&buffers->write_buffer_index), buffers->lines_received);
  }
#endif
}

/**
 * @brief Get read pointers for LuxStral synthesis processing (lock-free)
 * @param buffers Pointer to AudioImageBuffers structure
 * @param out_R Pointer to receive R channel read buffer
 * @param out_G Pointer to receive G channel read buffer
 * @param out_B Pointer to receive B channel read buffer
 */
void audio_image_buffers_get_read_pointers(AudioImageBuffers *buffers,
                                           uint8_t **out_R, uint8_t **out_G,
                                           uint8_t **out_B) {
  if (!buffers || !buffers->initialized) {
    fprintf(stderr, "ERROR: AudioImageBuffers not initialized\n");
    return;
  }

  if (!out_R || !out_G || !out_B) {
    fprintf(stderr, "ERROR: Output pointers are NULL\n");
    return;
  }

  // Get current read buffer index atomically (no mutex needed!)
  int read_idx = atomic_load(&buffers->read_buffer_index);

  // Return pointers to the current read buffer
  if (read_idx == 0) {
    *out_R = buffers->buffer0_R;
    *out_G = buffers->buffer0_G;
    *out_B = buffers->buffer0_B;
  } else {
    *out_R = buffers->buffer1_R;
    *out_G = buffers->buffer1_G;
    *out_B = buffers->buffer1_B;
  }

  // Update statistics (note: this is not thread-safe but only for monitoring)
  buffers->lines_processed++;
}

/**
 * @brief Get buffer statistics
 * @param buffers Pointer to AudioImageBuffers structure
 * @param lines_received Pointer to receive lines received count
 * @param lines_processed Pointer to receive lines processed count
 * @param buffer_swaps Pointer to receive buffer swaps count
 */
void audio_image_buffers_get_stats(AudioImageBuffers *buffers,
                                   uint64_t *lines_received,
                                   uint64_t *lines_processed,
                                   uint64_t *buffer_swaps) {
  if (!buffers || !buffers->initialized) {
    if (lines_received)
      *lines_received = 0;
    if (lines_processed)
      *lines_processed = 0;
    if (buffer_swaps)
      *buffer_swaps = 0;
    return;
  }

  if (lines_received)
    *lines_received = buffers->lines_received;
  if (lines_processed)
    *lines_processed = buffers->lines_processed;
  if (buffer_swaps)
    *buffer_swaps = buffers->buffer_swaps;
}

/**
 * @brief Capture a snapshot of the current read buffer into the raw buffers.
 *
 * DEPRECATED: reads from the read buffer AFTER complete_write() which is
 * racy — FramePlayerThread may swap the buffer between complete_write() and
 * snapshot_raw(), corrupting raw_R/G/B with sampler data.
 * Use audio_image_buffers_snapshot_raw_before_swap() instead.
 *
 * @param buffers Pointer to AudioImageBuffers structure
 */
void audio_image_buffers_snapshot_raw(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized)
    return;

  int nb_pixels = get_cis_pixels_nb();

  int read_idx = atomic_load(&buffers->read_buffer_index);
  if (read_idx == 0) {
    memcpy(buffers->raw_R, buffers->buffer0_R, nb_pixels);
    memcpy(buffers->raw_G, buffers->buffer0_G, nb_pixels);
    memcpy(buffers->raw_B, buffers->buffer0_B, nb_pixels);
  } else {
    memcpy(buffers->raw_R, buffers->buffer1_R, nb_pixels);
    memcpy(buffers->raw_G, buffers->buffer1_G, nb_pixels);
    memcpy(buffers->raw_B, buffers->buffer1_B, nb_pixels);
  }
}

/**
 * @brief Snapshot raw UDP data from the WRITE buffer BEFORE complete_write().
 *
 * FIX(routing): The original snapshot_raw() read from the read buffer AFTER
 * the buffer swap.  Between complete_write() and snapshot_raw(), FramePlayerThread
 * could call its own complete_write(), making the read buffer point to sampler
 * data — corrupting raw_R/G/B.
 *
 * This variant reads from the WRITE buffer (which holds the freshly assembled
 * UDP frame) while write_mutex is still held, guaranteeing that raw_R/G/B
 * is always pure UDP data — never contaminated by the sampler.
 *
 * Must be called BETWEEN start_write() and complete_write() (write_mutex held).
 *
 * @param buffers Pointer to AudioImageBuffers structure
 */
void audio_image_buffers_snapshot_raw_before_swap(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized)
    return;

  int nb_pixels = get_cis_pixels_nb();

  /* Read from the WRITE buffer — this is the buffer the UDP thread just filled.
   * write_mutex is currently held by the caller so no other thread can swap
   * or modify this buffer until complete_write() releases the mutex. */
  int write_idx = atomic_load(&buffers->write_buffer_index);
  if (write_idx == 0) {
    memcpy(buffers->raw_R, buffers->buffer0_R, nb_pixels);
    memcpy(buffers->raw_G, buffers->buffer0_G, nb_pixels);
    memcpy(buffers->raw_B, buffers->buffer0_B, nb_pixels);
  } else {
    memcpy(buffers->raw_R, buffers->buffer1_R, nb_pixels);
    memcpy(buffers->raw_G, buffers->buffer1_G, nb_pixels);
    memcpy(buffers->raw_B, buffers->buffer1_B, nb_pixels);
  }
}

/**
 * @brief Snapshot raw from external data (no write_mutex required).
 *
 * Used when the AudioImageBuffers write bus was not started (e.g. during
 * LuxSampler playback) but the UDP thread still needs to update raw_R/G/B
 * so the RAW visualizer and Source=L pipeline path stay live.
 *
 * @param buffers   Pointer to AudioImageBuffers structure
 * @param srcR      Source R channel (pure UDP frame)
 * @param srcG      Source G channel (pure UDP frame)
 * @param srcB      Source B channel (pure UDP frame)
 * @param nb_pixels Number of pixels to copy
 */
void audio_image_buffers_snapshot_raw_external(AudioImageBuffers *buffers,
                                               const uint8_t *srcR,
                                               const uint8_t *srcG,
                                               const uint8_t *srcB,
                                               int nb_pixels) {
  if (!buffers || !buffers->initialized || !srcR || !srcG || !srcB || nb_pixels <= 0)
    return;

  memcpy(buffers->raw_R, srcR, nb_pixels);
  memcpy(buffers->raw_G, srcG, nb_pixels);
  memcpy(buffers->raw_B, srcB, nb_pixels);
}

/**
 * @brief Get pointers to the last pure UDP frame (lock-free, read-only).
 *
 * These buffers are never written to by the sampler — they always
 * reflect the last frame received from the CIS sensor via UDP.
 *
 * @param buffers Pointer to AudioImageBuffers structure (const)
 * @param out_R   Receives pointer to raw R channel
 * @param out_G   Receives pointer to raw G channel
 * @param out_B   Receives pointer to raw B channel
 */
void audio_image_buffers_get_raw_pointers(const AudioImageBuffers *buffers,
                                          uint8_t **out_R, uint8_t **out_G,
                                          uint8_t **out_B) {
  if (!buffers || !buffers->initialized || !out_R || !out_G || !out_B)
    return;

  *out_R = buffers->raw_R;
  *out_G = buffers->raw_G;
  *out_B = buffers->raw_B;
}

/**
 * @brief Capture a snapshot of the pure sampler frame into the sampler buffers.
 *
 * Must be called ONLY from the FramePlayerThread, passing the raw slot data
 * BEFORE blending with live.  This ensures sampler_R/G/B always contain the
 * pure sampler frame — never contaminated by the live stream.
 *
 * @param buffers   Pointer to AudioImageBuffers structure
 * @param srcR      Source R channel (pure sampler frame)
 * @param srcG      Source G channel (pure sampler frame)
 * @param srcB      Source B channel (pure sampler frame)
 * @param nb_pixels Number of pixels to copy
 */
void audio_image_buffers_snapshot_sampler(AudioImageBuffers *buffers,
                                          const uint8_t *srcR,
                                          const uint8_t *srcG,
                                          const uint8_t *srcB,
                                          int nb_pixels) {
  if (!buffers || !buffers->initialized || !srcR || !srcG || !srcB || nb_pixels <= 0)
    return;

  memcpy(buffers->sampler_R, srcR, nb_pixels);
  memcpy(buffers->sampler_G, srcG, nb_pixels);
  memcpy(buffers->sampler_B, srcB, nb_pixels);
}

/**
 * @brief Get pointers to the last pure sampler frame (lock-free, read-only).
 *
 * These buffers are never written to by the UDP thread — they always
 * reflect the last frame played by the FramePlayerThread.
 *
 * @param buffers Pointer to AudioImageBuffers structure (const)
 * @param out_R   Receives pointer to sampler R channel
 * @param out_G   Receives pointer to sampler G channel
 * @param out_B   Receives pointer to sampler B channel
 */
void audio_image_buffers_get_sampler_pointers(const AudioImageBuffers *buffers,
                                              uint8_t **out_R, uint8_t **out_G,
                                              uint8_t **out_B) {
  if (!buffers || !buffers->initialized || !out_R || !out_G || !out_B)
    return;

  *out_R = buffers->sampler_R;
  *out_G = buffers->sampler_G;
  *out_B = buffers->sampler_B;
}

/**
 * @brief Capture a snapshot of the post-insert modulated frame.
 *
 * Single-producer (synthesis thread).  Called once per synthesis cycle, AFTER
 * the LuxSampler ► LuxPitch ► LuxMask chain has run, with the resulting RGB
 * pointers.  Multi-reader: video waterfall, visualizers, etc.
 *
 * @param buffers   Pointer to AudioImageBuffers structure
 * @param srcR      Source R channel (modulated frame)
 * @param srcG      Source G channel (modulated frame)
 * @param srcB      Source B channel (modulated frame)
 * @param nb_pixels Number of pixels to copy
 */
void audio_image_buffers_snapshot_modulated(AudioImageBuffers *buffers,
                                            const uint8_t *srcR,
                                            const uint8_t *srcG,
                                            const uint8_t *srcB,
                                            int nb_pixels) {
  if (!buffers || !buffers->initialized || !srcR || !srcG || !srcB || nb_pixels <= 0)
    return;

  memcpy(buffers->modulated_R, srcR, nb_pixels);
  memcpy(buffers->modulated_G, srcG, nb_pixels);
  memcpy(buffers->modulated_B, srcB, nb_pixels);

  // Publish a new generation tag so consumers polling `lines_modulated` can
  // detect a fresh modulated frame even while the UDP write bus is idle (e.g.
  // during LuxSampler playback where `lines_received` stays frozen).
  __atomic_store_n(&buffers->lines_modulated,
                   buffers->lines_modulated + 1u,
                   __ATOMIC_RELEASE);
}

/**
 * @brief Get pointers to the last modulated frame (lock-free, read-only).
 *
 * @param buffers Pointer to AudioImageBuffers structure (const)
 * @param out_R   Receives pointer to modulated R channel
 * @param out_G   Receives pointer to modulated G channel
 * @param out_B   Receives pointer to modulated B channel
 */
void audio_image_buffers_get_modulated_pointers(const AudioImageBuffers *buffers,
                                                uint8_t **out_R,
                                                uint8_t **out_G,
                                                uint8_t **out_B) {
  if (!buffers || !buffers->initialized || !out_R || !out_G || !out_B)
    return;

  *out_R = buffers->modulated_R;
  *out_G = buffers->modulated_G;
  *out_B = buffers->modulated_B;
}

