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
#include "../processing/lux_reverb.h"
#include "../processing/lux_echo.h"
#include "../processing/lux_eq.h"
#include "../processing/video_scroll.h"
#include "../processing/internal_source.h"
#include <time.h>
#include <sys/time.h>

/* VST synchronization function declaration (defined in vst_adapters.cpp) */
#ifdef VST_MODE
extern void luxstral_wait_for_buffer_consumed(void);
#include "../luxsampler/lux_sampler_hooks.h"
#endif

/* External sequencer instance */

/* ── M8: LuxStral engine B — independent input DoubleBuffer ──────────────────
 * Both the UDP thread (producer: fills preprocessed_data from engine B's OWN
 * chain) and audioProcessingThread (consumer: renders engine B) live in this
 * translation unit, so a file-static DoubleBuffer keeps the whole 2nd-voice
 * input path self-contained (no Context / Sp3ctraCore plumbing). Lazily
 * initialised on first use in the UDP thread. */
#include "../processing/synth_staging.h"
#include "../processing/image_pipeline_stages.h"   /* img_stage_blob_detect (P3 mixer) */

/* Synth-split P3 — per-thread scratch for ONE LuxStral send's conditioned
 * frame before staging (each producer thread has its own; the staging module
 * seqlocks the shared slots). */
static PreprocessedImageData s_ls_send_pp;         /* udpThread */
static PreprocessedImageData s_ls_send_pp_feeder;  /* feeder tick */
static int s_udp_frame_ls_sends = 0;   /* udpThread-only: plan.num_ls_sends of
                                        * the current line (commit-scope read) */

#ifdef VST_MODE
static DoubleBuffer          s_luxstral_b_db;
static PreprocessedImageData s_preprocessed_temp_b;   /* UDP-thread scratch (single writer) */
static int                   s_luxstral_b_db_ready = 0;

/* Lazy init shared by every engine-B producer (udpThread, feeder tick,
 * FramePlayerThread via luxstral_b_feed_player_frame). Returns 1 when the
 * DoubleBuffer is usable. */
static int luxstral_b_db_ensure_ready(void)
{
    if (__atomic_load_n(&s_luxstral_b_db_ready, __ATOMIC_ACQUIRE))
        return 1;
    if (initDoubleBuffer(&s_luxstral_b_db) != 0)
    {
        log_error("THREAD", "Engine-B DoubleBuffer init failed — engine B stays inactive");
        return 0;
    }
    /* RELEASE so the audio thread, on seeing ready=1 (ACQUIRE), also sees the
     * fully-initialised DoubleBuffer (mutex, buffers). */
    __atomic_store_n(&s_luxstral_b_db_ready, 1, __ATOMIC_RELEASE);
    return 1;
}

/* Commit TRUE silence into engine B's input. Zeroed notes/grayscale/contrast
 * are silent for every inversion/AC-removal combo — never run the pipeline on
 * a synthetic black/white frame instead (inversion ON turns a black frame
 * into ALL notes at max volume). */
static void luxstral_b_commit_silence(void)
{
    static PreprocessedImageData s_preprocessed_silence; /* stays zeroed */
    struct timeval tv_silence;
    if (!luxstral_b_db_ensure_ready())
        return;
    gettimeofday(&tv_silence, NULL);
    s_preprocessed_silence.timestamp_us =
        (uint64_t)tv_silence.tv_sec * 1000000ULL + (uint64_t)tv_silence.tv_usec;

    pthread_mutex_lock(&s_luxstral_b_db.mutex);
    s_luxstral_b_db.preprocessed_data = s_preprocessed_silence;
    s_luxstral_b_db.dataReady = 1;
    pthread_mutex_unlock(&s_luxstral_b_db.mutex);
}

/* Player-side execution of the inserts placed AFTER a SCORE/SAMPLER marker —
 * defined below chain_run_inserts_with_viz_tap (which it reuses). */
static int chain_apply_post_marker_inserts(const SynthChainPlan *sp,
                                           int marker_id,
                                           struct AudioImageBuffers *viz_bus,
                                           uint8_t *r, uint8_t *g, uint8_t *b,
                                           int nb_pixels);

/* ── Engine B ← sampler/score player feed (bug: B silent without A) ──────────
 * Called by FramePlayerThread (Non-RT) with the final blended playback frame.
 * The SCORE/SAMPLER players historically wrote ONLY engine A's DoubleBuffer,
 * so a [SCORE, LUXSTRAL B] chain never received a single frame and engine B
 * stayed silent unless some other producer (device stream + engine A's chain)
 * happened to run. This is the plan-driven feed that makes engine B's chain
 * self-sufficient:
 *   • is_score=1 → feed when a SCORE module sits upstream of LUXSTRAL B.
 *   • is_score=0 → feed when a SAMPLER sits upstream of LUXSTRAL B AND the
 *     device is NOT streaming (while it streams, udpThread already routes the
 *     modulated channel to B — two writers would fight).
 * force_play=1 forces the pipeline envelope to PLAY (sequencer/score driven),
 * mirroring the engine-A commit in FramePlayerThread. */
void luxstral_b_feed_player_frame(const uint8_t *r, const uint8_t *g,
                                  const uint8_t *b, int nb_pixels,
                                  int is_score, int force_play,
                                  struct AudioImageBuffers *viz_bus)
{
    static PreprocessedImageData s_pp_player; /* FramePlayerThread-only scratch */
    ChainPlan plan;
    chain_plan_get(&plan);
    const SynthChainPlan *spLB = &plan.synth[CHAIN_SYNTH_LUXSTRAL_B];

    if (!spLB->present)
        return;
    if (is_score ? !spLB->has_score
                 : (!spLB->has_sampler || internal_source_live_streaming()))
        return;
    if (!luxstral_b_db_ensure_ready())
        return;

    /* Apply the inserts of B's chain placed BELOW the score/sampler module
     * (REVERB/ECHO/probes) to the playback frame — the per-line producers skip
     * this chain while the player owns it, so this is their only execution. */
    static uint8_t s_b_fx_r[8192], s_b_fx_g[8192], s_b_fx_b[8192];
    int nb = nb_pixels;
    if (nb > (int)sizeof(s_b_fx_r)) nb = (int)sizeof(s_b_fx_r);
    memcpy(s_b_fx_r, r, (size_t)nb);
    memcpy(s_b_fx_g, g, (size_t)nb);
    memcpy(s_b_fx_b, b, (size_t)nb);
    const int tap_done = chain_apply_post_marker_inserts(
        spLB,
        is_score ? IMAGE_CHAIN_INSERT_SCORE : IMAGE_CHAIN_INSERT_SAMPLER,
        viz_bus, s_b_fx_r, s_b_fx_g, s_b_fx_b, nb);

    /* Contextual zone 1: while the player owns B's input, udpThread skips this
     * chain — publish the selection tap here. A tap at/above the marker shows
     * the RAW player frame; below it, chain_apply_post_marker_inserts already
     * published the stream at the selected module's position. */
    if (!tap_done && spLB->viz_tap_insert >= 0 && viz_bus != NULL)
        audio_image_buffers_publish_selection_tap(viz_bus, r, g, b, nb_pixels);

    PipelineConfig cfg_b = pipeline_build_config_luxstral_b();
    if (force_play)
        cfg_b.freeze_mode = 0; /* PLAY — sequencer/score drives the transport */
    if (pipeline_process_frame(s_b_fx_r, s_b_fx_g, s_b_fx_b, &cfg_b, &s_pp_player) == 0)
    {
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        s_pp_player.timestamp_us =
            (uint64_t)tv_now.tv_sec * 1000000ULL + (uint64_t)tv_now.tv_usec;
        pthread_mutex_lock(&s_luxstral_b_db.mutex);
        s_luxstral_b_db.preprocessed_data = s_pp_player;
        s_luxstral_b_db.dataReady = 1;
        pthread_mutex_unlock(&s_luxstral_b_db.mutex);
    }
}

/* ── Synth-split P3 — FramePlayerThread: stage every PLAYER-OWNED LuxStral
 * send from the blended playback frame. A send is player-owned when its chain
 * hosts the SAMPLER (the caller only runs while a slot plays), or the SCORE
 * during score playback. Each send applies ITS OWN chain's post-marker
 * inserts on a private copy (FX must not leak between chains) and its own
 * conditioning bank; intensity is applied at MIX time by the audio thread.
 * Returns plan.num_ls_sends so the caller keeps the legacy engine-A/B paths
 * alive when no send exists. */
int ls_sends_stage_player_frame(const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels,
                                int is_score, int force_play,
                                struct AudioImageBuffers *viz_bus)
{
    static PreprocessedImageData s_pp_send_player; /* FramePlayerThread scratch */
    static uint8_t s_snd_fx_r[8192], s_snd_fx_g[8192], s_snd_fx_b[8192];

    ChainPlan plan;
    chain_plan_get(&plan);
    if (plan.num_ls_sends <= 0)
        return 0;

