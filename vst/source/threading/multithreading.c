/* multithreading.c */

#include "multithreading.h"
#include "audio_c_api.h"
#include "config.h"
#include "config_instrument.h"
#include "config_loader.h"
#include "config_synth_luxstral.h" /* For IMU_FILTER_ALPHA_X, AUTO_VOLUME_POLL_MS */
#include "context.h"
#include "error.h"
#include "synth_luxstral.h"
#include "../synthesis/luxstral/luxstral_engine.h" /* display buffer accessors */
#include "udp.h"
#include "logger.h"
#include "../utils/image_debug_stubs.h"
#include "../utils/rt_profiler.h"
#include "../processing/image_preprocessor.h"
#include "../processing/image_pipeline.h"
#include "../processing/image_chain.h"
#include "../processing/chain_plan.h"
#include "../processing/lux_pitch.h"
#include "../processing/lux_mask.h"
#include "../processing/video_scroll.h"
#include "../processing/image_sequencer.h"
#include "../processing/internal_source.h"
#include "../synthesis/luxwave/synth_luxwave.h"
#include <time.h>
#include <sys/time.h>

/* VST synchronization function declaration (defined in vst_adapters.cpp) */
#ifdef VST_MODE
extern void luxstral_wait_for_buffer_consumed(void);
#include "../luxsampler/lux_sampler_hooks.h"
#endif

/* External sequencer instance */
extern ImageSequencer *g_image_sequencer;

/* ── M8: LuxStral engine B — independent input DoubleBuffer ──────────────────
 * Both the UDP thread (producer: fills preprocessed_data from engine B's OWN
 * chain) and audioProcessingThread (consumer: renders engine B) live in this
 * translation unit, so a file-static DoubleBuffer keeps the whole 2nd-voice
 * input path self-contained (no Context / Sp3ctraCore plumbing). Lazily
 * initialised on first use in the UDP thread. */
#ifdef VST_MODE
static DoubleBuffer          s_luxstral_b_db;
static PreprocessedImageData s_preprocessed_temp_b;   /* UDP-thread scratch (single writer) */
static int                   s_luxstral_b_db_ready = 0;
#endif

#ifndef NO_SFML
#include <SFML/Graphics.h>
#include <SFML/Network.h>
#endif // NO_SFML

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*------------------------------------------------------------------------------
    Helper Functions
------------------------------------------------------------------------------*/

/*
// Wait for DMX color update using condition variable
static void WaitForDMXColorUpdate(DMXContext *ctx) {
  pthread_mutex_lock(&ctx->mutex);
  while (!ctx->colorUpdated) {
    pthread_cond_wait(&ctx->cond, &ctx->mutex);
  }
  pthread_mutex_unlock(&ctx->mutex);
}
*/

