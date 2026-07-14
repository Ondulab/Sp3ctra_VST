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

  // Allocate selection-tap buffers (contextual visualizer)
  buffers->selection_tap_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->selection_tap_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  buffers->selection_tap_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate per-engine input tap buffers (per-chain display)
  for (i = 0; i < AUDIO_IMAGE_NUM_ENGINE_TAPS; i++) {
    buffers->engine_tap_R[i] = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
    buffers->engine_tap_G[i] = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
    buffers->engine_tap_B[i] = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  }

  // Check all allocations
  if (!buffers->buffer0_R   || !buffers->buffer0_G   || !buffers->buffer0_B   ||
      !buffers->buffer1_R   || !buffers->buffer1_G   || !buffers->buffer1_B) {

    fprintf(stderr, "ERROR: Failed to allocate audio image buffers\n");
    audio_image_buffers_cleanup(buffers);
    return -1;
  }
  if (!buffers->selection_tap_R || !buffers->selection_tap_G ||
      !buffers->selection_tap_B) {
    fprintf(stderr, "ERROR: Failed to allocate selection tap buffers\n");
    audio_image_buffers_cleanup(buffers);
    return -1;
  }
  for (i = 0; i < AUDIO_IMAGE_NUM_ENGINE_TAPS; i++) {
    if (!buffers->engine_tap_R[i] || !buffers->engine_tap_G[i] ||
        !buffers->engine_tap_B[i]) {
      fprintf(stderr, "ERROR: Failed to allocate engine input tap buffers\n");
      audio_image_buffers_cleanup(buffers);
      return -1;
    }
  }

  // Initialize the dual buffers with WHITE (blank paper): before the first
  // real line arrives, every consumer must see an empty — silent — stream.
  // (The historical sine "test pattern for immediate audio feedback" made
  // the default chain content non-blank and audible.)
  memset(buffers->buffer0_R, 255, nb_pixels);
  memset(buffers->buffer0_G, 255, nb_pixels);
  memset(buffers->buffer0_B, 255, nb_pixels);
  memset(buffers->buffer1_R, 255, nb_pixels);
  memset(buffers->buffer1_G, 255, nb_pixels);
  memset(buffers->buffer1_B, 255, nb_pixels);

  // Initialize the selection tap with white (nothing selected/published yet)
  memset(buffers->selection_tap_R, 255, nb_pixels);
  memset(buffers->selection_tap_G, 255, nb_pixels);
  memset(buffers->selection_tap_B, 255, nb_pixels);

  // Initialize engine input taps with white (no engine fed yet — white is
  // silence in the image-to-sound mapping, matching the unfed contract)
  for (i = 0; i < AUDIO_IMAGE_NUM_ENGINE_TAPS; i++) {
    memset(buffers->engine_tap_R[i], 255, nb_pixels);
    memset(buffers->engine_tap_G[i], 255, nb_pixels);
    memset(buffers->engine_tap_B[i], 255, nb_pixels);
  }


  log_info("BUFFERS", "Audio image buffers initialized WHITE (blank paper — empty-chain contract)");

  // Initialize atomic indices
  // Buffer 0 starts as read buffer, Buffer 1 starts as write buffer
  atomic_init(&buffers->read_buffer_index, 0);
  atomic_init(&buffers->write_buffer_index, 1);

  // Acquisition gate starts disabled (full-rate, legacy behaviour)
  atomic_init(&buffers->gate_enabled, 0);
  atomic_init(&buffers->gate_permit, 0);
  buffers->gate_holds = 0;

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
  if (buffers->selection_tap_R) { free(buffers->selection_tap_R); buffers->selection_tap_R = NULL; }
  if (buffers->selection_tap_G) { free(buffers->selection_tap_G); buffers->selection_tap_G = NULL; }
  if (buffers->selection_tap_B) { free(buffers->selection_tap_B); buffers->selection_tap_B = NULL; }

  {
    int e;
    for (e = 0; e < AUDIO_IMAGE_NUM_ENGINE_TAPS; e++) {
      if (buffers->engine_tap_R[e]) { free(buffers->engine_tap_R[e]); buffers->engine_tap_R[e] = NULL; }
      if (buffers->engine_tap_G[e]) { free(buffers->engine_tap_G[e]); buffers->engine_tap_G[e] = NULL; }
      if (buffers->engine_tap_B[e]) { free(buffers->engine_tap_B[e]); buffers->engine_tap_B[e] = NULL; }
    }
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
 * @brief Enable/disable the acquisition gate (audio thread).
 */
void audio_image_buffers_gate_set_enabled(AudioImageBuffers *buffers, int enabled) {
  if (!buffers || !buffers->initialized)
    return;
  atomic_store(&buffers->gate_enabled, enabled ? 1 : 0);
}

/**
 * @brief Grant exactly one frame advance (audio thread, on each gate tick).
 */
void audio_image_buffers_gate_grant(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized)
    return;
  atomic_store(&buffers->gate_permit, 1);
}