    int first_owned = 1;
    for (int k = 0; k < plan.num_ls_sends; k++)
    {
        const LsSendPlan     *snd = &plan.ls_send[k];
        const SynthChainPlan *sp  = &snd->recipe;

        if (! (is_score ? sp->has_score : sp->has_sampler))
            continue;   /* live/internal send — udpThread/feeder stage it */

        int nb = nb_pixels;
        if (nb > (int) sizeof(s_snd_fx_r)) nb = (int) sizeof(s_snd_fx_r);
        memcpy(s_snd_fx_r, r, (size_t) nb);
        memcpy(s_snd_fx_g, g, (size_t) nb);
        memcpy(s_snd_fx_b, b, (size_t) nb);

        const int tap_done = chain_apply_post_marker_inserts(
            sp,
            is_score ? IMAGE_CHAIN_INSERT_SCORE : IMAGE_CHAIN_INSERT_SAMPLER,
            viz_bus, s_snd_fx_r, s_snd_fx_g, s_snd_fx_b, nb);
        if (!tap_done && sp->viz_tap_insert >= 0 && viz_bus != NULL)
            audio_image_buffers_publish_selection_tap(viz_bus, r, g, b,
                                                      nb_pixels);

        PipelineConfig scfg = pipeline_build_config_ls_send(
            snd->bank_slot, snd->chain_idx, /*player_fed*/ 1);
        if (force_play)
            scfg.freeze_mode = 0; /* PLAY — sequencer/score drives transport */
        pipeline_path_luxstral(s_snd_fx_r, s_snd_fx_g, s_snd_fx_b,
                               &scfg, &s_pp_send_player);
        int nnotes = nb / (scfg.pixels_per_note > 0 ? scfg.pixels_per_note : 1);
        if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
        synth_staging_stage_luxstral(snd->chain_idx, snd->bank_slot,
                                     &s_pp_send_player, nnotes,
                                     scfg.stereo_enabled);

        if (first_owned)
        {
            first_owned = 0;
            if (viz_bus != NULL)
                audio_image_buffers_publish_engine_input(
                    viz_bus, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                    s_snd_fx_r, s_snd_fx_g, s_snd_fx_b, nb);
        }
    }
    return plan.num_ls_sends;
}

/* Player (sampler/score) stopped: silence engine B's input when its chain was
 * player-fed. Plan-gated so unrelated topologies are a no-op. Covers the
 * offline case where no per-line producer would overwrite the last frame. */
void luxstral_b_player_stopped(void)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    const SynthChainPlan *spLB = &plan.synth[CHAIN_SYNTH_LUXSTRAL_B];
    if (spLB->present && (spLB->has_score || spLB->has_sampler))
        luxstral_b_commit_silence();
}

/* UI (message thread): copy engine B's CURRENT preprocessed additive grayscale
 * — the REAL data engine B synthesises from, its own pipeline (inversion /
 * gamma / decode) already applied. Feeds the LUXSTRAL B zone-1 view without
 * any UI-side re-simulation. Returns the pixel count copied, 0 when engine
 * B's input buffer has not been initialised yet. */
int luxstral_b_copy_preprocessed_gray(uint8_t *gray_out, int max_pixels)
{
    if (gray_out == NULL || max_pixels <= 0)
        return 0;
    if (!__atomic_load_n(&s_luxstral_b_db_ready, __ATOMIC_ACQUIRE))
        return 0;

    int n = get_cis_pixels_nb();
    if (n > max_pixels)
        n = max_pixels;

    pthread_mutex_lock(&s_luxstral_b_db.mutex);
    const float *gsrc = s_luxstral_b_db.preprocessed_data.additive.grayscale;
    for (int i = 0; i < n; i++)
    {
        float v = gsrc[i];
        if (v < 0.0f) v = 0.0f;
        else if (v > 1.0f) v = 1.0f;
        gray_out[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    pthread_mutex_unlock(&s_luxstral_b_db.mutex);
    return n;
}
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
            case IMAGE_CHAIN_INSERT_LUXREVERB:
                states[i] = (void *)lux_reverb_instance(sp->insert_state_idx[i]); break;
            case IMAGE_CHAIN_INSERT_LUXECHO:
                states[i] = (void *)lux_echo_instance(sp->insert_state_idx[i]); break;
            case IMAGE_CHAIN_INSERT_LUXEQ:
                states[i] = (void *)lux_eq_instance(sp->insert_state_idx[i]); break;
            case IMAGE_CHAIN_INSERT_VIDEOSCROLL:
                states[i] = (void *)video_scroll_instance(sp->insert_state_idx[i]); break;
            default:
                states[i] = NULL; break;
        }
    }
}

/* ── M9: per-synth base frame — the chain's SOURCE module output ─────────────
 * LIVE (or sampler/score-fed) → the shared live frame (db->activeBuffer).
 * IMAGE / VIDEO / CAMERA → the latest line published by that source's engine,
 * copied into a per-synth scratch so the pointers stay stable for the rest of
 * the frame.
 *
 * Return codes:
 *    1 = internal source line copied (out_* point into the scratch)
 *    0 = live frame (out_* point into db->activeBuffer)
 *   -1 = NO SIGNAL — no source module in the chain (CHAIN_SRC_NONE) or an
 *        internal source module that is inactive / has published nothing.
 *        out_* still fall back to the live frame so legacy pointer users stay
 *        valid, but ROUTING callers must treat -1 as silence: a chain must
 *        never leak the live device feed by default (only an explicit SP3CTRA
 *        source, or a sampler/score upstream, may carry signal).
 *
 * The scratch is written by whichever thread currently drives the per-synth
 * processing (udpThread while the device streams, the feeder tick otherwise —
 * see internal_source_live_streaming()). The 250 ms hand-over hysteresis makes
 * concurrent writes to the same synth slot practically impossible; a glitched
 * frame at the boundary is acceptable.
 *
 * Slots CHAIN_SYNTH_COUNT..CHAIN_SYNTH_COUNT+CHAIN_MAX_CHAINS-1 belong to the
 * probe-only chains (plan.probe_chain[i] → slot CHAIN_SYNTH_COUNT + i). */
/* Slots: [0..3] synth engines, [4..11] probe chains, [12..19] P3 LuxStral
 * sends (CHAIN_SYNTH_COUNT + CHAIN_MAX_CHAINS + chain_idx). */
static uint8_t s_synth_src_scratch[CHAIN_SYNTH_COUNT + 2 * CHAIN_MAX_CHAINS][3][INTERNAL_SRC_MAX_PIXELS];

static int synth_source_base(const SynthChainPlan *sp, int synth_slot,
                             DoubleBuffer *db, int nb_pixels,
                             const uint8_t **out_r, const uint8_t **out_g,
                             const uint8_t **out_b)
{
    const int kind = internal_source_kind_for_chain_src(sp->source_kind);
    if (kind >= 0)
    {
        /* Defensive clamp — every caller passes a slot < the scratch dim
         * (engines 0..3, probes 4..11, P3 sends 12..19). */
        if (synth_slot < 0
            || synth_slot >= CHAIN_SYNTH_COUNT + 2 * CHAIN_MAX_CHAINS)
            synth_slot = 0;
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
    /* Internal module placed but empty/inactive, or no source module at all →
     * the chain has no signal of its own. */
    if (kind >= 0 || sp->source_kind == CHAIN_SRC_NONE)
        return -1;
    return 0;
}

/* True when a synth chain carries NO signal: its base source resolved to
 * "no signal" AND nothing upstream (sampler/score player) substitutes one. */
static int synth_chain_has_no_signal(const SynthChainPlan *sp, int base_sig)
{
    return sp->present && base_sig < 0 && !sp->has_sampler && !sp->has_score;
}

/* ── Contextual visualizer (zone 1) — selection tap ──────────────────────────
 * Run a chain's ordered inserts and, when the plan carries the selection tap
 * (viz_tap_insert >= 0 — the SELECTED module lives in this chain), publish the
 * stream frame AT that position to the selection-tap bus. The run is split at
 * the tap point; the final output is identical to a plain image_chain_run. */
static void chain_run_inserts_with_viz_tap(const SynthChainPlan *sp,
                                           AudioImageBuffers *audioBuffers,
                                           const uint8_t *inR, const uint8_t *inG,
                                           const uint8_t *inB, int nb_pixels,
                                           const uint8_t **outR, const uint8_t **outG,
                                           const uint8_t **outB)
{
    void *states[CHAIN_PLAN_MAX_INSERTS];
    chain_resolve_insert_states(sp, states);

    int tap = sp->viz_tap_insert;
    if (tap > sp->num_inserts)
        tap = sp->num_inserts;

    if (tap < 0 || audioBuffers == NULL)
    {
        image_chain_run(inR, inG, inB, nb_pixels, g_sp3ctra_config.num_octaves,
                        sp->insert_id, states, sp->num_inserts, outR, outG, outB);
        return;
    }

    const uint8_t *mr = inR, *mg = inG, *mb = inB;
    if (tap > 0)
        image_chain_run(inR, inG, inB, nb_pixels, g_sp3ctra_config.num_octaves,
                        sp->insert_id, states, tap, &mr, &mg, &mb);
    audio_image_buffers_publish_selection_tap(audioBuffers, mr, mg, mb, nb_pixels);
    if (tap < sp->num_inserts)
        image_chain_run(mr, mg, mb, nb_pixels, g_sp3ctra_config.num_octaves,
                        sp->insert_id + tap, (void *const *)(states + tap),
                        sp->num_inserts - tap, outR, outG, outB);
    else
    {
        *outR = mr; *outG = mg; *outB = mb;
    }
}

/* ── Player-side inserts (FramePlayerThread, Non-RT) ─────────────────────────
 * Apply the inserts of `sp` placed AFTER the LAST `marker_id` (SCORE/SAMPLER
 * position marker) to one playback frame, IN PLACE. While a player owns a
 * chain's stream, the per-line producers (udpThread/feeder) skip these
 * inserts, so this is their only execution — no double-run, and the pool
 * instances are only ever touched by one thread at a time.
 *
 * Publishes the zone-1 selection tap when it points INTO the post-marker span
 * (the producers' shortcut publishes the pre-marker taps). Returns 1 in that
 * case, 0 otherwise (including marker absent → frame untouched). */
static int chain_apply_post_marker_inserts(const SynthChainPlan *sp,
                                           int marker_id,
                                           struct AudioImageBuffers *viz_bus,
                                           uint8_t *r, uint8_t *g, uint8_t *b,
                                           int nb_pixels)
{
    int mk = -1;
    for (int i = 0; i < sp->num_inserts; i++)
        if (sp->insert_id[i] == marker_id)
            mk = i;                       /* LAST occurrence */
    if (mk < 0)
        return 0;

    /* Sub-plan = the inserts below the marker, tap index rebased onto it. */
    SynthChainPlan sub = *sp;
    sub.num_inserts = 0;
    for (int i = mk + 1; i < sp->num_inserts; i++)
    {
        sub.insert_id[sub.num_inserts]        = sp->insert_id[i];
        sub.insert_state_idx[sub.num_inserts] = sp->insert_state_idx[i];
        sub.num_inserts++;
    }
    const int tap_here = (sp->viz_tap_insert > mk);
    sub.viz_tap_insert = tap_here ? sp->viz_tap_insert - (mk + 1) : -1;

    const uint8_t *oR, *oG, *oB;
    chain_run_inserts_with_viz_tap(&sub, tap_here ? viz_bus : NULL,
                                   r, g, b, nb_pixels, &oR, &oG, &oB);
    if (oR != r) memcpy(r, oR, (size_t)nb_pixels);
    if (oG != g) memcpy(g, oG, (size_t)nb_pixels);
    if (oB != b) memcpy(b, oB, (size_t)nb_pixels);
    return tap_here;
}

/* Engine A's entry point for the FramePlayerThread (LuxSampler.cpp): apply the
 * inserts of LuxStral A's chain placed below the SCORE (is_score=1) or SAMPLER
 * (is_score=0) module to the final blended playback frame, in place. */
void chain_player_apply_synth_a_inserts(int is_score,
                                        struct AudioImageBuffers *viz_bus,
                                        uint8_t *r, uint8_t *g, uint8_t *b,
                                        int nb_pixels)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    const SynthChainPlan *spA = &plan.synth[CHAIN_SYNTH_LUXSTRAL];
    if (!spA->present)
        return;
    if (is_score ? !spA->has_score : !spA->has_sampler)
        return;
    chain_apply_post_marker_inserts(spA,
                                    is_score ? IMAGE_CHAIN_INSERT_SCORE
                                             : IMAGE_CHAIN_INSERT_SAMPLER,
                                    viz_bus, r, g, b, nb_pixels);
}