int initDoubleBuffer(DoubleBuffer *db) {
  int nb_pixels;

  /* Failures return -1 (caller decides) — never exit(EXIT_FAILURE): inside a
   * plugin that would kill the whole DAW process. */
  if (pthread_mutex_init(&db->mutex, NULL) != 0) {
    log_error("THREAD", "Mutex initialization failed");
    return -1;
  }
  if (pthread_cond_init(&db->cond, NULL) != 0) {
    log_error("THREAD", "Condition variable initialization failed");
    pthread_mutex_destroy(&db->mutex);
    return -1;
  }

  nb_pixels = get_cis_pixels_nb();
  
  db->activeBuffer_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->activeBuffer_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->activeBuffer_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  db->processingBuffer_R =
      (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->processingBuffer_G =
      (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->processingBuffer_B =
      (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  // Allocate persistent image buffers for audio continuity
  db->lastValidImage_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->lastValidImage_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  db->lastValidImage_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  if (!db->activeBuffer_R || !db->activeBuffer_G || !db->activeBuffer_B ||
      !db->processingBuffer_R || !db->processingBuffer_G ||
      !db->processingBuffer_B || !db->lastValidImage_R ||
      !db->lastValidImage_G || !db->lastValidImage_B) {
    log_error("THREAD", "Allocation of image buffers failed");
    free(db->activeBuffer_R);     db->activeBuffer_R = NULL;
    free(db->activeBuffer_G);     db->activeBuffer_G = NULL;
    free(db->activeBuffer_B);     db->activeBuffer_B = NULL;
    free(db->processingBuffer_R); db->processingBuffer_R = NULL;
    free(db->processingBuffer_G); db->processingBuffer_G = NULL;
    free(db->processingBuffer_B); db->processingBuffer_B = NULL;
    free(db->lastValidImage_R);   db->lastValidImage_R = NULL;
    free(db->lastValidImage_G);   db->lastValidImage_G = NULL;
    free(db->lastValidImage_B);   db->lastValidImage_B = NULL;
    pthread_cond_destroy(&db->cond);
    pthread_mutex_destroy(&db->mutex);
    return -1;
  }

  // Initialize persistent image with black (zero)
  memset(db->lastValidImage_R, 0, nb_pixels);
  memset(db->lastValidImage_G, 0, nb_pixels);
  memset(db->lastValidImage_B, 0, nb_pixels);

  db->dataReady = 0;
  db->lastValidImageExists = 0;
  db->udp_frames_received = 0;
  db->audio_frames_processed = 0;
  db->last_udp_frame_time = time(NULL);
  
  // 🔧 BUGFIX: Initialize preprocessed_data with safe default values
  // This prevents bus errors when audio thread starts before first UDP frame
  
  /* Initialize additive synthesis data */
  memset(db->preprocessed_data.additive.grayscale, 0, sizeof(db->preprocessed_data.additive.grayscale));
  memset(db->preprocessed_data.additive.notes, 0, sizeof(db->preprocessed_data.additive.notes));
  db->preprocessed_data.additive.contrast_factor = 1.0f;
  
  /* Initialize polyphonic synthesis data */
#ifndef DISABLE_LUXSYNTH
  memset(db->preprocessed_data.polyphonic.grayscale, 0, sizeof(db->preprocessed_data.polyphonic.grayscale));
  memset(db->preprocessed_data.polyphonic.magnitudes, 0, sizeof(db->preprocessed_data.polyphonic.magnitudes));
  db->preprocessed_data.polyphonic.valid = 0;
#endif
  
  /* Initialize photowave synthesis data */
  memset(db->preprocessed_data.photowave.r, 0, sizeof(db->preprocessed_data.photowave.r));
  memset(db->preprocessed_data.photowave.g, 0, sizeof(db->preprocessed_data.photowave.g));
  memset(db->preprocessed_data.photowave.b, 0, sizeof(db->preprocessed_data.photowave.b));
  
  /* Initialize stereo with center panning (equal-power law) */
  for (int i = 0; i < PREPROCESS_MAX_NOTES; i++) {
    db->preprocessed_data.stereo.pan_positions[i] = 0.0f;  // Center
    db->preprocessed_data.stereo.left_gains[i] = 0.707f;   // -3dB (equal power)
    db->preprocessed_data.stereo.right_gains[i] = 0.707f;  // -3dB (equal power)
  }
  
  /* Initialize DMX with black */
#ifdef USE_DMX
  memset(&db->preprocessed_data.dmx, 0, sizeof(db->preprocessed_data.dmx));
#endif
  
  db->preprocessed_data.timestamp_us = 0;

  log_info("THREAD", "DoubleBuffer preprocessed_data initialized with safe defaults");
  return 0;
}

void cleanupDoubleBuffer(DoubleBuffer *db) {
  if (db) {
    free(db->activeBuffer_R);
    free(db->activeBuffer_G);
    free(db->activeBuffer_B);
    free(db->processingBuffer_R);
    free(db->processingBuffer_G);
    free(db->processingBuffer_B);
    free(db->lastValidImage_R);
    free(db->lastValidImage_G);
    free(db->lastValidImage_B);

    db->activeBuffer_R = NULL;
    db->activeBuffer_G = NULL;
    db->activeBuffer_B = NULL;
    db->processingBuffer_R = NULL;
    db->processingBuffer_G = NULL;
    db->processingBuffer_B = NULL;
    db->lastValidImage_R = NULL;
    db->lastValidImage_G = NULL;
    db->lastValidImage_B = NULL;

    pthread_mutex_destroy(&db->mutex);
    pthread_cond_destroy(&db->cond);
  }
}

void swapBuffers(DoubleBuffer *db) {
  uint8_t *temp = NULL;

  temp = db->activeBuffer_R;
  db->activeBuffer_R = db->processingBuffer_R;
  db->processingBuffer_R = temp;

  temp = db->activeBuffer_G;
  db->activeBuffer_G = db->processingBuffer_G;
  db->processingBuffer_G = temp;

  temp = db->activeBuffer_B;
  db->activeBuffer_B = db->processingBuffer_B;
  db->processingBuffer_B = temp;
}

/**
 * @brief Update the persistent image buffer with latest valid image
 * @param db DoubleBuffer structure
 * @note Mutex must be locked before calling this function
 */
void updateLastValidImage(DoubleBuffer *db) {
  int nb_pixels = get_cis_pixels_nb();
  
  // Copy processing buffer to persistent image buffer
  memcpy(db->lastValidImage_R, db->processingBuffer_R, nb_pixels);
  memcpy(db->lastValidImage_G, db->processingBuffer_G, nb_pixels);
  memcpy(db->lastValidImage_B, db->processingBuffer_B, nb_pixels);

  db->lastValidImageExists = 1;
  db->udp_frames_received++;
  db->last_udp_frame_time = time(NULL);

  // Debug logs removed for production use
}

/**
 * @brief Get the last valid image for audio processing (thread-safe)
 * @param db DoubleBuffer structure
 * @param out_R Output buffer for red channel
 * @param out_G Output buffer for green channel
 * @param out_B Output buffer for blue channel
 */
void getLastValidImageForAudio(DoubleBuffer *db, uint8_t *out_R, uint8_t *out_G,
                               uint8_t *out_B) {
  int nb_pixels = get_cis_pixels_nb();
  
  pthread_mutex_lock(&db->mutex);

  if (db->lastValidImageExists) {
    memcpy(out_R, db->lastValidImage_R, nb_pixels);
    memcpy(out_G, db->lastValidImage_G, nb_pixels);
    memcpy(out_B, db->lastValidImage_B, nb_pixels);
    db->audio_frames_processed++;
  } else {
    // If no valid image exists, use black (silence)
    memset(out_R, 0, nb_pixels);
    memset(out_G, 0, nb_pixels);
    memset(out_B, 0, nb_pixels);
    // Debug log removed for production use
  }

  pthread_mutex_unlock(&db->mutex);
}

/**
 * @brief Check if a valid image exists for audio processing
 * @param db DoubleBuffer structure
 * @return 1 if valid image exists, 0 otherwise
 */
int hasValidImageForAudio(DoubleBuffer *db) {
  pthread_mutex_lock(&db->mutex);
  int exists = db->lastValidImageExists;
  pthread_mutex_unlock(&db->mutex);
  return exists;
}

/*------------------------------------------------------------------------------
    Thread Implementations
------------------------------------------------------------------------------*/
// Assume that Context, DoubleBuffer, packet_Image, UDP_MAX_NB_PACKET_PER_LINE,
// IMAGE_DATA_HEADER and swapBuffers() are defined elsewhere.
// It is also assumed that the Context structure now contains a boolean field
// 'enableImageTransform' to toggle image transformation at runtime.

/* Resolve the per-insert state pointers of one synth's chain plan (Pitch/Mask
 * pool instances, VideoScroll probe rings) for image_chain_run(). Marker and
 * unknown ids get NULL — the executor treats them as pass-through. Shared by
 * the three per-synth input selections in udpThread (single source of truth
 * for the insert-id → state-pool mapping). */
static void chain_resolve_insert_states(const SynthChainPlan *sp,
                                        void *states[CHAIN_PLAN_MAX_INSERTS])
{
    for (int i = 0; i < sp->num_inserts; i++)
    {
        switch (sp->insert_id[i])
        {
            case IMAGE_CHAIN_INSERT_LUXPITCH:
                states[i] = (void *)lux_pitch_instance(sp->insert_state_idx[i]); break;
            case IMAGE_CHAIN_INSERT_LUXMASK:
                states[i] = (void *)lux_mask_instance(sp->insert_state_idx[i]); break;
            case IMAGE_CHAIN_INSERT_VIDEOSCROLL:
                states[i] = (void *)video_scroll_instance(sp->insert_state_idx[i]); break;
            default:
                states[i] = NULL; break;
        }
    }
}

/* ── M9: per-synth base frame — the chain's SOURCE module output ─────────────
 * LIVE / NONE / unavailable-internal → the shared live frame (db->activeBuffer,
 * legacy behaviour). IMAGE / VIDEO / CAMERA → the latest line published by that
 * source's engine, copied into a per-synth scratch so the pointers stay stable
 * for the rest of the frame.
 *
 * The scratch is written by whichever thread currently drives the per-synth
 * processing (udpThread while the device streams, the feeder tick otherwise —
 * see internal_source_live_streaming()). The 250 ms hand-over hysteresis makes
 * concurrent writes to the same synth slot practically impossible; a glitched
 * frame at the boundary is acceptable. */
static uint8_t s_synth_src_scratch[CHAIN_SYNTH_COUNT][3][INTERNAL_SRC_MAX_PIXELS];

static int synth_source_base(const SynthChainPlan *sp, int synth_slot,
                             DoubleBuffer *db, int nb_pixels,
                             const uint8_t **out_r, const uint8_t **out_g,
                             const uint8_t **out_b)
{
    const int kind = internal_source_kind_for_chain_src(sp->source_kind);
    if (kind >= 0)
    {
        uint8_t *r = s_synth_src_scratch[synth_slot][0];
        uint8_t *g = s_synth_src_scratch[synth_slot][1];
        uint8_t *b = s_synth_src_scratch[synth_slot][2];
        if (internal_source_copy(kind, r, g, b, nb_pixels) > 0)
        {
            *out_r = r; *out_g = g; *out_b = b;
            return 1;
        }
    }
    *out_r = db->activeBuffer_R;
    *out_g = db->activeBuffer_G;
    *out_b = db->activeBuffer_B;
    return 0;
}

void *udpThread(void *arg) {
  Context *ctx;
  DoubleBuffer *db;
  AudioImageBuffers *audioBuffers;
  int s;
  struct sockaddr_in *si_other;
  socklen_t slen;
  ssize_t recv_len;
  struct packet_Image packet;
  int nb_pixels;
  uint8_t *mixed_R;
  uint8_t *mixed_G;
  uint8_t *mixed_B;
  uint32_t currentLineId;
  int *receivedFragments;
  uint32_t fragmentCount;
  uint8_t *audio_write_R;
  uint8_t *audio_write_G;
  uint8_t *audio_write_B;
  int audio_write_started;
  /* Acquisition-gate hold buffers: the last GRANTED line, repeated downstream
   * while the gate holds so audio + video freeze together (no dropped lines). */
  uint8_t *held_R;
  uint8_t *held_G;
  uint8_t *held_B;
  int held_line_valid;

  /* Initialize variables */
  ctx = (Context *)arg;
  db = ctx->doubleBuffer;
  audioBuffers = ctx->audioImageBuffers;
  s = ctx->socket;
  si_other = ctx->si_other;
  slen = sizeof(*si_other);
  nb_pixels = get_cis_pixels_nb();
  mixed_R = NULL;
  mixed_G = NULL;
  mixed_B = NULL;
  currentLineId = 0;
  fragmentCount = 0;
  audio_write_R = NULL;
  audio_write_G = NULL;
  audio_write_B = NULL;
  audio_write_started = 0;
  held_R = NULL;
  held_G = NULL;
  held_B = NULL;
  held_line_valid = 0;

  /* Allocate receivedFragments.
   * Allocation failures abort THIS thread only (return NULL) — never
   * exit(EXIT_FAILURE): inside a plugin that would kill the whole DAW
   * process and the user's unsaved project. */
  receivedFragments = (int *)calloc(UDP_MAX_NB_PACKET_PER_LINE, sizeof(int));
  if (receivedFragments == NULL) {
    log_error("THREAD", "Error allocating receivedFragments: %s", strerror(errno));
    return NULL;
  }

  /* Allocate mixed buffers dynamically */
  mixed_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  mixed_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  mixed_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));

  if (!mixed_R || !mixed_G || !mixed_B) {
    log_error("THREAD", "Failed to allocate mixed buffers");
    if (mixed_R) free(mixed_R);
    if (mixed_G) free(mixed_G);
    if (mixed_B) free(mixed_B);
    free(receivedFragments);
    return NULL;
  }

  /* Allocate acquisition-gate hold buffers */
  held_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  held_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  held_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  if (!held_R || !held_G || !held_B) {
    log_error("THREAD", "Failed to allocate acquisition-gate hold buffers");
    if (held_R) free(held_R);
    if (held_G) free(held_G);
    if (held_B) free(held_B);
    free(mixed_R);
    free(mixed_G);
    free(mixed_B);
    free(receivedFragments);
    return NULL;
  }

  log_info("THREAD", "UDP thread started with dual buffer system");
  log_info("THREAD", "Listening for packets on socket %d, expecting IMAGE_DATA_HEADER (0x%02X)", s, IMAGE_DATA_HEADER);

  int first_packet_logged = 0;  // Log very first packet after restart

  while (ctx->running) {
    recv_len = recvfrom(s, &packet, sizeof(packet), 0,
                        (struct sockaddr *)si_other, &slen);
    if (recv_len < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("THREAD", "recvfrom error: %s", strerror(errno));
      }
      continue;
    }

    // 🔧 CRITICAL FIX: Check ctx->running AFTER each packet reception
    // If packets arrive continuously (95000+), recvfrom() never times out
    // and the thread never re-checks the while() condition.
    // This check ensures the thread can exit promptly when requested.
    if (!ctx->running) {
      log_info("THREAD", "ctx->running=0 detected, exiting main loop");
      // 🔧 BUGFIX: Release write_mutex if held (prevents deadlock on restart)
      if (audio_write_started) {
        audio_image_buffers_complete_write(audioBuffers);
        audio_image_buffers_snapshot_raw(audioBuffers);
        audio_write_started = 0;
        log_info("THREAD", "Released write_mutex before exit (incomplete line)");
      }
      break;
    }

    // Log very first packet (to confirm reception restarted)
    if (!first_packet_logged) {
      log_info("THREAD", "🟢 FIRST PACKET after restart! type=0x%02X size=%zd socket=%d",
               packet.type, recv_len, s);
      first_packet_logged = 1;
    }

#ifdef DEBUG_UDP
    // Debug: Log every received packet
    log_debug("UDP", "Received packet: size=%zd bytes, type=0x%02X", recv_len, packet.type);
#endif

    if (packet.type == IMU_DATA_HEADER) {
      /* Lightweight IMU packet handling: update filtered X in Context.
         Keep this code fast and non-blocking. Comments in English as per
         project conventions. */
      struct packet_IMU *imu = (struct packet_IMU *)&packet;
      pthread_mutex_lock(&ctx->imu_mutex);
      float raw_x = imu->acc[0]; // X axis accelerometer
      float raw_y = imu->acc[1]; // Y axis accelerometer
      float raw_z = imu->acc[2]; // Z axis accelerometer
      
      /* Store RAW accelerometer values for display effects (reactive, no filtering) */
      ctx->imu_raw_x = raw_x;
      ctx->imu_raw_y = raw_y;
      ctx->imu_raw_z = raw_z;
      
      /* Store RAW gyroscope values (rad/s) */
      ctx->imu_gyro_x = imu->gyro[0];
      ctx->imu_gyro_y = imu->gyro[1];
      ctx->imu_gyro_z = imu->gyro[2];
      
      /* Store INTEGRATED positions (from sensor's onboard integration) */
      ctx->imu_position_x = imu->integrated_acc[0];
      ctx->imu_position_y = imu->integrated_acc[1];
      ctx->imu_position_z = imu->integrated_acc[2];
      
      /* Store INTEGRATED angles (from sensor's onboard integration, radians) */
      ctx->imu_angle_x = imu->integrated_gyro[0];
      ctx->imu_angle_y = imu->integrated_gyro[1];
      ctx->imu_angle_z = imu->integrated_gyro[2];
      
      /* Keep filtered X for auto-volume (needs stability) */
      if (!ctx->imu_has_value) {
        ctx->imu_x_filtered = raw_x;
        ctx->imu_has_value = 1;
#ifdef DEBUG_IMU_PACKETS
        log_debug("IMU", "First IMU packet received! raw_x=%.6f", raw_x);
#endif
      } else {
        ctx->imu_x_filtered = IMU_FILTER_ALPHA_X * raw_x +
                              (1.0f - IMU_FILTER_ALPHA_X) * ctx->imu_x_filtered;
      }
      ctx->last_imu_time = time(NULL);
      pthread_mutex_unlock(&ctx->imu_mutex);

#ifdef DEBUG_IMU_PACKETS
      log_debug("IMU", "raw_x=%.6f filtered=%.6f threshold=%.6f active=%s", raw_x,
                ctx->imu_x_filtered, g_luxstral_config.imu_active_threshold_x,
                (fabsf(ctx->imu_x_filtered) >= g_luxstral_config.imu_active_threshold_x) ? "YES" : "NO");
#endif
#ifdef DEBUG_UDP
      log_debug("UDP", "IMU raw_x=%.6f filtered=%.6f", raw_x, ctx->imu_x_filtered);
#endif
      continue;
    }

    if (packet.type != IMAGE_DATA_HEADER) {
#ifdef DEBUG_UDP
      log_debug("UDP", "Ignoring packet with type 0x%02X (expected 0x%02X)", packet.type, IMAGE_DATA_HEADER);
#endif
      continue;
    }

#ifdef DEBUG_UDP
    log_debug("UDP", "Processing IMAGE_DATA packet: line_id=%u, fragment_id=%u/%u, size=%u",
              packet.line_id, packet.fragment_id, packet.total_fragments, packet.fragment_size);
#endif

    if (currentLineId != packet.line_id) {
      // If we had a previous incomplete line, log it
      if (currentLineId != 0 && fragmentCount > 0) {
#ifdef DEBUG_UDP
        log_debug("UDP", "INCOMPLETE LINE DISCARDED: line_id=%u had %u/%d fragments",
                  currentLineId, fragmentCount, UDP_MAX_NB_PACKET_PER_LINE);
#endif

        // Complete the incomplete audio buffer write if it was started
        if (audio_write_started) {
          /* Snapshot raw BEFORE complete_write to avoid sampler contamination */
          audio_image_buffers_snapshot_raw_before_swap(audioBuffers);
          audio_image_buffers_complete_write(audioBuffers);
          audio_write_started = 0;
#ifdef DEBUG_UDP
          log_debug("UDP", "Completed partial audio buffer write for incomplete line");
#endif
        }
      }

      // New line started - prepare for writing
      currentLineId = packet.line_id;
      memset(receivedFragments, 0, UDP_MAX_NB_PACKET_PER_LINE * sizeof(int));
      fragmentCount = 0;

      // Start writing to audio buffers for new line.
      // During LuxSampler playback, bypass live feed: FramePlayerThread is
      // the sole writer. Skipping start_write here prevents overwriting
      // injected playback frames.
#ifdef VST_MODE
      if (!lux_sampler_is_playing())
      {
#endif
        if (audio_image_buffers_start_write(audioBuffers, &audio_write_R,
                                            &audio_write_G,
                                            &audio_write_B) == 0) {
          audio_write_started = 1;
#ifdef DEBUG_UDP
          log_debug("UDP", "Started audio buffer write for line_id=%u", packet.line_id);
#endif
        } else {
          audio_write_started = 0;
          log_warning("THREAD", "Failed to start audio buffer write");
        }
#ifdef VST_MODE
      }
      else
      {
        /* Playback active: live write skipped for this line */
        audio_write_started = 0;
      }
#endif
    }

    // Validate fragment_id to prevent buffer overflow
    if (packet.fragment_id >= UDP_MAX_NB_PACKET_PER_LINE) {
      log_error("THREAD", "fragment_id %u exceeds maximum %u, ignoring packet",
                packet.fragment_id, UDP_MAX_NB_PACKET_PER_LINE);
      continue;
    }

    uint32_t offset = packet.fragment_id * packet.fragment_size;
    if (!receivedFragments[packet.fragment_id]) {
      receivedFragments[packet.fragment_id] = 1;
      fragmentCount++;

      // Write to legacy double buffer (for display)
      memcpy(&db->activeBuffer_R[offset], packet.imageData_R,
             packet.fragment_size);
      memcpy(&db->activeBuffer_G[offset], packet.imageData_G,
             packet.fragment_size);
      memcpy(&db->activeBuffer_B[offset], packet.imageData_B,
             packet.fragment_size);

      // Write to new audio buffers (for continuous audio)
      if (audio_write_started) {
        memcpy(&audio_write_R[offset], packet.imageData_R,
               packet.fragment_size);
        memcpy(&audio_write_G[offset], packet.imageData_G,
               packet.fragment_size);
        memcpy(&audio_write_B[offset], packet.imageData_B,
               packet.fragment_size);
      }
    }

#ifdef DEBUG_UDP
    log_debug("UDP", "Fragment count: %u/%u for line %u", fragmentCount, packet.total_fragments, packet.line_id);
#endif

    if (fragmentCount == packet.total_fragments) {
      PreprocessedImageData preprocessed_temp;

      // 🔧 CRITICAL FIX: Abort heavy processing if stop requested
      if (!ctx->running) {
        log_info("THREAD", "ctx->running=0 detected before heavy processing, exiting");
        // 🔧 BUGFIX: Release write_mutex if held (prevents deadlock on restart)
        if (audio_write_started) {
          audio_image_buffers_complete_write(audioBuffers);
        audio_image_buffers_snapshot_raw(audioBuffers);
          audio_write_started = 0;
          log_info("THREAD", "Released write_mutex before exit (incomplete line)");
        }
        break;
      }

      /* ── Acquisition gate ("vitesse d'acquisition") ──────────────────────────
       * FREEZE, don't cut.  Decide ONCE whether this freshly assembled line
       * ADVANCES the stream or is HELD.  We never drop a held line: dropping
       * "cuts" the waterfall (it scrolls black) and lets the audio thread's
       * fallback keep re-preprocessing fresh data, so the freeze never reaches
       * the sound.  Instead, on HOLD we overwrite the incoming line with the
       * last GRANTED line in the single upstream source every consumer derives
       * from — db->activeBuffer AND the AudioImageBuffers write buffer — then let
       * the WHOLE pipeline run normally.  Audio and video therefore freeze on the
       * exact same held frame until the next gate tick / trig.  Gate disabled
       * (mode Off) ⇒ should_publish() always 1 = full rate (legacy). */
      {
        int gate_advance = audio_image_buffers_gate_should_publish(audioBuffers);
        if (gate_advance || !held_line_valid) {
          /* ADVANCE (or first line): latch this raw line as the new held frame. */
          memcpy(held_R, db->activeBuffer_R, nb_pixels);
          memcpy(held_G, db->activeBuffer_G, nb_pixels);
          memcpy(held_B, db->activeBuffer_B, nb_pixels);
          held_line_valid = 1;
        } else {
          /* HOLD: replace the incoming line with the latched frame so every
           * downstream path (audio preprocessing, LuxSynth/LuxWave, video
           * waterfall, display, AudioImageBuffers) sees the same held data. */
          memcpy(db->activeBuffer_R, held_R, nb_pixels);
          memcpy(db->activeBuffer_G, held_G, nb_pixels);
          memcpy(db->activeBuffer_B, held_B, nb_pixels);
          if (audio_write_started) {
            memcpy(audio_write_R, held_R, nb_pixels);
            memcpy(audio_write_G, held_G, nb_pixels);
            memcpy(audio_write_B, held_B, nb_pixels);
          }
        }
      }

#ifdef DEBUG_UDP
      log_debug("UDP", "COMPLETE LINE RECEIVED! line_id=%u, %u fragments", packet.line_id, fragmentCount);
#endif
      /* Complete line received */

      /* M9: stamp live activity — while the device streams, THIS thread drives
       * the per-synth processing (internal sources substituted below); the
       * SourceFeederThread stays out of the way. */
      internal_source_note_live_line();

      /* Complete audio buffer write and swap.
       * FIX(routing): Snapshot raw BEFORE complete_write so that raw_R/G/B
       * always contains pure UDP data.  If snapshot_raw() were called AFTER
       * complete_write(), FramePlayerThread could race and swap the buffer
       * between the two calls, causing sampler data to contaminate raw_R/G/B
       * (and therefore the LIVE visualizer / Source=L pipeline path). */
      if (audio_write_started) {
        audio_image_buffers_snapshot_raw_before_swap(audioBuffers);
        audio_image_buffers_complete_write(audioBuffers);
        audio_write_started = 0;
      }
      else {
        /* FIX(raw): Write bus was not started (sampler is playing and owns
         * AudioImageBuffers), but the RAW snapshot must still reflect the
         * live UDP data so the RAW visualizer and Source=L pipeline path
         * stay live during sampler playback. */
        audio_image_buffers_snapshot_raw_external(audioBuffers,
            db->activeBuffer_R, db->activeBuffer_G, db->activeBuffer_B,
            nb_pixels);
      }

      /* LuxSampler hook (phase 1 — live frame assembled).
       *
       * The image chain is now: Live → LuxPitch → LuxMask → LuxSampler.
       * LuxSampler must therefore see the post-mask frame to:
       *   • feed the Modulated channel from idle/REC passthrough
       *   • record into a slot with Pitch+Mask already applied
       *
       * Phase 1 here drains start/stop record commands and caches the live
       * frame for FramePlayerThread.  Phase 2 (the actual capture + sampler
       * snapshot update) happens AFTER LuxPitch + LuxMask have run — see
       * lux_sampler_on_modulated_frame_ready() below.
       */
#ifdef VST_MODE
      lux_sampler_on_live_frame_assembled(
          db->activeBuffer_R, db->activeBuffer_G, db->activeBuffer_B,
          (uint16_t)nb_pixels);
#endif

      /* 🎬 NEW ARCHITECTURE: Sequencer BEFORE preprocessing
       * 1. Sequencer mixes RGB (live + sequences)
       * 2. Preprocessing calculates grayscale/pan/DMX from MIXED RGB
       * 3. Display shows the MIXED RGB colors
       */
      
      /* Step 1: Mix RGB through sequencer (or passthrough if no sequencer) */
      if (g_image_sequencer) {
        if (image_sequencer_process_frame(g_image_sequencer,
                                          db->activeBuffer_R, db->activeBuffer_G, db->activeBuffer_B,
                                          mixed_R, mixed_G, mixed_B) != 0) {
          log_error("THREAD", "Sequencer processing failed, using live RGB");
          memcpy(mixed_R, db->activeBuffer_R, nb_pixels);
          memcpy(mixed_G, db->activeBuffer_G, nb_pixels);
          memcpy(mixed_B, db->activeBuffer_B, nb_pixels);
        }
      } else {
        /* No sequencer: passthrough live RGB */
        memcpy(mixed_R, db->activeBuffer_R, nb_pixels);
        memcpy(mixed_G, db->activeBuffer_G, nb_pixels);
        memcpy(mixed_B, db->activeBuffer_B, nb_pixels);
      }
      
      /* Step 2: Preprocess via pipeline — channel routing selects either
       * the MODULATED chain or the raw LIVE feed for each synthesis path.
       *
       *   Channel A — MODULATED : Live ► LuxSampler ► LuxPitch ► LuxMask
       *     • base frame  = sampler frame (FramePlayerThread output when a slot
       *       plays, otherwise live UDP passthrough)
       *     • LuxPitch and LuxMask auto-bypass when inactive (no voice, no
       *       config.enabled, etc.) — no branching required at this level
       *   Channel B — LIVE      : db->activeBuffer (raw UDP, no processing)
       *
       * Both synthesis paths (LuxStral and LuxSynth+LuxWave) independently pick
       * one of these two channels via their own APVTS source parameter.
       */
      {
        PipelineConfig live_cfg = pipeline_build_config_live();

        const uint8_t *src_R;
        const uint8_t *src_G;
        const uint8_t *src_B;

        /* ── Build the MODULATED frame once — used by any path that selects it.
         * The inserts run inside their own preallocated buffers (see lux_pitch.c
         * / lux_mask.c, no allocation in this hot path).  When disabled or no
         * voice is active the engines short-circuit and return the input
         * pointers as-is, so the cost is O(1). */
        const uint8_t *mod_R = NULL;
        const uint8_t *mod_G = NULL;
        const uint8_t *mod_B = NULL;
        int            need_modulated =
            (live_cfg.luxstral_path.source         == IMAGE_SOURCE_MODULATED) ||
            (live_cfg.luxsynth_luxwave_path.source == IMAGE_SOURCE_MODULATED) ||
            image_chain_any_tap_demand(); /* a visualizer watches an insert tap */

        if (need_modulated)
        {
            /* ── Chain: Live ► [LuxPitch ⇄ LuxMask] ► LuxSampler ──
             *
             * The insert order is configurable (chainInsertOrder param, see
             * image_chain.h) and executed by image_chain_process_inserts().
             *
             * The Modulated channel is the FINAL stage of the image chain
             * (= what the synth engines actually consume when source=MODULATED,
             * and what the VideoScroll waterfall mirrors).  The sampler is
             * the last stage, so two cases:
             *
             *   • PLAYING : modulated = sampler frame directly.
             *               The recorded sample already contains the Pitch +
             *               Mask processing that was active at record time
             *               (see Phase 2 hook below) — re-applying Pitch/Mask
             *               on top would double-apply them.  Therefore the
             *               sampler output is the final modulated signal as-is.
             *
             *   • IDLE / REC / STEP_LIVE :
             *               modulated = LuxMask(LuxPitch(live)).
             *               LuxSampler::onModulatedFrameReady() then mirrors
             *               this post-mask result back into the sampler
             *               snapshot AND writes it into the active recording
             *               slot, so anything captured already has Pitch+Mask
             *               baked in.
             */
#ifdef VST_MODE
            if (lux_sampler_is_playing())
            {
                /* PLAYING: sampler frame IS the modulated output.  No Pitch/Mask
                 * re-application — recorded samples already contain them. */
                uint8_t *smpR, *smpG, *smpB;
                audio_image_buffers_get_sampler_pointers(audioBuffers, &smpR, &smpG, &smpB);
                mod_R = smpR;
                mod_G = smpG;
                mod_B = smpB;

                if (mod_R && mod_G && mod_B)
                {
                    audio_image_buffers_snapshot_modulated(audioBuffers,
                                                           mod_R, mod_G, mod_B,
                                                           nb_pixels);

                    /* RESAMPLING: a slot is playing INTO the modulated channel.
                     * Feed the final modulated frame to every sampler engine
                     * with an armed rec slot so a downstream sampler records the
                     * combination (e.g. sampler B records sampler A's playback).
                     * No snapshot mirror — the playing engine owns the snapshot. */
#ifdef VST_MODE
                    lux_samplers_record_modulated(mod_R, mod_G, mod_B,
                                                  (uint16_t)nb_pixels,
                                                  packet.line_id);
#endif
                }
            }
            else
#endif
            {
                /* IDLE / REC / STEP_LIVE: run the insert chain on the raw
                 * live frame (configurable order, per-insert visual taps),
                 * publish as modulated, then let the sampler hook mirror &
                 * record it. */
                image_chain_process_inserts(db->activeBuffer_R,
                                            db->activeBuffer_G,
                                            db->activeBuffer_B,
                                            nb_pixels,
                                            g_sp3ctra_config.num_octaves,
                                            &mod_R, &mod_G, &mod_B,
                                            audioBuffers);

                if (mod_R && mod_G && mod_B)
                {
                    audio_image_buffers_snapshot_modulated(audioBuffers,
                                                           mod_R, mod_G, mod_B,
                                                           nb_pixels);

                    /* LuxSampler hook (phase 2 — modulated frame ready).
                     * Mirrors the post-mask frame into the sampler snapshot
                     * (so the sampler visualiser stays alive in idle) and
                     * writes it into the active recording slot, so recorded
                     * samples are Pitch+Mask "printed". */
#ifdef VST_MODE
                    lux_sampler_on_modulated_frame_ready(mod_R, mod_G, mod_B,
                                                         (uint16_t)nb_pixels,
                                                         packet.line_id);
#endif
                }
            }
        }


        /* ── ONE ChainPlan snapshot per frame ─────────────────────────────────
         * The three per-synth input selections below (LuxStral A, LuxStral B,
         * LuxSynth/LuxWave) must all see the SAME topology: taking a fresh
         * snapshot per synth could mix two different plans within one frame
         * when a rack edit is published mid-frame. */
        ChainPlan frame_plan;
        chain_plan_get(&frame_plan);

        /* ── Pick LuxStral's input (M6 Phase 2 — fed by ITS OWN chain) ────────
         * • A chain holding the Sampler IS the modulated channel (mod) — it
         *   carries the sampler playback frame + the recorded Pitch/Mask, so
         *   reuse mod (handles playing + idle). This keeps the sampler/recording
         *   path untouched.
         * • Otherwise run LuxStral's own ordered inserts (its per-chain Pitch/
         *   Mask instances) over the live frame, so LuxStral's processing is
         *   independent of every other chain. No inserts → raw live. */
        {
            const SynthChainPlan *spA = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL];

            /* M9: base frame = the chain's SOURCE module output (live feed, or
             * the IMAGE/VIDEO/CAMERA internal source line when one is placed
             * and active in LuxStral's chain). */
            const uint8_t *baseA_R, *baseA_G, *baseA_B;
            synth_source_base(spA, CHAIN_SYNTH_LUXSTRAL, db, nb_pixels,
                              &baseA_R, &baseA_G, &baseA_B);

            if (spA->present && spA->has_sampler && mod_R)
            {
                src_R = mod_R; src_G = mod_G; src_B = mod_B;   /* modulated/sampler channel */
                /* This short-circuit skips image_chain_run (the sampler chain IS the
                 * modulated channel). Feed VideoScroll probes by their POSITION
                 * relative to the sampler: a probe ABOVE the sampler shows the live
                 * source frame (what the sampler records); a probe BELOW shows the
                 * modulated/sampler output. (Per-chain Pitch/Mask upstream of the
                 * sampler aren't reflected in the pre-sampler tap — v1 limitation.) */
                int after_sampler = 0;
                for (int i = 0; i < spA->num_inserts; i++)
                {
                    if (spA->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER) { after_sampler = 1; continue; }
                    if (spA->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                    {
                        const uint8_t *fr = after_sampler ? mod_R : baseA_R;
                        const uint8_t *fg = after_sampler ? mod_G : baseA_G;
                        const uint8_t *fb = after_sampler ? mod_B : baseA_B;
                        video_scroll_capture_line(
                            video_scroll_instance(spA->insert_state_idx[i]),
                            fr, fg, fb, nb_pixels);
                    }
                }
            }
            else if (spA->present && spA->num_inserts > 0)
            {
                void *states[CHAIN_PLAN_MAX_INSERTS];
                chain_resolve_insert_states(spA, states);
                image_chain_run(baseA_R, baseA_G, baseA_B,
                                nb_pixels, g_sp3ctra_config.num_octaves,
                                spA->insert_id, states, spA->num_inserts,
                                &src_R, &src_G, &src_B);
            }
            else
            {
                src_R = baseA_R;   /* chain source (no inserts, no sampler) */
                src_G = baseA_G;
                src_B = baseA_B;
            }
        }

        if (pipeline_process_frame(src_R, src_G, src_B, &live_cfg, &preprocessed_temp) != 0) {
          log_error("THREAD", "Pipeline processing failed");
        }

#ifdef VST_MODE
        /* ── LuxStral engine B (M8) — fed by ITS OWN chain, into s_luxstral_b_db ─
         * Computed HERE (before db->activeBuffer is overwritten with the display
         * mix below) using the live frame, mirroring engine A's src selection.
         * Committed to the file-static DoubleBuffer that audioProcessingThread
         * hands to synth_AudioProcess_b(). Independent scratch so A's not-yet-
         * committed preprocessed_temp is untouched. */
        {
            if (!s_luxstral_b_db_ready) {
                if (initDoubleBuffer(&s_luxstral_b_db) != 0) {
                    log_error("THREAD", "Engine-B DoubleBuffer init failed — engine B stays inactive");
                } else {
                    // RELEASE so the audio thread, on seeing ready=1 (ACQUIRE), also
                    // sees the fully-initialised DoubleBuffer (mutex, buffers).
                    __atomic_store_n(&s_luxstral_b_db_ready, 1, __ATOMIC_RELEASE);
                }
            }

            const SynthChainPlan *spLB = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL_B];
            if (spLB->present && !s_luxstral_b_db_ready)
            {
                /* DoubleBuffer init failed above — its mutex/buffers are not
                 * usable, skip engine-B feeding entirely. */
            }
            else if (spLB->present && spLB->source_kind == CHAIN_SRC_NONE)
            {
                /* No source placed in this chain → TRUE silence. Do NOT run the
                 * pipeline on a synthetic frame: with inversion ON a black frame
                 * comes out as ALL notes at max volume (wall of sound), and no
                 * uniform frame is silent for every inversion/AC-removal combo.
                 * Zeroed notes/grayscale/contrast are silent unconditionally. */
                static PreprocessedImageData s_preprocessed_silence; /* stays zeroed */
                struct timeval tv_silence;
                gettimeofday(&tv_silence, NULL);
                s_preprocessed_silence.timestamp_us =
                    (uint64_t)tv_silence.tv_sec * 1000000ULL + (uint64_t)tv_silence.tv_usec;

                pthread_mutex_lock(&s_luxstral_b_db.mutex);
                s_luxstral_b_db.preprocessed_data = s_preprocessed_silence;
                s_luxstral_b_db.dataReady = 1;
                pthread_mutex_unlock(&s_luxstral_b_db.mutex);
            }
            else if (spLB->present)
            {
                /* M9: base frame = engine B's own chain source (live or internal). */
                const uint8_t *baseB_R, *baseB_G, *baseB_B;
                synth_source_base(spLB, CHAIN_SYNTH_LUXSTRAL_B, db, nb_pixels,
                                  &baseB_R, &baseB_G, &baseB_B);

                const uint8_t *bxR, *bxG, *bxB;
                if (spLB->has_sampler && mod_R)
                {
                    bxR = mod_R; bxG = mod_G; bxB = mod_B;   /* modulated/sampler channel */
                }
                else if (spLB->num_inserts > 0)
                {
                    void *states[CHAIN_PLAN_MAX_INSERTS];
                    chain_resolve_insert_states(spLB, states);
                    image_chain_run(baseB_R, baseB_G, baseB_B,
                                    nb_pixels, g_sp3ctra_config.num_octaves,
                                    spLB->insert_id, states, spLB->num_inserts,
                                    &bxR, &bxG, &bxB);
                }
                else
                {
                    bxR = baseB_R; bxG = baseB_G; bxB = baseB_B;
                }

                /* Engine B's OWN pipeline config (M8): its inversion/AC/gamma/
                 * contrast/stereo knobs + its own freeze-envelope state — fully
                 * decoupled from engine A's settings (live_cfg above). */
                PipelineConfig cfg_b = pipeline_build_config_luxstral_b();
                if (pipeline_process_frame(bxR, bxG, bxB, &cfg_b, &s_preprocessed_temp_b) == 0)
                {
                    pthread_mutex_lock(&s_luxstral_b_db.mutex);
                    s_luxstral_b_db.preprocessed_data = s_preprocessed_temp_b;
                    s_luxstral_b_db.dataReady = 1;
                    pthread_mutex_unlock(&s_luxstral_b_db.mutex);
                }
            }
        }
#endif

        /* ── Path B (LuxSynth + LuxWave) — fed by ITS OWN chain (M6 Phase 2) ──
         * Run only the inserts on LuxSynth's chain (its own per-chain Pitch/Mask
         * instances, in its order) over the live frame, so a processor placed
         * before LuxSynth affects ONLY LuxSynth — never LuxStral's modulated
         * channel above. For the default topology LuxSynth's chain has no inserts
         * → raw live, identical to before. LuxWave shares this Path-B input.
         * pipeline_process_frame() above already ran Path B on LuxStral's frame;
         * this call overrides polyphonic.* + the LuxWave wavetable with Chain B's
         * own signal (live_cfg.envelope_id == ENVELOPE_LIVE gates the Chain B
         * freeze envelope here, skipped on the sampler worker). */
#ifdef VST_MODE
        {
            const SynthChainPlan *spB = &frame_plan.synth[CHAIN_SYNTH_LUXSYNTH];

            /* M9: base frame = LuxSynth's own chain source (live or internal). */
            const uint8_t *bR, *bG, *bB;
            synth_source_base(spB, CHAIN_SYNTH_LUXSYNTH, db, nb_pixels,
                              &bR, &bG, &bB);

            if (spB->present && spB->num_inserts > 0)
            {
                void *states[CHAIN_PLAN_MAX_INSERTS];
                chain_resolve_insert_states(spB, states);
                image_chain_run(bR, bG, bB,
                                nb_pixels, g_sp3ctra_config.num_octaves,
                                spB->insert_id, states, spB->num_inserts,
                                &bR, &bG, &bB);
            }

            pipeline_path_luxsynth_luxwave(bR, bG, bB, &live_cfg, &preprocessed_temp);
        }
#endif
      }

      /* 🎵 LUXWAVE FIX: Pass grayscale image data to LuxWave synthesis thread
       * This connects the scanner data pipeline to LuxWave for audio generation
       * Note: LuxWave will convert RGB to grayscale internally, so we pass mixed_R
       */
      synth_luxwave_set_image_line(&g_luxwave_state, 
                                     mixed_R, 
                                     nb_pixels);

      /* Step 3: Update display buffers with MIXED RGB (fixes N&B display issue) */
      pthread_mutex_lock(&db->mutex);
      
      /* CRITICAL FIX: Copy mixed RGB to activeBuffer so display shows colors */
      memcpy(db->activeBuffer_R, mixed_R, nb_pixels);
      memcpy(db->activeBuffer_G, mixed_G, nb_pixels);
      memcpy(db->activeBuffer_B, mixed_B, nb_pixels);
      
      swapBuffers(db);
      updateLastValidImage(db);

      /* During LuxSampler playback, FramePlayerThread owns preprocessed_data.
       * Skipping the live update here prevents overwriting playback preprocessing
       * that FramePlayerThread already wrote for the current synthesis cycle.
       * synth_AudioProcess reads db->preprocessed_data directly for audio gen. */
#ifdef VST_MODE
      /* Source routing: allow live preprocessed data to flow based on
       * luxstral_source_type.
       * Source=S(0): UDP thread writes ONLY while recording (the RAW incoming
       *   stream must drive the synth during rec; FramePlayerThread is idle).
       *   During playback, FramePlayerThread is the sole writer.
       * Source=L(1): UDP thread always writes (live data).
       * Source=M(2): UDP thread always writes (live component of mix). */
      {
        int src = g_sp3ctra_config.luxstral_source_type;
        if (src == 1 /* IMAGE_SOURCE_LIVE */ ||
            src == 2 /* IMAGE_SOURCE_MIX  */ ||
            src == 3 /* IMAGE_SOURCE_LUXPITCH */ ||
            src == 4 /* IMAGE_SOURCE_LUXMASK */)
        {
          db->preprocessed_data = preprocessed_temp;
          db->dataReady = 1;
        }
        else if (src == 0 /* IMAGE_SOURCE_SAMPLER */)
        {
          /* Source=S routing — RAW pass-through whenever no slot is playing.
           *
           * Behaviour change: the sampler now propagates the live UDP stream
           * (preprocessed_temp) to db->preprocessed_data continuously while
           * idle / recording / STEP_LIVE, mirroring the visual snapshot
           * mirror added in LuxSampler::onFrameAssembled().  The previous
           * silence-injection branch (idle ⇒ zero additive/polyphonic) is
           * gone — silence is now driven exclusively by the consumer paths
           * (STEP_EMPTY / sampler transport STOP / FramePlayerThread end).
           *
           * Cases:
           *   • !is_playing() && is_passthrough()  → idle / REC / STEP_LIVE:
           *       write preprocessed_temp, tag=2 (sampler).
           *       (passthroughEnabled stays true during recording and during
           *        STEP_LIVE; it only goes false during PLAY or STEP_EMPTY.)
           *   • is_playing()                       → FramePlayerThread is the
           *       sole writer of preprocessed_data; do not touch.
           *   • !is_passthrough() && !is_playing() → STEP_EMPTY (or transient
           *       PLAY-pending state) — the sequencer / injectSilenceCmd will
           *       inject silence; do not overwrite preprocessed_data here. */
          if (!lux_sampler_is_playing() && lux_sampler_is_passthrough())
          {
            db->preprocessed_data = preprocessed_temp;
            db->dataReady = 2; /* tag=2: sampler slot — consumer gating intact */
          }
        }

      }
#else
        db->preprocessed_data = preprocessed_temp;
        db->dataReady = 1;
#endif

      /* FIX(routing): LuxSynth polyphonic independent write path.
       * The LuxStral routing block above is gated entirely by luxstral_source_type.
       * When LuxStral=S and lux_sampler_is_playing(), the ENTIRE block is skipped,
       * leaving polyphonic.* (LuxSynth input) frozen on its last written value.
       * This causes the LuxSynth "Synth Grey" visualisation to freeze whenever
       * the sampler is playing, regardless of LuxSynth's own source setting.
       *
       * Fix: always update polyphonic.* from preprocessed_temp when
       * luxsynth_source_type is LIVE or MIX.  This write is independent of
       * LuxStral's transport state and of lux_sampler_is_playing().
       * preprocessed_temp.polyphonic was already recomputed from LuxSynth's
       * designated source (live UDP / sampler / mix) by preprocess_luxsynth()
       * in the pipeline processing block above (Change A). */
#ifdef VST_MODE
      {
        int luxsynth_src = g_sp3ctra_config.luxsynth_source_type;
        if (luxsynth_src == 1 /* IMAGE_SOURCE_LIVE */ ||
            luxsynth_src == 2 /* IMAGE_SOURCE_MIX  */ ||
            luxsynth_src == 3 /* IMAGE_SOURCE_LUXPITCH */ ||
            luxsynth_src == 4 /* IMAGE_SOURCE_LUXMASK */)
        {
          db->preprocessed_data.polyphonic = preprocessed_temp.polyphonic;
          if (db->dataReady == 0)
            db->dataReady = 1; /* polyphonic data is now valid */
        }
      }
#endif
      pthread_cond_signal(&db->cond);
      pthread_mutex_unlock(&db->mutex);

      /* 🎨 DISPLAY FIX: Update global display buffers with MIXED RGB colors
       * This replaces the grayscale→RGB conversion in synth_luxstral.c
       */
      
      /* DEBUG: Pixel difference check - DISABLED (too verbose in production) */
      /*
      if (++diff_log_counter % 1000 == 0) {
        diff_count = 0;
        for (i = 0; i < nb_pixels; i++) {
          if (mixed_R[i] != db->activeBuffer_R[i]) diff_count++;
        }
        log_debug("UDP", "Pixels different: %d/%d (%.1f%%)",
                  diff_count, nb_pixels, (diff_count * 100.0f) / nb_pixels);
      }
      */
      
      luxstral_engine_displayable_lock();
      memcpy(luxstral_engine_displayable_R(), mixed_R, nb_pixels);
      memcpy(luxstral_engine_displayable_G(), mixed_G, nb_pixels);
      memcpy(luxstral_engine_displayable_B(), mixed_B, nb_pixels);
      luxstral_engine_displayable_unlock();

      /* Capture raw scanner data only when new UDP data arrives
       * Function handles runtime enable/disable internally
       */
      image_debug_capture_raw_scanner_line(db->processingBuffer_R, 
                                          db->processingBuffer_G, 
                                          db->processingBuffer_B);
    }
    
    // 🔧 CRITICAL FIX: Check ctx->running at END of loop iteration too
    // Heavy processing above (preprocessing, sequencer, display update) can take time
    // Check again before next recvfrom() to exit promptly
    if (!ctx->running) {
      log_info("THREAD", "ctx->running=0 detected at end of iteration, exiting");
      // 🔧 BUGFIX: Release write_mutex if held (prevents deadlock on restart)
      if (audio_write_started) {
        audio_image_buffers_complete_write(audioBuffers);
        audio_image_buffers_snapshot_raw(audioBuffers);
        audio_write_started = 0;
        log_info("THREAD", "Released write_mutex before exit (incomplete line)");
      }
      break;
    }
  }

  // 🔧 FINAL SAFETY: Release write_mutex if still held after normal loop exit
  // This handles the case where the while(ctx->running) condition fails
  if (audio_write_started) {
    audio_image_buffers_complete_write(audioBuffers);
        audio_image_buffers_snapshot_raw(audioBuffers);
    audio_write_started = 0;
    log_info("THREAD", "Released write_mutex at thread exit (safety cleanup)");
  }

  log_info("THREAD", "UDP thread terminating");

  // Free allocated buffers
  if (mixed_R) free(mixed_R);
  if (mixed_G) free(mixed_G);
  if (mixed_B) free(mixed_B);
  if (held_R) free(held_R);
  if (held_G) free(held_G);
  if (held_B) free(held_B);
  free(receivedFragments);

  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * M9 — internal source feeder tick
 *
 * Drives the per-synth chains from the IMAGE / VIDEO / CAMERA internal sources
 * while the SP3CTRA device is NOT streaming. When the device streams, the
 * per-line block in udpThread() substitutes the source frames itself (see
 * synth_source_base) and this function returns immediately — the 250 ms
 * hysteresis in internal_source_live_streaming() makes the hand-over race-free
 * in practice.
 *
 * Mirrors the per-synth routing of udpThread's completed-line block, minus the
 * device-only machinery (fragment reassembly, acquisition gate, sequencer mix,
 * sampler record hooks). Known v1 limitations, matching FramePlayerThread's
 * behaviour: no sampler recording of internal sources without a device stream,
 * and the shared modulated channel stays owned by the sampler/score.
 *
 * Runs on MediaSourceService (Non-RT JUCE thread), a few hundred Hz.
 * ═══════════════════════════════════════════════════════════════════════════ */