/**
 * @brief Decide whether the live publish path may swap now (UDP thread).
 * @return 1 if the gate is disabled or a permit was pending (consumed); 0 to hold.
 */
int audio_image_buffers_gate_should_publish(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized)
    return 1;
  if (!atomic_load(&buffers->gate_enabled))
    return 1;
  // Consume one permit (max 1 — the gate only brakes, never fast-forwards).
  return atomic_exchange(&buffers->gate_permit, 0);
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
 * @brief Publish the selection-tap frame (contextual visualizer)
 *
 * Single producer (whichever chain executor hosts the selected module —
 * udpThread or feeder tick, mutually exclusive by the 250 ms live/feeder
 * hysteresis), multi reader (UI visualizer at 30 fps). Same tearing model as
 * every other snapshot bus here: a torn line is repainted one frame later.
 */
void audio_image_buffers_publish_selection_tap(AudioImageBuffers *buffers,
                                               const uint8_t *srcR,
                                               const uint8_t *srcG,
                                               const uint8_t *srcB,
                                               int nb_pixels) {
  if (!buffers || !buffers->initialized || !srcR || !srcG || !srcB)
    return;

  int max_pixels = get_cis_pixels_nb();
  int count = (nb_pixels < max_pixels) ? nb_pixels : max_pixels;
  if (count <= 0)
    return;

  memcpy(buffers->selection_tap_R, srcR, (size_t)count);
  memcpy(buffers->selection_tap_G, srcG, (size_t)count);
  memcpy(buffers->selection_tap_B, srcB, (size_t)count);
}

void audio_image_buffers_get_selection_tap_pointers(const AudioImageBuffers *buffers,
                                                    uint8_t **out_R,
                                                    uint8_t **out_G,
                                                    uint8_t **out_B) {
  if (!buffers || !buffers->initialized || !out_R || !out_G || !out_B)
    return;
  *out_R = buffers->selection_tap_R;
  *out_G = buffers->selection_tap_G;
  *out_B = buffers->selection_tap_B;
}

void audio_image_buffers_clear_selection_tap(AudioImageBuffers *buffers) {
  if (!buffers || !buffers->initialized)
    return;
  int nb_pixels = get_cis_pixels_nb();
  memset(buffers->selection_tap_R, 255, (size_t)nb_pixels);
  memset(buffers->selection_tap_G, 255, (size_t)nb_pixels);
  memset(buffers->selection_tap_B, 255, (size_t)nb_pixels);
}

/**
 * @brief Publish the frame engine `engine` consumes this cycle (per-chain
 *        display). srcR == NULL publishes WHITE (engine unfed).
 *
 * Single producer at any instant: the thread that owns the engine's
 * preprocessed commit this cycle (udpThread / feeder tick /
 * FramePlayerThread — mutually exclusive per the source-routing
 * arbitration). Multi reader (UI). Same tearing model as the other
 * snapshot buses: a torn line is repainted one frame later.
 */
void audio_image_buffers_publish_engine_input(AudioImageBuffers *buffers,
                                              int engine,
                                              const uint8_t *srcR,
                                              const uint8_t *srcG,
                                              const uint8_t *srcB,
                                              int nb_pixels) {
  if (!buffers || !buffers->initialized || engine < 0 ||
      engine >= AUDIO_IMAGE_NUM_ENGINE_TAPS)
    return;

  int max_pixels = get_cis_pixels_nb();
  int count = (nb_pixels < max_pixels) ? nb_pixels : max_pixels;
  if (count <= 0)
    return;

  if (srcR && srcG && srcB) {
    memcpy(buffers->engine_tap_R[engine], srcR, (size_t)count);
    memcpy(buffers->engine_tap_G[engine], srcG, (size_t)count);
    memcpy(buffers->engine_tap_B[engine], srcB, (size_t)count);
  } else {
    memset(buffers->engine_tap_R[engine], 255, (size_t)count);
    memset(buffers->engine_tap_G[engine], 255, (size_t)count);
    memset(buffers->engine_tap_B[engine], 255, (size_t)count);
  }

  /* Freshness tick (P4-M3): the engine taps ARE the rendered frames — any
   * publish, from any producer thread (udp/feeder/player), means "new frame
   * available". fetch_add: multiple concurrent producers, unlike the old
   * single-producer modulated counter. */
  __atomic_fetch_add(&buffers->frame_seq, 1u, __ATOMIC_RELEASE);
}

int audio_image_buffers_get_engine_input_pointers(const AudioImageBuffers *buffers,
                                                  int engine,
                                                  uint8_t **out_R,
                                                  uint8_t **out_G,
                                                  uint8_t **out_B) {
  if (!buffers || !buffers->initialized || engine < 0 ||
      engine >= AUDIO_IMAGE_NUM_ENGINE_TAPS || !out_R || !out_G || !out_B)
    return -1;

  *out_R = buffers->engine_tap_R[engine];
  *out_G = buffers->engine_tap_G[engine];
  *out_B = buffers->engine_tap_B[engine];
  return 0;
}