/* Selection tap for the SAMPLER/SCORE SHORT-CIRCUIT branches (no
 * image_chain_run): approximate the selected module's position as "before or
 * after the player marker" — the chain source frame before, the modulated
 * channel after. While the player is RUNNING, the post-marker tap is published
 * by the player thread at the exact position (chain_apply_post_marker_inserts)
 * — skip it here to avoid a second, pre-FX publication. Same for the
 * PRE-marker span when the modulated build already ran the chain's upstream
 * processors and published the exact tap (premarker_tap_done). */
static void publish_viz_tap_sampler_shortcut(const SynthChainPlan *sp,
                                             AudioImageBuffers *audioBuffers,
                                             int player_running,
                                             int premarker_tap_done,
                                             const uint8_t *baseR, const uint8_t *baseG,
                                             const uint8_t *baseB,
                                             const uint8_t *modR, const uint8_t *modG,
                                             const uint8_t *modB, int nb_pixels)
{
    if (sp->viz_tap_insert < 0 || audioBuffers == NULL)
        return;
    int after_marker = 0;
    for (int i = 0; i < sp->viz_tap_insert && i < sp->num_inserts; i++)
        if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
            || sp->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
            after_marker = 1;
    if (after_marker && player_running)
        return;
    if (!after_marker && premarker_tap_done)
        return;   /* exact tap already published by the modulated build */
    audio_image_buffers_publish_selection_tap(audioBuffers,
                                              after_marker ? modR : baseR,
                                              after_marker ? modG : baseG,
                                              after_marker ? modB : baseB,
                                              nb_pixels);
}

/* ── Sampler chain: PRE-MARKER processor sub-plan ────────────────────────────
 * Every processor insert (Pitch/Mask + FX) placed ABOVE the first SAMPLER
 * marker — probes and markers excluded (fed by the per-position loops). The
 * zone-1 selection tap is rebased onto the kept processors when it points
 * into the pre-marker span. Only num_inserts / insert_id / insert_state_idx /
 * viz_tap_insert of `pre` are filled (all chain_run_inserts_with_viz_tap
 * reads). Returns 1 when the sub-plan needs the chain-specific run (any FX,
 * or a Pitch/Mask bound to a pool slot other than 0) — udpThread keeps the
 * legacy image_chain_process_inserts path otherwise. */
static int chain_build_sampler_premarker_plan(const SynthChainPlan *spSmp,
                                              SynthChainPlan *pre)
{
    int chain_specific = 0;
    pre->num_inserts    = 0;
    pre->viz_tap_insert = -1;

    int mk = spSmp->num_inserts;   /* first SAMPLER marker */
    for (int i = 0; i < spSmp->num_inserts; i++)
        if (spSmp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER)
        { mk = i; break; }
    for (int i = 0; i < mk; i++)
    {
        const int id    = spSmp->insert_id[i];
        const int is_pm = (id == IMAGE_CHAIN_INSERT_LUXPITCH ||
                           id == IMAGE_CHAIN_INSERT_LUXMASK);
        const int is_fx = (id == IMAGE_CHAIN_INSERT_LUXREVERB ||
                           id == IMAGE_CHAIN_INSERT_LUXECHO ||
                           id == IMAGE_CHAIN_INSERT_LUXEQ);
        if (!is_pm && !is_fx)
            continue;   /* probes/markers: fed elsewhere */
        pre->insert_id[pre->num_inserts]        = id;
        pre->insert_state_idx[pre->num_inserts] = spSmp->insert_state_idx[i];
        pre->num_inserts++;
        if (is_fx || spSmp->insert_state_idx[i] != 0)
            chain_specific = 1;
    }
    /* Selection tap inside the pre-marker span → rebase it onto the kept
     * processors (exact contextual tap, replaces the shortcut's raw-frame
     * approximation). */
    if (spSmp->viz_tap_insert >= 0 && spSmp->viz_tap_insert <= mk)
    {
        int t = 0;
        for (int i = 0; i < spSmp->viz_tap_insert; i++)
        {
            const int id = spSmp->insert_id[i];
            if (id == IMAGE_CHAIN_INSERT_LUXPITCH  ||
                id == IMAGE_CHAIN_INSERT_LUXMASK   ||
                id == IMAGE_CHAIN_INSERT_LUXREVERB ||
                id == IMAGE_CHAIN_INSERT_LUXECHO   ||
                id == IMAGE_CHAIN_INSERT_LUXEQ)
                t++;
        }
        pre->viz_tap_insert = t;
    }
    return chain_specific;
}

#ifdef VST_MODE
/* Feeder-side mirror of udpThread's player-chain short-circuit: the chain
 * holding the sampler consumes the pre-marker stream built by the feeder's
 * sampler block (mod) instead of re-running its inserts — stateful FX
 * (reverb/echo) must tick exactly once per line. Feeds the chain's
 * VideoScroll probes by their position relative to the marker and publishes
 * the zone-1 selection tap. Post-marker processors only run in the player
 * thread (playback), as on the device path. */