void internal_sources_process_tick(void *arg)
{
  Context *ctx = (Context *)arg;
  static PreprocessedImageData s_feeder_pp;     /* A + Path-B commit scratch */
#ifdef VST_MODE
  static PreprocessedImageData s_feeder_pp_b;   /* LuxStral engine B scratch */
#endif

  if (!ctx || !ctx->running || !ctx->doubleBuffer || !ctx->audioImageBuffers)
    return;
  if (!internal_source_any_active())
    return;
  if (internal_source_live_streaming())
    return;   /* device streams → udpThread owns the per-synth processing */

  DoubleBuffer      *db           = ctx->doubleBuffer;
  AudioImageBuffers *audioBuffers = ctx->audioImageBuffers;
  const int          nb_pixels    = get_cis_pixels_nb();

  ChainPlan frame_plan;
  chain_plan_get(&frame_plan);

  const SynthChainPlan *spA  = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL];
  const SynthChainPlan *spLB = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL_B];
  const SynthChainPlan *spS  = &frame_plan.synth[CHAIN_SYNTH_LUXSYNTH];
  const SynthChainPlan *spW  = &frame_plan.synth[CHAIN_SYNTH_LUXWAVE];

#ifdef VST_MODE
  const int sampler_playing = lux_sampler_is_playing();
#else
  const int sampler_playing = 0;
#endif

  /* ── LuxStral A — its own chain, only when fed by an internal source ────── */
  const uint8_t *baseA_R = NULL, *baseA_G = NULL, *baseA_B = NULL;
  int a_done = 0;
  if (spA->present && !sampler_playing
      && synth_source_base(spA, CHAIN_SYNTH_LUXSTRAL, db, nb_pixels,
                           &baseA_R, &baseA_G, &baseA_B))
  {
    const uint8_t *srcR = baseA_R, *srcG = baseA_G, *srcB = baseA_B;
    if (spA->num_inserts > 0)
    {
      void *states[CHAIN_PLAN_MAX_INSERTS];
      chain_resolve_insert_states(spA, states);
      image_chain_run(baseA_R, baseA_G, baseA_B,
                      nb_pixels, g_sp3ctra_config.num_octaves,
                      spA->insert_id, states, spA->num_inserts,
                      &srcR, &srcG, &srcB);
    }
    PipelineConfig cfg = pipeline_build_config_live();
    if (pipeline_process_frame(srcR, srcG, srcB, &cfg, &s_feeder_pp) == 0)
      a_done = 1;
  }

  /* ── Path B (LuxSynth + LuxWave) — LuxSynth's chain (LuxWave shares it, as
   * in udpThread); fall back to LuxWave's own chain when LuxSynth is absent. */
  const SynthChainPlan *spPB = spS->present ? spS
                             : (spW->present ? spW : NULL);
  const int pb_slot = (spPB == spW) ? CHAIN_SYNTH_LUXWAVE : CHAIN_SYNTH_LUXSYNTH;
  const uint8_t *pbR = NULL, *pbG = NULL, *pbB = NULL;
  int pb_done = 0;
  if (spPB && synth_source_base(spPB, pb_slot, db, nb_pixels, &pbR, &pbG, &pbB))
  {
    const uint8_t *sR = pbR, *sG = pbG, *sB = pbB;
    if (spPB->num_inserts > 0)
    {
      void *states[CHAIN_PLAN_MAX_INSERTS];
      chain_resolve_insert_states(spPB, states);
      image_chain_run(pbR, pbG, pbB,
                      nb_pixels, g_sp3ctra_config.num_octaves,
                      spPB->insert_id, states, spPB->num_inserts,
                      &sR, &sG, &sB);
    }
    PipelineConfig cfg = pipeline_build_config_live();
    pipeline_path_luxsynth_luxwave(sR, sG, sB, &cfg, &s_feeder_pp);
    synth_luxwave_set_image_line(&g_luxwave_state, sR, nb_pixels);
    pb_done = 1;
  }