static void feeder_sampler_chain_shortcut(const SynthChainPlan *sp,
                                          AudioImageBuffers *audioBuffers,
                                          int premarker_tap_done,
                                          const uint8_t *baseR, const uint8_t *baseG,
                                          const uint8_t *baseB,
                                          const uint8_t *modR, const uint8_t *modG,
                                          const uint8_t *modB, int nb_pixels)
{
    int after_marker = 0;
    for (int i = 0; i < sp->num_inserts; i++)
    {
        if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
            || sp->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
        { after_marker = 1; continue; }
        if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
            video_scroll_capture_line(
                video_scroll_instance(sp->insert_state_idx[i]),
                after_marker ? modR : baseR,
                after_marker ? modG : baseG,
                after_marker ? modB : baseB,
                nb_pixels);
    }
    publish_viz_tap_sampler_shortcut(sp, audioBuffers, /*player_running*/ 0,
                                     premarker_tap_done,
                                     baseR, baseG, baseB,
                                     modR, modG, modB, nb_pixels);
}
#endif

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

  /* Allocate acquisition-gate hold buffers */
  held_R = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  held_G = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  held_B = (uint8_t *)malloc(nb_pixels * sizeof(uint8_t));
  if (!held_R || !held_G || !held_B) {
    log_error("THREAD", "Failed to allocate acquisition-gate hold buffers");
    if (held_R) free(held_R);
    if (held_G) free(held_G);
    if (held_B) free(held_B);
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

      /* (Legacy ImageSequencer removed: g_image_sequencer was always NULL —
       * the passthrough round-trip copied activeBuffer→mixed→activeBuffer,
       * 6 full-line memcpy per UDP line for an identity. The display buses
       * below read db->activeBuffer directly now.) */

      /* Preprocess via pipeline — channel routing selects either
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

        /* ── ONE ChainPlan snapshot per frame ─────────────────────────────────
         * Taken BEFORE the modulated build: need_modulated below must also see
         * the plan (a sampler on LuxStral B's chain needs the modulated frame
         * even when engine A's routing does not), and every per-synth input
         * selection further down must share the SAME topology — a fresh
         * snapshot per consumer could mix two plans within one frame when a
         * rack edit is published mid-frame. */
        ChainPlan frame_plan;
        chain_plan_get(&frame_plan);
        s_udp_frame_ls_sends = frame_plan.num_ls_sends;   /* P3 — read by the
                                     * commit block after this scope closes */

        /* ── Build the MODULATED frame once — used by any path that selects it.
         * The inserts run inside their own preallocated buffers (see lux_pitch.c
         * / lux_mask.c, no allocation in this hot path).  When disabled or no
         * voice is active the engines short-circuit and return the input
         * pointers as-is, so the cost is O(1). */
        const uint8_t *mod_R = NULL;
        const uint8_t *mod_G = NULL;
        const uint8_t *mod_B = NULL;
        /* Set when the idle modulated build ran the sampler chain's pre-marker
         * processors AND published the exact selection tap — the shortcut
         * publishers below must then skip their raw-frame approximation. */
        int premarker_tap_done = 0;
        int            need_modulated =
            (live_cfg.luxstral_path.source         == IMAGE_SOURCE_MODULATED) ||
            (live_cfg.luxsynth_luxwave_path.source == IMAGE_SOURCE_MODULATED) ||
            image_chain_any_tap_demand() || /* a visualizer watches an insert tap */
            /* deriveChainRouting only routes engine A's global source — a
             * sampler sitting on LuxStral B's chain was invisible here, so B
             * played the LIVE feed instead of the sampler (and resampling
             * captured nothing). Engine A's clause is included for symmetry
             * (normally already covered by luxstral_path.source above). */
            (frame_plan.synth[CHAIN_SYNTH_LUXSTRAL].present
             && frame_plan.synth[CHAIN_SYNTH_LUXSTRAL].has_sampler) ||
            (frame_plan.synth[CHAIN_SYNTH_LUXSTRAL_B].present
             && frame_plan.synth[CHAIN_SYNTH_LUXSTRAL_B].has_sampler);
        /* P3 — a sampler on ANY LuxStral send's chain needs the modulated
         * channel (the legacy synth[A/B] clauses above only see the first
         * two sends). */
        for (int k = 0; k < frame_plan.num_ls_sends && !need_modulated; k++)
            if (frame_plan.ls_send[k].recipe.has_sampler)
                need_modulated = 1;

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
                 * record it.
                 *
                 * The chain that HOLDS the sampler owns the modulated channel.
                 * The sampler is a plain in→out module: its INPUT stream is
                 * the chain processed up to its marker — so EVERY processor
                 * insert placed ABOVE it (Pitch/Mask AND the FX: Reverb/Echo/
                 * EQ) runs here, shaping both the idle pass-through and what
                 * an armed slot records. Probes/markers are excluded (fed by
                 * the per-position loops — double-capture otherwise).
                 * image_chain_process_inserts() only knows the GLOBAL slot-0
                 * Pitch/Mask pair, so any FX — or a Pitch/Mask bound to
                 * another pool slot — forces the chain-specific run. Pool
                 * slot 0 Pitch/Mask-only chains keep the legacy path
                 * (identical behaviour + live per-insert visual taps). */
                const SynthChainPlan *spSmp = NULL;
                for (int s = 0; s < CHAIN_SYNTH_COUNT && !spSmp; s++)
                    if (frame_plan.synth[s].present && frame_plan.synth[s].has_sampler)
                        spSmp = &frame_plan.synth[s];

                SynthChainPlan pre;          /* pre-marker processor sub-plan */
                pre.num_inserts    = 0;
                pre.viz_tap_insert = -1;
                int chain_specific = 0;
                if (spSmp)
                    chain_specific = chain_build_sampler_premarker_plan(spSmp, &pre);

                if (chain_specific)
                {
                    chain_run_inserts_with_viz_tap(&pre, audioBuffers,
                                                   db->activeBuffer_R,
                                                   db->activeBuffer_G,
                                                   db->activeBuffer_B,
                                                   nb_pixels,
                                                   &mod_R, &mod_G, &mod_B);
                    premarker_tap_done = (pre.viz_tap_insert >= 0);
                }
                else
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


        /* ── Pick LuxStral's input (M6 Phase 2 — fed by ITS OWN chain) ────────
         * • A chain holding the Sampler IS the modulated channel (mod) — it
         *   carries the sampler playback frame + the recorded Pitch/Mask, so
         *   reuse mod (handles playing + idle). This keeps the sampler/recording
         *   path untouched.
         * • Otherwise run LuxStral's own ordered inserts (its per-chain Pitch/
         *   Mask instances) over the live frame, so LuxStral's processing is
         *   independent of every other chain. No inserts → raw live. */
        int a_no_signal = 0;   /* engine A's chain carries no signal this frame */
        {
            const SynthChainPlan *spA = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL];

            /* M9: base frame = the chain's SOURCE module output (live feed, or
             * the IMAGE/VIDEO/CAMERA internal source line when one is placed
             * and active in LuxStral's chain). */
            const uint8_t *baseA_R, *baseA_G, *baseA_B;
            const int baseSigA = synth_source_base(spA, CHAIN_SYNTH_LUXSTRAL,
                                                   db, nb_pixels,
                                                   &baseA_R, &baseA_G, &baseA_B);
            a_no_signal = synth_chain_has_no_signal(spA, baseSigA);

            /* Player transport (sampler OR score relay): while it runs, the
             * post-marker inserts/probes/tap are the PLAYER THREAD's job. */
#ifdef VST_MODE
            const int player_running_now = lux_sampler_is_playing();
            const int score_playing_now  = lux_sampler_is_score_playing();
#else
            const int player_running_now = 0;
            const int score_playing_now  = 0;
#endif

            if (frame_plan.num_ls_sends > 0)
            {
                /* Synth-split P3 — the SEND LOOP below owns every LuxStral
                 * chain (inserts, probes, taps, staging); the legacy A path
                 * only keeps the pipeline ticking on the base frame for the
                 * Path-B/polyphonic legacy commit. Its additive result is
                 * never committed (the audio-thread mixer owns db additive). */
                src_R = baseA_R;
                src_G = baseA_G;
                src_B = baseA_B;
                a_no_signal = 0;
            }
            else if (a_no_signal)
            {
                /* No source module in engine A's chain (or an empty internal
                 * source) → the chain has NO stream. Mirror of engine B's
                 * guard below: never fall back to the live device feed. The
                 * pipeline still runs (on the live frame) so Path B/state
                 * keep ticking, but A's sections of the result are zeroed
                 * after the run — see below. Inserts and probes are skipped:
                 * there is no stream at any position of this chain. */
                src_R = baseA_R;
                src_G = baseA_G;
                src_B = baseA_B;
            }
            else if (spA->present && mod_R
                     && (spA->has_sampler
                         || (spA->has_score && score_playing_now)))
            {
                src_R = mod_R; src_G = mod_G; src_B = mod_B;   /* modulated/player channel */
                /* This short-circuit skips image_chain_run (the player chain IS the
                 * modulated channel: SAMPLER always — idle included —, SCORE only
                 * while the relay actually plays). Feed VideoScroll probes by their
                 * POSITION relative to the marker: a probe ABOVE it shows the live
                 * source frame; a probe BELOW shows the modulated/player output.
                 * While the player RUNS, the post-marker inserts (FX + probes) are
                 * executed by the player thread on the playback frames
                 * (chain_apply_post_marker_inserts) — skip those probes here so
                 * each line is captured exactly once. The pre-marker processors
                 * (Pitch/Mask/FX above the sampler) run in the IDLE modulated
                 * build, which also publishes the exact pre-marker selection
                 * tap; while the player RUNS they are paused (the live input
                 * is not consumed) and the pre-marker tap falls back to the
                 * raw source frame. */
                int after_marker = 0;
                for (int i = 0; i < spA->num_inserts; i++)
                {
                    if (spA->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
                        || spA->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
                    { after_marker = 1; continue; }
                    if (spA->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                    {
                        if (after_marker && player_running_now)
                            continue;   /* captured by the player thread */
                        const uint8_t *fr = after_marker ? mod_R : baseA_R;
                        const uint8_t *fg = after_marker ? mod_G : baseA_G;
                        const uint8_t *fb = after_marker ? mod_B : baseA_B;
                        video_scroll_capture_line(
                            video_scroll_instance(spA->insert_state_idx[i]),
                            fr, fg, fb, nb_pixels);
                    }
                }
                publish_viz_tap_sampler_shortcut(spA, audioBuffers,
                                                 player_running_now,
                                                 premarker_tap_done,
                                                 baseA_R, baseA_G, baseA_B,
                                                 mod_R, mod_G, mod_B, nb_pixels);
            }
            else if (spA->present)
            {
                /* Ordered inserts (none → pass-through) + selection tap. */
                chain_run_inserts_with_viz_tap(spA, audioBuffers,
                                               baseA_R, baseA_G, baseA_B,
                                               nb_pixels, &src_R, &src_G, &src_B);
            }
            else
            {
                src_R = baseA_R;   /* engine A absent — legacy live frame */
                src_G = baseA_G;
                src_B = baseA_B;
            }
        }

        /* ── Probe-only chains (no synth): run each chain's ordered inserts so
         * every VideoScroll probe captures the stream AT ITS POSITION. The old
         * path fed probes the raw live frame unconditionally — a MASK placed
         * before the probe was never executed, so the probe kept scrolling the
         * live feed no matter what the chain contained. Output is discarded —
         * only the side effects (probe captures) matter. */
        for (int pc = 0; pc < frame_plan.num_probe_chains; pc++)
        {
            const SynthChainPlan *spP = &frame_plan.probe_chain[pc];
            const uint8_t *pcR, *pcG, *pcB;
            const int sigP = synth_source_base(spP, CHAIN_SYNTH_COUNT + pc, db,
                                               nb_pixels, &pcR, &pcG, &pcB);
            if (synth_chain_has_no_signal(spP, sigP))
                continue;   /* chain carries no stream → probes stay static */

#ifdef VST_MODE
            const int player_running_p = lux_sampler_is_playing();
            const int score_playing_p  = lux_sampler_is_score_playing();
#else
            const int player_running_p = 0;
            const int score_playing_p  = 0;
#endif
            if (mod_R && (spP->has_sampler
                          || (spP->has_score && score_playing_p)))
            {
                /* Mirror of the synth-chain player short-circuit: probes see
                 * the chain source pre-marker, the modulated channel after.
                 * (Probe-only chains have no synth, so no player thread runs
                 * their post-marker inserts — capture them here from mod.) */
                int after_marker_p = 0;
                for (int i = 0; i < spP->num_inserts; i++)
                {
                    if (spP->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
                        || spP->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
                    { after_marker_p = 1; continue; }
                    if (spP->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                        video_scroll_capture_line(
                            video_scroll_instance(spP->insert_state_idx[i]),
                            after_marker_p ? mod_R : pcR,
                            after_marker_p ? mod_G : pcG,
                            after_marker_p ? mod_B : pcB,
                            nb_pixels);
                }
                publish_viz_tap_sampler_shortcut(spP, audioBuffers,
                                                 /*player_running*/ 0,
                                                 /*premarker_tap_done*/ 0,
                                                 pcR, pcG, pcB,
                                                 mod_R, mod_G, mod_B, nb_pixels);
                (void)player_running_p;
            }
            else
            {
                const uint8_t *poR, *poG, *poB;
                chain_run_inserts_with_viz_tap(spP, audioBuffers, pcR, pcG, pcB,
                                               nb_pixels, &poR, &poG, &poB);
            }
        }

        /* ── Synth-split P3: LuxStral SENDS ─────────────────────────────────
         * Each "→ LUXSTRAL" chain is executed here (source → inserts → per-
         * send conditioning) and STAGED; the audio thread mixes every active
         * send into the single engine feed. Player-owned sends (sampler on
         * the chain — idle included —, or score while the relay plays) are
         * produced by FramePlayerThread instead. */
        for (int k = 0; k < frame_plan.num_ls_sends; k++)
        {
            const LsSendPlan     *snd = &frame_plan.ls_send[k];
            const SynthChainPlan *sp  = &snd->recipe;

#ifdef VST_MODE
            const int player_running_s = lux_sampler_is_playing();
            const int score_playing_s  = lux_sampler_is_score_playing();
#else
            const int player_running_s = 0;
            const int score_playing_s  = 0;
#endif
            if ((sp->has_sampler && player_running_s)
                || (sp->has_score && score_playing_s))
                continue;   /* staged by FramePlayerThread */

            const uint8_t *sbR, *sbG, *sbB;
            const int sig = synth_source_base(
                sp, CHAIN_SYNTH_COUNT + CHAIN_MAX_CHAINS + snd->chain_idx,
                db, nb_pixels, &sbR, &sbG, &sbB);
            if (synth_chain_has_no_signal(sp, sig))
            {
                synth_staging_set_inactive(snd->chain_idx);
                continue;   /* no stream → the send contributes silence */
            }

            const uint8_t *ssR = sbR, *ssG = sbG, *ssB = sbB;
            if (mod_R && (sp->has_sampler
                          || (sp->has_score && score_playing_s)))
            {
                /* Sampler chain, player idle: the modulated channel IS this
                 * chain's stream (mirror of the legacy A short-circuit —
                 * probes above the marker see the source, below see mod). */
                int after_marker = 0;
                for (int i = 0; i < sp->num_inserts; i++)
                {
                    if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
                        || sp->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
                    { after_marker = 1; continue; }
                    if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                        video_scroll_capture_line(
                            video_scroll_instance(sp->insert_state_idx[i]),
                            after_marker ? mod_R : sbR,
                            after_marker ? mod_G : sbG,
                            after_marker ? mod_B : sbB,
                            nb_pixels);
                }
                publish_viz_tap_sampler_shortcut(sp, audioBuffers,
                                                 player_running_s,
                                                 premarker_tap_done,
                                                 sbR, sbG, sbB,
                                                 mod_R, mod_G, mod_B, nb_pixels);
                ssR = mod_R; ssG = mod_G; ssB = mod_B;
            }
            else if (sp->num_inserts > 0 || sp->viz_tap_insert >= 0)
            {
                chain_run_inserts_with_viz_tap(sp, audioBuffers,
                                               sbR, sbG, sbB,
                                               nb_pixels, &ssR, &ssG, &ssB);
            }

            /* Per-send conditioning + staging (intensity applied at MIX). */
            {
                PipelineConfig scfg = pipeline_build_config_ls_send(
                    snd->bank_slot, snd->chain_idx, /*player_fed*/ 0);
                pipeline_path_luxstral(ssR, ssG, ssB, &scfg, &s_ls_send_pp);
                int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                          ? scfg.pixels_per_note : 1);
                if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
                synth_staging_stage_luxstral(snd->chain_idx, snd->bank_slot,
                                             &s_ls_send_pp, nnotes,
                                             scfg.stereo_enabled);
            }

            /* Head-panel engine tap A — published from the FIRST send (M1
             * approximation: the head panel shows one engine input line). */
            if (k == 0)
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                    ssR, ssG, ssB, nb_pixels);
        }

        if (pipeline_process_frame(src_R, src_G, src_B, &live_cfg, &preprocessed_temp) != 0) {
          log_error("THREAD", "Pipeline processing failed");
        }

        if (a_no_signal)
        {
          /* Engine A's chain has no source → TRUE silence (mirror of engine
           * B's zeroed-frame guard): zeroed notes/grayscale/contrast are
           * silent for every inversion/AC-removal combo, whereas running the
           * pipeline on a synthetic black frame is NOT (inversion ON turns it
           * into all-notes-at-max). Only A's sections are cleared — Path B
           * (polyphonic/photowave) is recomputed from its own chain below. */
          memset(&preprocessed_temp.additive,    0, sizeof(preprocessed_temp.additive));
          memset(&preprocessed_temp.stereo,      0, sizeof(preprocessed_temp.stereo));
          memset(&preprocessed_temp.strokeforge, 0, sizeof(preprocessed_temp.strokeforge));
        }

        /* Per-engine input tap A (per-chain display): published by the thread
         * that owns A's preprocessed commit this line — mirror of the
         * source-routing gating at the commit site below (src != 0 →
         * udpThread always commits; src == 0 → only the idle/REC
         * passthrough, a RUNNING player owns the commit AND the tap via
         * FramePlayerThread). White when A's chain carries no signal
         * (mirror of the zeroed sections above). */
        if (frame_plan.num_ls_sends == 0)   /* P3: the send loop owns tap A */
        {
#ifdef VST_MODE
          const int a_commit_here =
              (g_sp3ctra_config.luxstral_source_type != 0) ||
              (!lux_sampler_is_playing() && lux_sampler_is_passthrough());
#else
          const int a_commit_here = 1;
#endif
          if (a_commit_here)
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                a_no_signal ? NULL : src_R,
                a_no_signal ? NULL : src_G,
                a_no_signal ? NULL : src_B,
                nb_pixels);
        }

#ifdef VST_MODE
        /* ── LuxStral engine B (M8) — LEGACY (num_ls_sends == 0 only; the P3
         * send loop + mixer replace the whole B input path) ─────────────────
         * Computed HERE (before db->activeBuffer is overwritten with the display
         * mix below) using the live frame, mirroring engine A's src selection.
         * Committed to the file-static DoubleBuffer that audioProcessingThread
         * hands to synth_AudioProcess_b(). Independent scratch so A's not-yet-
         * committed preprocessed_temp is untouched. */
        if (frame_plan.num_ls_sends == 0)
        {
            const SynthChainPlan *spLB = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL_B];
            if (spLB->present && !luxstral_b_db_ensure_ready())
            {
                /* DoubleBuffer init failed — its mutex/buffers are not
                 * usable, skip engine-B feeding entirely. */
            }
            else if (spLB->present && spLB->has_score
                     && lux_sampler_is_score_playing())
            {
                /* SCORE playback overrides the chain source (relay semantics):
                 * FramePlayerThread feeds engine B's input directly via
                 * luxstral_b_feed_player_frame() — do not fight it here. */
            }
            else if (spLB->present && spLB->source_kind == CHAIN_SRC_NONE
                     && !spLB->has_sampler)
            {
                /* No source placed in this chain (and the SCORE, if any, is
                 * idle) → TRUE silence — UNLESS a sampler sits upstream:
                 * [SAMPLER, LUXSTRAL B] needs no source module (the sampler
                 * IS the signal), and the same topology plays on engine A. */
                luxstral_b_commit_silence();
            }
            else if (spLB->present)
            {
                /* M9: base frame = engine B's own chain source (live or internal). */
                const uint8_t *baseB_R, *baseB_G, *baseB_B;
                const int baseSigB =
                    synth_source_base(spLB, CHAIN_SYNTH_LUXSTRAL_B, db, nb_pixels,
                                      &baseB_R, &baseB_G, &baseB_B);

                const uint8_t *bxR = NULL, *bxG = NULL, *bxB = NULL;
                if (baseSigB < 0 && !spLB->has_sampler)
                {
                    /* Internal source module placed but empty/inactive → the
                     * chain has no signal; never leak the live device feed.
                     * bx stays NULL → the pipeline/commit below is skipped. */
                    luxstral_b_commit_silence();
                }
                else if (spLB->has_sampler && mod_R)
                {
                    bxR = mod_R; bxG = mod_G; bxB = mod_B;   /* modulated/sampler channel */
                    /* Feed VideoScroll probes by their position relative to the
                     * marker — mirror of engine A's short-circuit branch (the
                     * probes of a sampler chain on B stayed black without it).
                     * NOTE: this per-line branch only runs while the DEVICE
                     * streams, and B's player feed skips itself in that case
                     * (two writers would fight) — so no double capture here. */
                    int after_marker_b = 0;
                    for (int i = 0; i < spLB->num_inserts; i++)
                    {
                        if (spLB->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
                            || spLB->insert_id[i] == IMAGE_CHAIN_INSERT_SCORE)
                        { after_marker_b = 1; continue; }
                        if (spLB->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                        {
                            const uint8_t *fr = after_marker_b ? mod_R : baseB_R;
                            const uint8_t *fg = after_marker_b ? mod_G : baseB_G;
                            const uint8_t *fb = after_marker_b ? mod_B : baseB_B;
                            video_scroll_capture_line(
                                video_scroll_instance(spLB->insert_state_idx[i]),
                                fr, fg, fb, nb_pixels);
                        }
                    }
                    publish_viz_tap_sampler_shortcut(spLB, audioBuffers,
                                                     /*player_running*/ 0,
                                                     premarker_tap_done,
                                                     baseB_R, baseB_G, baseB_B,
                                                     mod_R, mod_G, mod_B, nb_pixels);
                }
                else
                {
                    /* Ordered inserts (none → pass-through) + selection tap. */
                    chain_run_inserts_with_viz_tap(spLB, audioBuffers,
                                                   baseB_R, baseB_G, baseB_B,
                                                   nb_pixels, &bxR, &bxG, &bxB);
                }

                /* Engine B's OWN pipeline config (M8): its inversion/AC/gamma/
                 * contrast/stereo knobs + its own freeze-envelope state — fully
                 * decoupled from engine A's settings (live_cfg above). */
                if (bxR != NULL)
                {
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
            /* LuxWave shares the Path-B input; when LuxSynth is absent honour
             * a placed LuxWave's OWN chain instead (mirrors the feeder tick). */
            const SynthChainPlan *spS2 = &frame_plan.synth[CHAIN_SYNTH_LUXSYNTH];
            const SynthChainPlan *spW2 = &frame_plan.synth[CHAIN_SYNTH_LUXWAVE];
            const SynthChainPlan *spB  = spS2->present ? spS2
                                       : (spW2->present ? spW2 : NULL);
            const int pbSlot = (spB == spW2) ? CHAIN_SYNTH_LUXWAVE
                                             : CHAIN_SYNTH_LUXSYNTH;

            /* M9: base frame = the Path-B chain's source (live or internal). */
            const uint8_t *bR = db->activeBuffer_R;
            const uint8_t *bG = db->activeBuffer_G;
            const uint8_t *bB = db->activeBuffer_B;
            const int baseSigPB = (spB != NULL)
                ? synth_source_base(spB, pbSlot, db, nb_pixels, &bR, &bG, &bB)
                : 0;

            if (spB != NULL && synth_chain_has_no_signal(spB, baseSigPB))
            {
                /* No source in the Path-B chain → silence for LuxSynth (and
                 * LuxWave's preprocessed mirror): zero their sections instead
                 * of running the path on the live fallback — mirror of engine
                 * A's guard above. (The LuxWave wavetable itself keeps its
                 * last content — it only sounds under held MIDI notes.) */
                memset(&preprocessed_temp.polyphonic, 0,
                       sizeof(preprocessed_temp.polyphonic));
                memset(&preprocessed_temp.photowave,  0,
                       sizeof(preprocessed_temp.photowave));
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                    NULL, NULL, NULL, nb_pixels);
            }
            else
            {
                if (spB != NULL)
                    chain_run_inserts_with_viz_tap(spB, audioBuffers, bR, bG, bB,
                                                   nb_pixels, &bR, &bG, &bB);

                /* Per-engine input tap Path-B (per-chain display) — skip
                 * while a player owns the polyphonic commit (luxsynth source
                 * MODULATED + playback: FramePlayerThread publishes then). */
                if (!(g_sp3ctra_config.luxsynth_source_type == 0
                      && lux_sampler_is_playing()))
                    audio_image_buffers_publish_engine_input(
                        audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                        bR, bG, bB, nb_pixels);

                pipeline_path_luxsynth_luxwave(bR, bG, bB, &live_cfg, &preprocessed_temp);
            }
        }
#endif
      }

      /* (Legacy no-op removed: synth_luxwave_set_image_line() resolved to an
       * empty stub — the REAL LuxWave feed is pipeline_path_luxsynth_luxwave
       * above, via luxwave_engine_set_image_line().) */

      pthread_mutex_lock(&db->mutex);
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
        if (s_udp_frame_ls_sends > 0)
        {
          /* Synth-split P3 — the audio-thread MIXER owns the additive/stereo/
           * strokeforge sections of db->preprocessed_data (N-send blend).
           * Only Path-B products are committed from here (polyphonic has its
           * own dedicated block below; photowave rides along). */
          db->preprocessed_data.photowave = preprocessed_temp.photowave;
        }
        else if (src == 1 /* IMAGE_SOURCE_LIVE */ ||
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
      /* 🎨 DISPLAY: update global display buffers from the just-assembled
       * line. After swapBuffers() above it lives in db->processingBuffer_*
       * (assembly fills activeBuffer, the swap moves it there — the exact
       * bytes the former mixed_* identity copies carried). Done UNDER
       * db->mutex: during the device↔feeder handover the MediaSourceService
       * thread can swap/write these very buffers (the displayable mutex nests
       * inside db->mutex here; no path takes them in the reverse order). */
      luxstral_engine_displayable_lock();
      memcpy(luxstral_engine_displayable_R(), db->processingBuffer_R, nb_pixels);
      memcpy(luxstral_engine_displayable_G(), db->processingBuffer_G, nb_pixels);
      memcpy(luxstral_engine_displayable_B(), db->processingBuffer_B, nb_pixels);
      luxstral_engine_displayable_unlock();

      pthread_cond_signal(&db->cond);
      pthread_mutex_unlock(&db->mutex);

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
 * Mirrors the per-synth routing of udpThread's completed-line block — sampler
 * record hooks included (REC from an internal source, resampling while a
 * player runs) — minus the device-only machinery (fragment reassembly,
 * acquisition gate, sequencer mix). Known v1 limitation, matching
 * FramePlayerThread's behaviour: the shared modulated channel stays owned by
 * the sampler/score.
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
  const int score_playing   = lux_sampler_is_score_playing();
#else
  const int sampler_playing = 0;
  const int score_playing   = 0;
#endif

#ifdef VST_MODE
  /* ── Sampler machinery — mirror of udpThread's modulated build ─────────────
   * Without it, REC was dead whenever the device was silent (VIDEO / IMAGE /
   * CAMERA sessions): phase 1 below is the ONLY drain site of the start/stop
   * record commands, and phase 2 the only capture site. Runs BEFORE the
   * any-active early-out — the drain and the resampling capture need no
   * internal source (MediaSourceService keeps ticking at its idle rate). */
  const SynthChainPlan *spSmp = NULL;
  int smp_slot = -1;
  for (int s = 0; s < CHAIN_SYNTH_COUNT && !spSmp; s++)
      if (frame_plan.synth[s].present && frame_plan.synth[s].has_sampler)
      { spSmp = &frame_plan.synth[s]; smp_slot = s; }

  /* Set when the idle build below ran: the sampler chain's pre-marker stream
   * (its modulated channel). The per-synth blocks further down consume it
   * instead of re-running the chain (stateful FX tick once per line). */
  const uint8_t *smpMod_R = NULL, *smpMod_G = NULL, *smpMod_B = NULL;
  int smp_premarker_tap_done = 0;
  if (spSmp)
  {
      static uint32_t s_feeder_line_id = 0;   /* debug/sync id (no UDP line) */
      const uint8_t *sbR, *sbG, *sbB;
      const int sbSig = synth_source_base(spSmp, smp_slot, db, nb_pixels,
                                          &sbR, &sbG, &sbB);

      /* Phase 1 — drain start/stop REC commands + cache the chain's source
       * frame for the player's darken-blend. */
      lux_sampler_on_live_frame_assembled(sbR, sbG, sbB, (uint16_t)nb_pixels);

      if (sampler_playing)
      {
          /* PLAYING (sampler or score relay): resampling capture — the frame
           * player owns the modulated channel; feed its output to every other
           * engine with an armed rec slot (records the combination). */
          uint8_t *plR, *plG, *plB;
          audio_image_buffers_get_sampler_pointers(audioBuffers,
                                                   &plR, &plG, &plB);
          if (plR && plG && plB)
              lux_samplers_record_modulated(plR, plG, plB,
                                            (uint16_t)nb_pixels,
                                            ++s_feeder_line_id);
      }
      else if (sbSig > 0)
      {
          /* IDLE / REC: run the chain's pre-marker processors once → the
           * modulated stream; publish it (the sampler chain OWNS the
           * modulated bus, as on the device path), mirror the sampler
           * snapshot and capture into the armed slot (phase 2). */
          SynthChainPlan pre;
          chain_build_sampler_premarker_plan(spSmp, &pre);
          chain_run_inserts_with_viz_tap(&pre, audioBuffers, sbR, sbG, sbB,
                                         nb_pixels,
                                         &smpMod_R, &smpMod_G, &smpMod_B);
          smp_premarker_tap_done = (pre.viz_tap_insert >= 0);
          if (smpMod_R && smpMod_G && smpMod_B)
          {
              audio_image_buffers_snapshot_modulated(audioBuffers,
                                                     smpMod_R, smpMod_G,
                                                     smpMod_B, nb_pixels);
              lux_sampler_on_modulated_frame_ready(smpMod_R, smpMod_G,
                                                   smpMod_B,
                                                   (uint16_t)nb_pixels,
                                                   ++s_feeder_line_id);
          }
      }
  }
#endif

  if (!internal_source_any_active())
    return;

  /* ── Synth-split P3: LuxStral SENDS (internal-source ticks) ──────────────
   * Mirror of udpThread's send loop — every live/internal-fed send is staged
   * here when the device does not stream. Player-owned sends belong to
   * FramePlayerThread. */
  for (int k = 0; k < frame_plan.num_ls_sends; k++)
  {
      const LsSendPlan     *snd = &frame_plan.ls_send[k];
      const SynthChainPlan *sp  = &snd->recipe;

      if ((sp->has_sampler && sampler_playing)
          || (sp->has_score && score_playing))
          continue;   /* staged by FramePlayerThread */

      const uint8_t *sbR, *sbG, *sbB;
      const int sig = synth_source_base(
          sp, CHAIN_SYNTH_COUNT + CHAIN_MAX_CHAINS + snd->chain_idx,
          db, nb_pixels, &sbR, &sbG, &sbB);
      if (synth_chain_has_no_signal(sp, sig) || sig <= 0)
      {
          /* No internal stream this tick: sig==0 means the live path owns the
           * frame (device streaming edge) — leave the staging as-is; a true
           * no-signal chain goes silent. */
          if (synth_chain_has_no_signal(sp, sig))
          {
              synth_staging_set_inactive(snd->chain_idx);
              if (k == 0)
                  audio_image_buffers_publish_engine_input(
                      audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                      NULL, NULL, NULL, nb_pixels);
          }
          continue;
      }

      const uint8_t *ssR = sbR, *ssG = sbG, *ssB = sbB;
#ifdef VST_MODE
      if (sp->has_sampler && smpMod_R)
      {
          ssR = smpMod_R; ssG = smpMod_G; ssB = smpMod_B;
          feeder_sampler_chain_shortcut(sp, audioBuffers,
                                        smp_premarker_tap_done,
                                        sbR, sbG, sbB,
                                        smpMod_R, smpMod_G, smpMod_B,
                                        nb_pixels);
      }
      else
#endif
      if (sp->num_inserts > 0 || sp->viz_tap_insert >= 0)
          chain_run_inserts_with_viz_tap(sp, audioBuffers, sbR, sbG, sbB,
                                         nb_pixels, &ssR, &ssG, &ssB);

      if (k == 0 && !(sp->has_score && score_playing))
      {
#ifdef VST_MODE
          if (!smpMod_R)   /* sampler chain owns the bus — published above */
#endif
          audio_image_buffers_snapshot_modulated(audioBuffers, ssR, ssG, ssB,
                                                 nb_pixels);
          audio_image_buffers_publish_engine_input(
              audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
              ssR, ssG, ssB, nb_pixels);
      }

      {
          PipelineConfig scfg = pipeline_build_config_ls_send(
              snd->bank_slot, snd->chain_idx, /*player_fed*/ 0);
          pipeline_path_luxstral(ssR, ssG, ssB, &scfg, &s_ls_send_pp_feeder);
          int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                    ? scfg.pixels_per_note : 1);
          if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
          synth_staging_stage_luxstral(snd->chain_idx, snd->bank_slot,
                                       &s_ls_send_pp_feeder, nnotes,
                                       scfg.stereo_enabled);
      }
  }

  /* ── LuxStral A — LEGACY (num_ls_sends == 0 only; P3 sends staged above) ── */
  const uint8_t *baseA_R = NULL, *baseA_G = NULL, *baseA_B = NULL;
  int a_done = 0;
  if (frame_plan.num_ls_sends == 0 && spA->present && !sampler_playing)
  {
    const int baseSigA = synth_source_base(spA, CHAIN_SYNTH_LUXSTRAL, db,
                                           nb_pixels,
                                           &baseA_R, &baseA_G, &baseA_B);
    if (baseSigA > 0)
    {
    const uint8_t *srcR, *srcG, *srcB;
#ifdef VST_MODE
    if (spA == spSmp && smpMod_R)
    {
        /* The chain holding the sampler IS the modulated channel — reuse the
         * pre-marker stream built by the sampler block above (udpThread's
         * short-circuit: stateful FX tick once per line); probes and the
         * selection tap are fed by their position around the marker. */
        srcR = smpMod_R; srcG = smpMod_G; srcB = smpMod_B;
        feeder_sampler_chain_shortcut(spA, audioBuffers,
                                      smp_premarker_tap_done,
                                      baseA_R, baseA_G, baseA_B,
                                      smpMod_R, smpMod_G, smpMod_B, nb_pixels);
    }
    else
#endif
    chain_run_inserts_with_viz_tap(spA, audioBuffers,
                                   baseA_R, baseA_G, baseA_B,
                                   nb_pixels, &srcR, &srcG, &srcB);

    /* MODULATED display bus — mirror of udpThread's per-line publish (legacy
     * CHAIN-1 panel + resampling views keep working off internal sources).
     * Skipped while a score relay owns A's stream — the player thread feeds
     * the engine, not this tick. */
    if (!(spA->has_score && score_playing))
    {
#ifdef VST_MODE
      if (!smpMod_R)   /* sampler chain owns the bus — published above */
#endif
      audio_image_buffers_snapshot_modulated(audioBuffers, srcR, srcG, srcB,
                                             nb_pixels);

      /* Per-engine input tap A (per-chain display) — the feeder owns A's
       * commit here (same source-routing gating as the commit at the bottom
       * of this tick: srcA != 0, or the Source=S idle passthrough). */
#ifdef VST_MODE
      const int a_commit_here =
          (g_sp3ctra_config.luxstral_source_type != 0)
          || lux_sampler_is_passthrough();
#else
      const int a_commit_here = 1;
#endif
      if (a_commit_here)
        audio_image_buffers_publish_engine_input(
            audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
            srcR, srcG, srcB, nb_pixels);
    }

    PipelineConfig cfg = pipeline_build_config_live();
    if (pipeline_process_frame(srcR, srcG, srcB, &cfg, &s_feeder_pp) == 0)
      a_done = 1;
    }
    else if (synth_chain_has_no_signal(spA, baseSigA))
    {
      /* A's chain carries no stream of its own (internal source empty / no
       * source module) → white tap = unfed engine, never a stale frame. */
      audio_image_buffers_publish_engine_input(
          audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
          NULL, NULL, NULL, nb_pixels);
    }
  }

  /* ── Path B (LuxSynth + LuxWave) — LuxSynth's chain (LuxWave shares it, as
   * in udpThread); fall back to LuxWave's own chain when LuxSynth is absent. */
  const SynthChainPlan *spPB = spS->present ? spS
                             : (spW->present ? spW : NULL);
  const int pb_slot = (spPB == spW) ? CHAIN_SYNTH_LUXWAVE : CHAIN_SYNTH_LUXSYNTH;
  const uint8_t *pbR = NULL, *pbG = NULL, *pbB = NULL;
  int pb_done = 0;
  if (spPB)
  {
    const int baseSigPB = synth_source_base(spPB, pb_slot, db, nb_pixels,
                                            &pbR, &pbG, &pbB);
    if (baseSigPB > 0)
    {
    const uint8_t *sR, *sG, *sB;
#ifdef VST_MODE
    if (spPB == spSmp && smpMod_R)
    {
        /* Sampler on Path-B's chain: same short-circuit as engine A's. */
        sR = smpMod_R; sG = smpMod_G; sB = smpMod_B;
        feeder_sampler_chain_shortcut(spPB, audioBuffers,
                                      smp_premarker_tap_done,
                                      pbR, pbG, pbB,
                                      smpMod_R, smpMod_G, smpMod_B, nb_pixels);
    }
    else
#endif
    chain_run_inserts_with_viz_tap(spPB, audioBuffers, pbR, pbG, pbB,
                                   nb_pixels, &sR, &sG, &sB);

    /* Same MODULATED publish for a Path-B-only session (legacy CHAIN-1
     * panel): only when LuxStral A is absent — with A placed, A's block
     * above owns the (single, global) bus. Sampler/score guards mirror A's
     * (a published sampler-chain stream owns the bus, as on the device path). */
    if (!spA->present && !sampler_playing
        && !(spPB->has_score && score_playing)
#ifdef VST_MODE
        && !smpMod_R
#endif
       )
      audio_image_buffers_snapshot_modulated(audioBuffers, sR, sG, sB,
                                             nb_pixels);

    /* Per-engine input tap Path-B (per-chain display) — skip while a player
     * owns the polyphonic commit (luxsynth source MODULATED + playback). */
#ifdef VST_MODE
    const int pb_player_owned =
        (g_sp3ctra_config.luxsynth_source_type == 0) && sampler_playing;
#else
    const int pb_player_owned = 0;
#endif
    if (!pb_player_owned)
      audio_image_buffers_publish_engine_input(
          audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
          sR, sG, sB, nb_pixels);

    PipelineConfig cfg = pipeline_build_config_live();
    pipeline_path_luxsynth_luxwave(sR, sG, sB, &cfg, &s_feeder_pp);
    pb_done = 1;
    }
    else if (synth_chain_has_no_signal(spPB, baseSigPB))
    {
      audio_image_buffers_publish_engine_input(
          audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
          NULL, NULL, NULL, nb_pixels);
    }
  }

#ifdef VST_MODE
  /* ── LuxStral engine B — LEGACY (num_ls_sends == 0 only; the P3 send loop
   * above owns every LuxStral chain). Skipped while a player owns B's input. */
  if (frame_plan.num_ls_sends == 0
      && spLB->present && !(spLB->has_sampler && sampler_playing)
      && !(spLB->has_score && lux_sampler_is_score_playing()))
  {
    const uint8_t *baseB_R, *baseB_G, *baseB_B;
    if (synth_source_base(spLB, CHAIN_SYNTH_LUXSTRAL_B, db, nb_pixels,
                          &baseB_R, &baseB_G, &baseB_B) > 0)
    {
      if (luxstral_b_db_ensure_ready())
      {
        const uint8_t *bxR, *bxG, *bxB;
        if (spLB == spSmp && smpMod_R)
        {
            /* Sampler on B's chain: same short-circuit as engine A's. */
            bxR = smpMod_R; bxG = smpMod_G; bxB = smpMod_B;
            feeder_sampler_chain_shortcut(spLB, audioBuffers,
                                          smp_premarker_tap_done,
                                          baseB_R, baseB_G, baseB_B,
                                          smpMod_R, smpMod_G, smpMod_B,
                                          nb_pixels);
        }
        else
        chain_run_inserts_with_viz_tap(spLB, audioBuffers,
                                       baseB_R, baseB_G, baseB_B,
                                       nb_pixels, &bxR, &bxG, &bxB);
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

  /* ── Probe-only chains fed by an INTERNAL source (device silent): run their
   * ordered inserts so probes capture at their position — the udpThread path
   * does the same at line rate while the device streams. Live-sourced probe
   * chains are skipped here (no fresh live frames to observe). */
  for (int pc = 0; pc < frame_plan.num_probe_chains; pc++)
  {
    const SynthChainPlan *spP = &frame_plan.probe_chain[pc];
    const uint8_t *pcR, *pcG, *pcB;
    if (synth_source_base(spP, CHAIN_SYNTH_COUNT + pc, db, nb_pixels,
                          &pcR, &pcG, &pcB) > 0)
    {
      const uint8_t *poR, *poG, *poB;
      chain_run_inserts_with_viz_tap(spP, audioBuffers, pcR, pcG, pcB,
                                     nb_pixels, &poR, &poG, &poB);
    }
  }

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
// REMOVED (DMX):   // Close the file descriptor only if it is valid
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
      ChainPlan planB_render;
      chain_plan_get(&planB_render);

      if (planB_render.num_ls_sends > 0)
      {
        /* ── Synth-split P3: PULL MIX ─────────────────────────────────────
         * Blend every active LuxStral send (intensity-weighted) into the
         * single engine feed. The mixer is db->preprocessed_data's SOLE
         * writer for the additive/stereo/strokeforge sections while sends
         * exist (producers stage; udp/feeder commit Path-B products only).
         * StrokeForge analyses the MIXED notes — single-send parity intact. */
        DoubleBuffer *mdb = context->doubleBuffer;
        static PreprocessedImageData s_mixed_pp;   /* audio-thread scratch */
        float  mixed_contrast = 0.0f;
        int    stereo_valid   = 0;
        const int max_notes   = PREPROCESS_MAX_NOTES;

        const int mixed = synth_staging_mix_luxstral(
            &planB_render,
            s_mixed_pp.additive.notes, max_notes,
            s_mixed_pp.stereo.left_gains, s_mixed_pp.stereo.right_gains,
            &mixed_contrast, &stereo_valid);
        (void) stereo_valid;   /* gains are centre-filled when mono */

        if (mixed > 0)
        {
          s_mixed_pp.additive.contrast_factor = mixed_contrast;
          /* Display axis: with pixels_per_note == 1 the note and pixel axes
           * coincide — reuse the mixed amplitudes as the grayscale mirror. */
          memcpy(s_mixed_pp.additive.grayscale, s_mixed_pp.additive.notes,
                 sizeof(s_mixed_pp.additive.grayscale));
          /* StrokeForge on the blended flux (cheap early-out when disabled). */
          {
            int sf_notes = get_cis_pixels_nb();
            if (sf_notes > max_notes) sf_notes = max_notes;
            img_stage_blob_detect(s_mixed_pp.additive.notes, sf_notes,
                                  mixed_contrast, &s_mixed_pp.strokeforge);
          }
        }
        else
        {
          /* No active send → TRUE silence (chain no-signal contract). */
          memset(&s_mixed_pp.additive,    0, sizeof(s_mixed_pp.additive));
          memset(&s_mixed_pp.stereo.left_gains, 0,
                 sizeof(s_mixed_pp.stereo.left_gains));
          memset(&s_mixed_pp.stereo.right_gains, 0,
                 sizeof(s_mixed_pp.stereo.right_gains));
          memset(&s_mixed_pp.strokeforge, 0, sizeof(s_mixed_pp.strokeforge));
        }

        pthread_mutex_lock(&mdb->mutex);
        memcpy(&mdb->preprocessed_data.additive, &s_mixed_pp.additive,
               sizeof(s_mixed_pp.additive));
        memcpy(mdb->preprocessed_data.stereo.left_gains,
               s_mixed_pp.stereo.left_gains,
               sizeof(s_mixed_pp.stereo.left_gains));
        memcpy(mdb->preprocessed_data.stereo.right_gains,
               s_mixed_pp.stereo.right_gains,
               sizeof(s_mixed_pp.stereo.right_gains));
        mdb->preprocessed_data.strokeforge = s_mixed_pp.strokeforge;
        mdb->preprocessed_data.timestamp_us = 1;   /* has_preprocessed gate */
        {
          struct timeval tv;
          gettimeofday(&tv, NULL);
          mdb->preprocessed_data.timestamp_us =
              (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
        }
        /* Tag must match the engine's source gating (src==0 wants tag 2). */
        mdb->dataReady = (g_sp3ctra_config.luxstral_source_type == 0) ? 2 : 1;
        pthread_mutex_unlock(&mdb->mutex);

        synth_AudioProcess(audio_read_R, audio_read_G, audio_read_B,
                           context->doubleBuffer);
      }
      else
      {
        const int bActive = planB_render.synth[CHAIN_SYNTH_LUXSTRAL_B].present
                            && __atomic_load_n(&s_luxstral_b_db_ready, __ATOMIC_ACQUIRE);
        if (bActive)
        {
          /* Lazy init keyed on the ENGINE's own state, not a function-local
           * static: the old flag survived a core teardown (which now frees B's
           * oscillator clone) and skipped re-initialisation on the next start. */
          if (!synth_luxstral_engine_b_ready())
            synth_luxstral_init_engine_b();
          synth_AudioProcess_ab(audio_read_R, audio_read_G, audio_read_B,
                                context->doubleBuffer, &s_luxstral_b_db);
        }
        else
        {
          synth_AudioProcess(audio_read_R, audio_read_G, audio_read_B, context->doubleBuffer);
        }
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