#ifdef VST_MODE
  /* ── LuxStral engine B — its own chain, into the file-static DoubleBuffer ── */
  if (spLB->present && !(spLB->has_sampler && sampler_playing))
  {
    const uint8_t *baseB_R, *baseB_G, *baseB_B;
    if (synth_source_base(spLB, CHAIN_SYNTH_LUXSTRAL_B, db, nb_pixels,
                          &baseB_R, &baseB_G, &baseB_B))
    {
      if (!s_luxstral_b_db_ready)
      {
        if (initDoubleBuffer(&s_luxstral_b_db) != 0)
          log_error("THREAD", "Engine-B DoubleBuffer init failed — engine B stays inactive");
        else
          __atomic_store_n(&s_luxstral_b_db_ready, 1, __ATOMIC_RELEASE);
      }
      if (s_luxstral_b_db_ready)   /* skip if the DoubleBuffer init failed */
      {
        const uint8_t *bxR = baseB_R, *bxG = baseB_G, *bxB = baseB_B;
        if (spLB->num_inserts > 0)
        {
          void *states[CHAIN_PLAN_MAX_INSERTS];
          chain_resolve_insert_states(spLB, states);
          image_chain_run(baseB_R, baseB_G, baseB_B,
                          nb_pixels, g_sp3ctra_config.num_octaves,
                          spLB->insert_id, states, spLB->num_inserts,
                          &bxR, &bxG, &bxB);
        }
        PipelineConfig cfg_b = pipeline_build_config_luxstral_b();
        if (pipeline_process_frame(bxR, bxG, bxB, &cfg_b, &s_feeder_pp_b) == 0)
        {
          pthread_mutex_lock(&s_luxstral_b_db.mutex);
          s_luxstral_b_db.preprocessed_data = s_feeder_pp_b;
          s_luxstral_b_db.dataReady = 1;
          pthread_mutex_unlock(&s_luxstral_b_db.mutex);
        }
      }
    }
  }
#endif

  if (!a_done && !pb_done)
    return;

  /* ── Display / visual buses — mirror the device behaviour so the CIS
   * visualizer, waterfall and RAW snapshot follow the internal source. The
   * primary display line is the first internally-fed synth (A, then Path B). */
  const uint8_t *dispR = a_done ? baseA_R : pbR;
  const uint8_t *dispG = a_done ? baseA_G : pbG;
  const uint8_t *dispB = a_done ? baseA_B : pbB;

  if (!sampler_playing)
  {
    uint8_t *wR, *wG, *wB;
    if (audio_image_buffers_start_write(audioBuffers, &wR, &wG, &wB) == 0)
    {
      memcpy(wR, dispR, (size_t) nb_pixels);
      memcpy(wG, dispG, (size_t) nb_pixels);
      memcpy(wB, dispB, (size_t) nb_pixels);
      audio_image_buffers_snapshot_raw_before_swap(audioBuffers);
      audio_image_buffers_complete_write(audioBuffers);
    }
  }

  /* ── Commit: display buffers + preprocessed data (same lock scope and
   * source-routing gating as udpThread's per-line commit). */
  pthread_mutex_lock(&db->mutex);

  memcpy(db->activeBuffer_R, dispR, (size_t) nb_pixels);
  memcpy(db->activeBuffer_G, dispG, (size_t) nb_pixels);
  memcpy(db->activeBuffer_B, dispB, (size_t) nb_pixels);
  swapBuffers(db);
  updateLastValidImage(db);

  if (a_done)
  {
#ifdef VST_MODE
    const int srcA = g_sp3ctra_config.luxstral_source_type;
    if (srcA != 0 /* LIVE / MIX / PITCH / MASK routing */)
    {
      db->preprocessed_data = s_feeder_pp;
      db->dataReady = 1;
    }
    else if (!sampler_playing && lux_sampler_is_passthrough())
    {
      /* Source=S idle passthrough — same tag as udpThread (consumer gating). */
      db->preprocessed_data = s_feeder_pp;
      db->dataReady = 2;
    }
#else
    db->preprocessed_data = s_feeder_pp;
    db->dataReady = 1;
#endif
  }
  else if (pb_done)
  {
    /* Path B only: never clobber LuxStral A's sections with stale scratch. */
    db->preprocessed_data.polyphonic = s_feeder_pp.polyphonic;
    if (db->dataReady == 0)
      db->dataReady = 1;
  }

  pthread_cond_signal(&db->cond);
  pthread_mutex_unlock(&db->mutex);

  /* LuxStral waterfall display mirror (same as udpThread's Step 3 tail). */
  luxstral_engine_displayable_lock();
  memcpy(luxstral_engine_displayable_R(), dispR, (size_t) nb_pixels);
  memcpy(luxstral_engine_displayable_G(), dispG, (size_t) nb_pixels);
  memcpy(luxstral_engine_displayable_B(), dispB, (size_t) nb_pixels);
  luxstral_engine_displayable_unlock();
}

// REMOVED (DMX): void *dmxSendingThread(void *arg) {
// REMOVED (DMX):   DMXContext *dmxCtx = (DMXContext *)arg;
// REMOVED (DMX):   unsigned char frame[DMX_FRAME_SIZE];
// REMOVED (DMX): 
// REMOVED (DMX):   // Check if DMX file descriptor is valid
// REMOVED (DMX):   if (dmxCtx->fd < 0) {
// REMOVED (DMX):     log_error("THREAD", "DMX thread started with invalid file descriptor, exiting thread");
// REMOVED (DMX):     return NULL;
// REMOVED (DMX):   }
// REMOVED (DMX): 
// REMOVED (DMX):   while (dmxCtx->running && keepRunning) {
// REMOVED (DMX):     // Check if file descriptor is still valid
// REMOVED (DMX):     if (dmxCtx->fd < 0) {
// REMOVED (DMX):       log_error("THREAD", "DMX file descriptor became invalid, exiting thread");
// REMOVED (DMX):       break;
// REMOVED (DMX):     }
// REMOVED (DMX): 
// REMOVED (DMX):     // Check immediately if a stop signal has been received
// REMOVED (DMX):     if (!dmxCtx->running || !keepRunning) {
// REMOVED (DMX):       break;
// REMOVED (DMX):     }
// REMOVED (DMX): 
// REMOVED (DMX):     // Reset DMX frame and set start code
// REMOVED (DMX):     memset(frame, 0, DMX_FRAME_SIZE);
// REMOVED (DMX):     frame[0] = 0;
// REMOVED (DMX): 
// REMOVED (DMX):     // For each spot, insert channels based on spot type (RGB or RGBW)
// REMOVED (DMX):     for (int i = 0; i < dmxCtx->num_spots; i++) {
// REMOVED (DMX):       int base = dmxCtx->spots[i].start_channel;
// REMOVED (DMX):       
// REMOVED (DMX):       if (dmxCtx->spots[i].type == DMX_SPOT_RGB) {
// REMOVED (DMX):         // RGB: 3 channels
// REMOVED (DMX):         if ((base + 2) < DMX_FRAME_SIZE) {
// REMOVED (DMX):           frame[base + 0] = dmxCtx->spots[i].data.rgb.red;
// REMOVED (DMX):           frame[base + 1] = dmxCtx->spots[i].data.rgb.green;
// REMOVED (DMX):           frame[base + 2] = dmxCtx->spots[i].data.rgb.blue;
// REMOVED (DMX):         } else {
// REMOVED (DMX):           log_error("THREAD", "DMX address out of bounds for RGB spot %d", i);
// REMOVED (DMX):         }
// REMOVED (DMX):       } else if (dmxCtx->spots[i].type == DMX_SPOT_RGBW) {
// REMOVED (DMX):         // RGBW: 4 channels
// REMOVED (DMX):         if ((base + 3) < DMX_FRAME_SIZE) {
// REMOVED (DMX):           frame[base + 0] = dmxCtx->spots[i].data.rgbw.red;
// REMOVED (DMX):           frame[base + 1] = dmxCtx->spots[i].data.rgbw.green;
// REMOVED (DMX):           frame[base + 2] = dmxCtx->spots[i].data.rgbw.blue;
// REMOVED (DMX):           frame[base + 3] = dmxCtx->spots[i].data.rgbw.white;
// REMOVED (DMX):         } else {
// REMOVED (DMX):           log_error("THREAD", "DMX address out of bounds for RGBW spot %d", i);
// REMOVED (DMX):         }
// REMOVED (DMX):       }
// REMOVED (DMX):     }
// REMOVED (DMX): 
// REMOVED (DMX):     // Send DMX frame only if fd is valid and the
// REMOVED (DMX):     // application is still running
// REMOVED (DMX):     if (dmxCtx->running && keepRunning && dmxCtx->fd >= 0 &&
// REMOVED (DMX):         send_dmx_frame(dmxCtx->fd, frame, DMX_FRAME_SIZE) < 0) {
// REMOVED (DMX):       log_error("THREAD", "Error sending DMX frame: %s", strerror(errno));
// REMOVED (DMX):       // In case of repeated error, we can exit the thread
// REMOVED (DMX):       if (errno == EBADF || errno == EIO) {
// REMOVED (DMX):         log_error("THREAD", "Critical DMX error, exiting thread");
// REMOVED (DMX):         break;
// REMOVED (DMX):       }
// REMOVED (DMX):     }
// REMOVED (DMX): 
// REMOVED (DMX):     // Use an interruptible sleep that periodically checks if a stop signal
// REMOVED (DMX):     // has been received
// REMOVED (DMX):     for (int i = 0; i < 5; i++) { // 5 * 5ms = 25ms total
// REMOVED (DMX):       if (!dmxCtx->running || !keepRunning) {
// REMOVED (DMX):         break;
// REMOVED (DMX):       }
// REMOVED (DMX):       usleep(5000); // 5ms
// REMOVED (DMX):     }
// REMOVED (DMX):   }
// REMOVED (DMX): 
// REMOVED (DMX):   log_info("THREAD", "DMX thread terminating");
// REMOVED (DMX): 
// REMOVED (DMX):   // Fermer le descripteur de fichier seulement s'il est valide
// REMOVED (DMX):   if (dmxCtx->fd >= 0) {
// REMOVED (DMX):     close(dmxCtx->fd);
// REMOVED (DMX):     dmxCtx->fd = -1;
// REMOVED (DMX):   }
// REMOVED (DMX):   return NULL;
// REMOVED (DMX): }

void *audioProcessingThread(void *arg) {
  Context *context = (Context *)arg;
  AudioImageBuffers *audioBuffers = context->audioImageBuffers;

  // Local buffers for synth_AudioProcess - lock-free access!
  uint8_t *audio_read_R = NULL;
  uint8_t *audio_read_G = NULL;
  uint8_t *audio_read_B = NULL;

#ifdef VST_MODE
  log_info("THREAD", "Audio processing thread started with VST SYNCHRONIZATION");
  log_info("THREAD", "Producer/consumer handoff with processBlock() callback");
#else
  log_info("THREAD", "Audio processing thread started with lock-free dual buffer system");
  log_info("THREAD", "Real-time audio processing guaranteed - no timeouts, no blocking!");
#endif

  // OPTIMIZATION: Set real-time priority for audio processing thread
  // This ensures the thread gets CPU time even under system load
#ifdef __linux__
  struct sched_param param;
  param.sched_priority = 70; // Same priority as RtAudio callback
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
    log_info("THREAD", "Audio processing thread set to RT priority 70 (SCHED_FIFO)");
  } else {
    log_warning("THREAD", "Failed to set RT priority (may need CAP_SYS_NICE capability)");
  }
#endif

  // 🔧 VST FIX: Use audio_thread_running for audio thread control
  // This allows stopping ONLY the audio thread during buffer size changes
  // without affecting the UDP thread (which uses context->running)
#ifdef VST_MODE
  while (context->audio_thread_running) {
#else
  while (context->running) {
#endif
#ifdef VST_MODE
    // 🎯 VST SYNCHRONIZATION: Wait for processBlock() to consume the previous buffer
    // This ensures perfect producer/consumer handoff without buffer overwrites
    // The wait has a 200ms timeout to avoid deadlock when audio stops
    luxstral_wait_for_buffer_consumed();
    
    // Check if we should exit after waking up (use audio_thread_running, NOT running!)
    if (!context->audio_thread_running) break;
#endif

    // Get current read pointers atomically (no mutex, no blocking!)
    audio_image_buffers_get_read_pointers(audioBuffers, &audio_read_R,
                                          &audio_read_G, &audio_read_B);

    // Measure synthesis time for performance profiling
    struct timeval iteration_start, iteration_end;
    gettimeofday(&iteration_start, NULL);

    // Call synthesis routine directly with stable image data
    // This will NEVER block, even if scanner disconnects!
#ifdef VST_MODE
    /* M8 — dual-engine publish. When engine B is active, render A AND B and
     * publish them ATOMICALLY (adjacent index flips) via synth_AudioProcess_ab,
     * so the consumer never pairs A's new frame with B's stale one (the window
     * that duplicated/skipped B frames → robotic artefact). B renders in the SAME
     * iteration as A (paced by A's consumed-buffer handshake) and spins up lazily
     * on first render. Otherwise render engine A alone. */
    {
      static int s_engine_b_inited = 0;
      ChainPlan planB_render;
      chain_plan_get(&planB_render);
      const int bActive = planB_render.synth[CHAIN_SYNTH_LUXSTRAL_B].present
                          && __atomic_load_n(&s_luxstral_b_db_ready, __ATOMIC_ACQUIRE);
      if (bActive)
      {
        if (!s_engine_b_inited) { synth_luxstral_init_engine_b(); s_engine_b_inited = 1; }
        synth_AudioProcess_ab(audio_read_R, audio_read_G, audio_read_B,
                              context->doubleBuffer, &s_luxstral_b_db);
      }
      else
      {
        synth_AudioProcess(audio_read_R, audio_read_G, audio_read_B, context->doubleBuffer);
      }
    }
#else
    synth_AudioProcess(audio_read_R, audio_read_G, audio_read_B, context->doubleBuffer);
#endif

    // Report iteration time to profiler (VST mode only - uses extern profiler)
#ifdef VST_MODE
    gettimeofday(&iteration_end, NULL);
    int64_t sec_diff = (int64_t)(iteration_end.tv_sec - iteration_start.tv_sec);
    int64_t usec_diff = (int64_t)(iteration_end.tv_usec - iteration_start.tv_usec);
    uint64_t elapsed_us = (uint64_t)(sec_diff * 1000000LL + usec_diff);
    
    // Access VST's global profiler
    extern RTProfiler g_vst_rt_profiler;
    rt_profiler_report_audio_thread_iteration(&g_vst_rt_profiler, elapsed_us);
#endif

#ifndef VST_MODE
    // Standalone mode: Small sleep to prevent excessive CPU usage
    // VST mode uses condition variable synchronization instead
    usleep(100); // 0.1ms - much smaller than before
#endif
  }

  log_info("THREAD", "Audio processing thread terminated");
  return NULL;
}
