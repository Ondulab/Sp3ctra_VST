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

#include "../processing/synth_staging.h"
#include "../processing/luxsynth_feed.h"           /* M4 — core-side LuxSynth feed */
#include "../processing/image_pipeline_stages.h"   /* img_stage_blob_detect (P3 mixer) */

/* Synth-split P3 — per-thread scratch for ONE LuxStral send's conditioned
 * frame before staging (each producer thread has its own; the staging module
 * seqlocks the shared slots). */
static PreprocessedImageData s_ls_send_pp;         /* udpThread */
static PreprocessedImageData s_ls_send_pp_feeder;  /* feeder tick */
/* M4 — per-thread scratch for ONE LuxSynth send's conditioned line. */
static float s_lx_line[CIS_MAX_PIXELS_NB];         /* udpThread */
static float s_lx_line_feeder[CIS_MAX_PIXELS_NB];  /* feeder tick */
static int s_udp_frame_ls_sends = 0;   /* udpThread-only: plan.num_ls_sends of
                                        * the current line (commit-scope read) */
static int s_udp_frame_pb_ran = 0;     /* udpThread-only: Path-B products of
                                        * preprocessed_temp valid this line */

#ifdef VST_MODE
/* Player-side execution of the inserts placed AFTER a SCORE/SAMPLER marker —
 * defined below chain_run_inserts_with_viz_tap (which it reuses). */
static int chain_apply_post_marker_inserts(const SynthChainPlan *sp,
                                           int marker_id,
                                           struct AudioImageBuffers *viz_bus,
                                           uint8_t *r, uint8_t *g, uint8_t *b,
                                           int nb_pixels);

/* ── Synth-split P3 — FramePlayerThread: stage every PLAYER-OWNED LuxStral
 * send from the blended playback frame. A send is player-owned when its chain
 * hosts the SAMPLER (the caller only runs while a slot plays), or the SCORE
 * during score playback. Each send applies ITS OWN chain's post-marker
 * inserts on a private copy (FX must not leak between chains) and its own
 * conditioning bank; intensity is applied at MIX time by the audio thread.
 * Returns plan.num_ls_sends so the caller keeps the legacy engine-A path
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
 * Slots: [0..3] synth engines (legacy recipes), [4..11] the M3 uniform chain
 * recipes (CHAIN_SYNTH_COUNT + chain_idx). */
static uint8_t s_synth_src_scratch[CHAIN_SYNTH_COUNT + CHAIN_MAX_CHAINS][3][INTERNAL_SRC_MAX_PIXELS];

static int synth_source_base(const SynthChainPlan *sp, int synth_slot,
                             DoubleBuffer *db, int nb_pixels,
                             const uint8_t **out_r, const uint8_t **out_g,
                             const uint8_t **out_b)
{
    const int kind = internal_source_kind_for_chain_src(sp->source_kind);
    if (kind >= 0)
    {
        /* Defensive clamp — every caller passes a slot < the scratch dim
         * (engines 0..3, uniform chains 4..11). */
        if (synth_slot < 0
            || synth_slot >= CHAIN_SYNTH_COUNT + CHAIN_MAX_CHAINS)
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

/* ── M3: uniform per-chain execution ─────────────────────────────────────────
 * One chain recipe (plan.chain[c]) runs ONCE per frame: source → ordered
 * inserts. Probes capture, the selection tap publishes, LuxStral OUT markers
 * STAGE their conditioned frame, and the Path-B OUT marker (LuxSynth — or
 * LuxWave when no LuxSynth is placed) reports the stream at its position for
 * the pipeline_path_luxsynth_luxwave call that follows the loop. */
typedef struct {
    const uint8_t *pbR, *pbG, *pbB;   /* stream at the Path-B OUT marker */
    int            pb_found;
    const uint8_t *lsR, *lsG, *lsB;   /* stream staged into the FIRST LuxStral OUT */
    int            ls_staged;
} ChainExecOut;

/* Positional executor (non-player chains): run inserts one step at a time so
 * each OUT marker and the selection tap observe the stream at their exact
 * position. Stages every "→ LUXSTRAL" marker (bank = insert_state_idx) via
 * `pp_scratch` and every "→ LUXSYNTH" marker (conditioned line, M4) via
 * `lx_line` (both per-thread scratches; lx_line may be NULL to skip). */
static void chain_execute_positional(const SynthChainPlan *sp, int chain_idx,
                                     AudioImageBuffers *viz_bus,
                                     const uint8_t *inR, const uint8_t *inG,
                                     const uint8_t *inB, int nb_pixels,
                                     PreprocessedImageData *pp_scratch,
                                     float *lx_line,
                                     int player_fed, int pb_marker_id,
                                     int allow_sampler_record,
                                     ChainExecOut *out)
{
    void *states[CHAIN_PLAN_MAX_INSERTS];
    chain_resolve_insert_states(sp, states);

    const uint8_t *cr = inR, *cg = inG, *cb = inB;
    int tap = sp->viz_tap_insert;
    if (tap > sp->num_inserts)
        tap = sp->num_inserts;

    memset(out, 0, sizeof(*out));

    for (int i = 0; i <= sp->num_inserts; i++)
    {
        if (viz_bus != NULL && tap == i)
            audio_image_buffers_publish_selection_tap(viz_bus, cr, cg, cb,
                                                      nb_pixels);
        if (i == sp->num_inserts)
            break;

        const int id = sp->insert_id[i];
        if (id == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL)
        {
            const int bank = sp->insert_state_idx[i];
            PipelineConfig scfg =
                pipeline_build_config_ls_send(bank, chain_idx, player_fed);
            pipeline_path_luxstral(cr, cg, cb, &scfg, pp_scratch);
            int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                      ? scfg.pixels_per_note : 1);
            if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
            synth_staging_stage_luxstral(chain_idx, bank, pp_scratch, nnotes,
                                         scfg.stereo_enabled);
            if (!out->ls_staged)
            { out->lsR = cr; out->lsG = cg; out->lsB = cb; out->ls_staged = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
        {
            /* M4 — stage the conditioned line + RGB at the send's position;
             * the audio thread mixes and runs ONE FFT (luxsynth_feed_tick). */
            if (lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxsynth_condition_line(cr, cg, cb, bank, lx_line, nb_pixels);
                synth_staging_stage_luxsynth(chain_idx, bank, lx_line,
                                             cr, cg, cb, nb_pixels);
            }
            if (pb_marker_id == id && !out->pb_found)
            { out->pbR = cr; out->pbG = cg; out->pbB = cb; out->pb_found = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
        {
            /* M5 — stage the conditioned wavetable line at the send's
             * position; the audio thread pulls the bipolar mix. */
            if (lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxwave_condition_line(cr, cg, cb, bank, lx_line, nb_pixels);
                synth_staging_stage_luxwave(chain_idx, bank, lx_line,
                                            nb_pixels);
            }
            if (pb_marker_id == id && !out->pb_found)
            { out->pbR = cr; out->pbG = cg; out->pbB = cb; out->pb_found = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_SAMPLER)
        {
            /* Per-chain sampler feed (2026-07-11): a positionally-executed
             * SAMPLER marker records ITS chain's stream into ITS engine's
             * armed slot (idle only — the caller clears the flag during
             * playback, when the resampling path owns every recording).
             * Pass-through: the idle sampler is a passthrough module. */
#ifdef VST_MODE
            if (allow_sampler_record)
                lux_sampler_record_chain_frame(sp->insert_state_idx[i],
                                               cr, cg, cb,
                                               (uint16_t) nb_pixels);
#endif
        }
        else
        {
            /* Processors / probes / pass-through markers: one-insert step
             * (identical to the batched image_chain_run, which is just this
             * sequential walk). */
            const uint8_t *nr, *ng, *nbv;
            image_chain_run(cr, cg, cb, nb_pixels, g_sp3ctra_config.num_octaves,
                            sp->insert_id + i, (void *const *)(states + i), 1,
                            &nr, &ng, &nbv);
            cr = nr; cg = ng; cb = nbv;
        }
    }
}

/* Sampler/score-relay chain SHORT-CIRCUIT walk (shared udpThread + feeder):
 * the modulated/player channel IS the stream below the marker — no
 * image_chain_run. Probes and the Path-B OUT observe base (above the marker)
 * or mod (below); post-marker probes are skipped while the player thread owns
 * them (it captures at the exact position). The LuxStral OUT stream is mod
 * (staged by the caller). The LuxSynth OUT is staged HERE (conditioned line,
 * M4) unless the player thread owns the stream (lx_line NULL to skip). */
static void chain_shortcut_walk(const SynthChainPlan *sp, int chain_idx,
                                int skip_post_marker_probes,
                                int pb_marker_id, float *lx_line,
                                const uint8_t *baseR, const uint8_t *baseG,
                                const uint8_t *baseB,
                                const uint8_t *modR, const uint8_t *modG,
                                const uint8_t *modB, int nb_pixels,
                                ChainExecOut *out)
{
    memset(out, 0, sizeof(*out));
    int after_marker = 0;
    for (int i = 0; i < sp->num_inserts; i++)
    {
        const int id = sp->insert_id[i];
        if (id == IMAGE_CHAIN_INSERT_SAMPLER || id == IMAGE_CHAIN_INSERT_SCORE)
        { after_marker = 1; continue; }
        if (id == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
        {
            if (after_marker && skip_post_marker_probes)
                continue;   /* captured by the player thread */
            video_scroll_capture_line(
                video_scroll_instance(sp->insert_state_idx[i]),
                after_marker ? modR : baseR,
                after_marker ? modG : baseG,
                after_marker ? modB : baseB, nb_pixels);
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
        {
            const uint8_t *fr = after_marker ? modR : baseR;
            const uint8_t *fg = after_marker ? modG : baseG;
            const uint8_t *fb = after_marker ? modB : baseB;
            if (lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxsynth_condition_line(fr, fg, fb, bank, lx_line, nb_pixels);
                synth_staging_stage_luxsynth(chain_idx, bank, lx_line,
                                             fr, fg, fb, nb_pixels);
            }
            if (pb_marker_id == id && !out->pb_found)
            { out->pbR = fr; out->pbG = fg; out->pbB = fb; out->pb_found = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
        {
            const uint8_t *fr = after_marker ? modR : baseR;
            const uint8_t *fg = after_marker ? modG : baseG;
            const uint8_t *fb = after_marker ? modB : baseB;
            if (lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxwave_condition_line(fr, fg, fb, bank, lx_line, nb_pixels);
                synth_staging_stage_luxwave(chain_idx, bank, lx_line,
                                            nb_pixels);
            }
            if (pb_marker_id == id && !out->pb_found)
            { out->pbR = fr; out->pbG = fg; out->pbB = fb; out->pb_found = 1; }
        }
    }
    /* LuxStral OUT stream = the modulated channel (staged by the caller). */
    out->lsR = modR; out->lsG = modG; out->lsB = modB;
}

/* ── M7 — plan-driven ownership queries (replace the *_source_type gates) ────
 * Used by FramePlayerThread/LuxSampler: which db sections may the player own?
 * Cheap (one plan snapshot + scan); Non-RT callers only. */
int chain_additive_player_candidate(void)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    if (plan.num_ls_sends > 0)
        return plan.ls_send[0].recipe.has_sampler
            || plan.ls_send[0].recipe.has_score;
    /* no send → legacy additive tick; the first sampler chain owns it */
    for (int c = 0; c < plan.num_chains; c++)
        if (plan.chain[c].present && plan.chain[c].has_sampler)
            return 1;
    return 0;
}

int chain_pathb_player_candidate(int is_score)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    for (int c = 0; c < plan.num_chains; c++)
    {
        const SynthChainPlan *sp = &plan.chain[c];
        if (!sp->present) continue;
        for (int i = 0; i < sp->num_inserts; i++)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                return is_score ? sp->has_score : sp->has_sampler;
    }
    return 0;
}

/* ── M4 — FramePlayerThread: stage the "→ LUXSYNTH" send from the blended
 * playback frame while the player owns its chain's stream (sampler on the
 * chain, or score relay). Single writer: udpThread/feeder skip the LuxSynth
 * staging of player-owned chains. Called at the same site that publishes the
 * Path-B engine tap. */
void lx_send_stage_player_frame(const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels)
{
    static float s_line_player[CIS_MAX_PIXELS_NB];   /* FramePlayerThread only */
    ChainPlan plan;
    chain_plan_get(&plan);
    if (nb_pixels > CIS_MAX_PIXELS_NB)
        nb_pixels = CIS_MAX_PIXELS_NB;
    for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; c++)
    {
        const SynthChainPlan *sp = &plan.chain[c];
        if (!sp->present || !(sp->has_sampler || sp->has_score))
            continue;
        for (int i = 0; i < sp->num_inserts; i++)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
            {
                const int bank = sp->insert_state_idx[i];
                luxsynth_condition_line(r, g, b, bank, s_line_player,
                                        nb_pixels);
                synth_staging_stage_luxsynth(c, bank, s_line_player,
                                             r, g, b, nb_pixels);
                return;
            }
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
            /* deriveChainRouting only routes engine A's global source —
             * engine A's clause is included for symmetry (normally already
             * covered by luxstral_path.source above). */
            (frame_plan.synth[CHAIN_SYNTH_LUXSTRAL].present
             && frame_plan.synth[CHAIN_SYNTH_LUXSTRAL].has_sampler);
        /* MOD-BUS OWNER: the FIRST chain (model order) hosting a sampler —
         * the single modulated channel is built from ITS recipe and ITS OWN
         * source (fix: a sampler chain fed by IMAGE/VIDEO must never build
         * its passthrough/record stream from the live device frames). Other
         * sampler chains run positionally on their own stream. Any sampler
         * chain needs the modulated machinery (REC hooks). */
        int mod_owner_chain = -1;
        for (int c = 0; c < frame_plan.num_chains; c++)
            if (frame_plan.chain[c].present && frame_plan.chain[c].has_sampler)
            { mod_owner_chain = c; need_modulated = 1; break; }

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
                const SynthChainPlan *spSmp =
                    (mod_owner_chain >= 0)
                        ? &frame_plan.chain[mod_owner_chain] : NULL;

                /* Base frame = the OWNER CHAIN's own source (IMAGE/VIDEO/
                 * CAMERA line, or the live frame for a SP3CTRA/legacy chain).
                 * A no-signal chain builds NO modulated frame this line —
                 * never the live device fallback. */
                const uint8_t *smbR = db->activeBuffer_R;
                const uint8_t *smbG = db->activeBuffer_G;
                const uint8_t *smbB = db->activeBuffer_B;
                int smp_no_signal = 0;
                if (spSmp)
                {
                    const int smSig = synth_source_base(
                        spSmp, CHAIN_SYNTH_COUNT + mod_owner_chain, db,
                        nb_pixels, &smbR, &smbG, &smbB);
                    smp_no_signal = synth_chain_has_no_signal(spSmp, smSig)
                                    || smSig < 0;
                }

                SynthChainPlan pre;          /* pre-marker processor sub-plan */
                pre.num_inserts    = 0;
                pre.viz_tap_insert = -1;
                int chain_specific = 0;
                if (spSmp && !smp_no_signal)
                    chain_specific = chain_build_sampler_premarker_plan(spSmp, &pre);

                if (smp_no_signal)
                {
                    /* owner chain carries no stream → mod stays NULL */
                }
                else if (chain_specific)
                {
                    chain_run_inserts_with_viz_tap(&pre, audioBuffers,
                                                   smbR, smbG, smbB,
                                                   nb_pixels,
                                                   &mod_R, &mod_G, &mod_B);
                    premarker_tap_done = (pre.viz_tap_insert >= 0);
                }
                else
                image_chain_process_inserts(smbR, smbG, smbB,
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
                     * writes it into the OWNER ENGINE's active recording
                     * slot, so recorded samples are Pitch+Mask "printed".
                     * Other sampler engines record their own chain's stream
                     * at their marker (per-chain feed). */
#ifdef VST_MODE
                    {
                        int owner_engine = 0;
                        if (spSmp)
                            for (int i = 0; i < spSmp->num_inserts; i++)
                                if (spSmp->insert_id[i]
                                    == IMAGE_CHAIN_INSERT_SAMPLER)
                                { owner_engine = spSmp->insert_state_idx[i];
                                  break; }
                        lux_sampler_on_modulated_frame_ready(
                            owner_engine, mod_R, mod_G, mod_B,
                            (uint16_t)nb_pixels, packet.line_id);
                    }
#endif
                }
            }
        }


        /* ── Legacy full-pipeline tick input ──────────────────────────────────
         * With LuxStral sends present (any "→ LUXSTRAL" placed), the M3 chain
         * loop below owns every chain (inserts, probes, taps, staging) and the
         * audio-thread mixer owns db's additive sections — this base frame
         * only keeps pipeline_process_frame ticking for the legacy commit
         * paths (no-LuxStral topologies + Path-B products). */
        {
            const SynthChainPlan *spA = &frame_plan.synth[CHAIN_SYNTH_LUXSTRAL];
            (void) synth_source_base(spA, CHAIN_SYNTH_LUXSTRAL, db, nb_pixels,
                                     &src_R, &src_G, &src_B);
        }
#ifdef VST_MODE
        const int player_running_now = lux_sampler_is_playing();
        const int score_playing_now  = lux_sampler_is_score_playing();
#else
        const int player_running_now = 0;
        const int score_playing_now  = 0;
#endif

        /* ── M3: uniform per-chain loop ──────────────────────────────────────
         * Every observable chain (an OUT, a probe, or the zone-1 selection)
         * runs ONCE: source → ordered inserts; probes capture and OUT markers
         * tap the stream at their exact position. LuxStral OUTs stage their
         * conditioned frame (mixed by the audio thread); the Path-B OUT
         * (LuxSynth — or LuxWave when no LuxSynth is placed anywhere) feeds
         * pipeline_path_luxsynth_luxwave after the loop. */
        const uint8_t *pb_R = NULL, *pb_G = NULL, *pb_B = NULL;
        int pb_found = 0, pb_no_signal = 0, pb_player_owned = 0;
        int pb_chain = -1, pb_marker = -1;
        for (int c = 0; c < frame_plan.num_chains; c++)
        {
            const SynthChainPlan *sp = &frame_plan.chain[c];
            if (!sp->present) continue;
            for (int i = 0; i < sp->num_inserts; i++)
            {
                if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
                    && pb_marker != IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                { pb_chain = c; pb_marker = IMAGE_CHAIN_INSERT_OUT_LUXSYNTH; }
                else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE
                         && pb_chain < 0)
                { pb_chain = c; pb_marker = IMAGE_CHAIN_INSERT_OUT_LUXWAVE; }
            }
        }
        const int first_send_chain =
            frame_plan.num_ls_sends > 0 ? frame_plan.ls_send[0].chain_idx : -1;

        for (int c = 0; c < frame_plan.num_chains; c++)
        {
            const SynthChainPlan *sp = &frame_plan.chain[c];
            if (!sp->present) continue;

            /* This chain's LuxStral OUT bank + LuxSynth/LuxWave OUT presence
             * (V1: at most one OUT per type per chain). */
            int ls_bank = -1, has_lx = 0, has_lw = 0;
            for (int i = 0; i < sp->num_inserts; i++)
            {
                if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL
                    && ls_bank < 0)
                    ls_bank = sp->insert_state_idx[i];
                else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                    has_lx = 1;
                else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
                    has_lw = 1;
            }

            const int stream_player_owned =
                (sp->has_sampler && player_running_now)
                || (sp->has_score && score_playing_now);

            if (ls_bank >= 0 && stream_player_owned)
            {
                /* FramePlayerThread stages this send and runs its post-marker
                 * inserts/probes/taps on the playback frames. */
                if (c == pb_chain) pb_player_owned = 1;
                continue;
            }

            const uint8_t *sbR, *sbG, *sbB;
            const int sig = synth_source_base(sp, CHAIN_SYNTH_COUNT + c, db,
                                              nb_pixels, &sbR, &sbG, &sbB);
            if (synth_chain_has_no_signal(sp, sig))
            {
                if (ls_bank >= 0)
                    synth_staging_set_inactive(c);
                if (has_lx)
                    synth_staging_luxsynth_set_inactive(c);
                if (has_lw)
                    synth_staging_luxwave_set_inactive(c);
                if (c == pb_chain)
                    pb_no_signal = 1;
                continue;   /* no stream at any position of this chain */
            }

            ChainExecOut ex;
            const int pb_here = (c == pb_chain) ? pb_marker : -1;
            /* LuxSynth staging: single writer — while the player owns the
             * stream, FramePlayerThread stages (lx_send_stage_player_frame). */
            float *lx_line = stream_player_owned ? NULL : s_lx_line;
            if (mod_R && ((sp->has_sampler && c == mod_owner_chain)
                          || (sp->has_score && score_playing_now)))
            {
                /* MOD-BUS OWNER chain (or score relay): the modulated/player
                 * channel IS the stream below the marker — probes/OUTs
                 * observe base or mod by position; the exact pre-marker
                 * processors already ran in the modulated build. Other
                 * sampler chains run POSITIONALLY on their own stream (their
                 * SAMPLER marker is pass-through) — a chain fed by IMAGE
                 * must never display/send the live device flux. */
                chain_shortcut_walk(sp, c,
                                    /*skip_post_marker_probes*/
                                    stream_player_owned && ls_bank >= 0,
                                    pb_here, lx_line,
                                    sbR, sbG, sbB,
                                    mod_R, mod_G, mod_B, nb_pixels, &ex);
                publish_viz_tap_sampler_shortcut(sp, audioBuffers,
                                                 /*player_running*/ 0,
                                                 premarker_tap_done,
                                                 sbR, sbG, sbB,
                                                 mod_R, mod_G, mod_B, nb_pixels);
                if (ls_bank >= 0)
                {
                    PipelineConfig scfg = pipeline_build_config_ls_send(
                        ls_bank, c, /*player_fed*/ 0);
                    pipeline_path_luxstral(ex.lsR, ex.lsG, ex.lsB, &scfg,
                                           &s_ls_send_pp);
                    int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                              ? scfg.pixels_per_note : 1);
                    if (nnotes > PREPROCESS_MAX_NOTES)
                        nnotes = PREPROCESS_MAX_NOTES;
                    synth_staging_stage_luxstral(c, ls_bank, &s_ls_send_pp,
                                                 nnotes, scfg.stereo_enabled);
                    ex.ls_staged = 1;
                }
            }
            else
            {
                chain_execute_positional(sp, c, audioBuffers, sbR, sbG, sbB,
                                         nb_pixels, &s_ls_send_pp, lx_line,
                                         /*player_fed*/ 0, pb_here,
                                         /*allow_sampler_record*/
                                         !player_running_now, &ex);
            }

            if (c == pb_chain && ex.pb_found)
            { pb_R = ex.pbR; pb_G = ex.pbG; pb_B = ex.pbB; pb_found = 1; }

            /* Head-panel engine tap A — published from the FIRST send (M1
             * approximation: the head panel shows one engine input line). */
            if (ex.ls_staged && c == first_send_chain)
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                    ex.lsR, ex.lsG, ex.lsB, nb_pixels);
        }

        if (pipeline_process_frame(src_R, src_G, src_B, &live_cfg, &preprocessed_temp) != 0) {
          log_error("THREAD", "Pipeline processing failed");
        }

        /* Per-engine input tap A — no-LuxStral topologies only (with sends,
         * the chain loop above published it from the first send). While a
         * player runs, FramePlayerThread owns the tap. */
        if (frame_plan.num_ls_sends == 0 && !player_running_now)
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                src_R, src_G, src_B, nb_pixels);

        /* ── Path B (LuxSynth + LuxWave) — fed at its OUT marker position ────
         * The chain loop above captured the stream AT the "→ LUXSYNTH" OUT
         * (fallback: "→ LUXWAVE" when no LuxSynth is placed); this call
         * overrides polyphonic.* + the LuxWave wavetable with that chain's
         * own signal (live_cfg.envelope_id == ENVELOPE_LIVE gates the Chain B
         * freeze envelope here, skipped on the sampler worker). */
#ifdef VST_MODE
        if (pb_chain >= 0)
        {
            if (pb_no_signal)
            {
                /* No source in the Path-B chain → silence for LuxSynth (and
                 * LuxWave's preprocessed mirror): zero their sections instead
                 * of running the path on the live fallback. (The LuxWave
                 * wavetable itself keeps its last content — it only sounds
                 * under held MIDI notes.) */
                memset(&preprocessed_temp.polyphonic, 0,
                       sizeof(preprocessed_temp.polyphonic));
                memset(&preprocessed_temp.photowave,  0,
                       sizeof(preprocessed_temp.photowave));
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                    NULL, NULL, NULL, nb_pixels);
            }
            else if (pb_found)
            {
                /* Per-engine input tap Path-B (per-chain display). A player-
                 * owned pb chain never reaches this branch (skipped in the
                 * loop → pb_found stays 0), so no playback gating is needed:
                 * the plan is the routing authority (M7). */
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                    pb_R, pb_G, pb_B, nb_pixels);

                pipeline_path_luxsynth_luxwave(pb_R, pb_G, pb_B, &live_cfg,
                                               &preprocessed_temp);
            }
            /* else: the Path-B chain is player-owned this line (LuxStral send
             * chain skipped) — the player commit path owns polyphonic. */
            (void) pb_player_owned;
        }
        /* Commit-scope flag (the db commit below runs outside this scope):
         * the Path-B products of preprocessed_temp are valid this line. */
        s_udp_frame_pb_ran = (pb_chain >= 0) && (pb_found || pb_no_signal);
#endif
      }

      /* (Legacy no-op removed: synth_luxwave_set_image_line() resolved to an
       * empty stub — the REAL LuxWave feed is pipeline_path_luxsynth_luxwave
       * above, via luxwave_engine_set_image_line().) */

      pthread_mutex_lock(&db->mutex);
      swapBuffers(db);
      updateLastValidImage(db);

      /* ── M7 — plan-driven commits (no legacy source-type routing) ───────── */
#ifdef VST_MODE
      {
        if (s_udp_frame_ls_sends > 0)
        {
          /* Synth-split P3 — the audio-thread MIXER owns the additive/stereo/
           * strokeforge sections of db->preprocessed_data (N-send blend).
           * Only Path-B products are committed from here (polyphonic has its
           * own dedicated block below; photowave rides along). */
          db->preprocessed_data.photowave = preprocessed_temp.photowave;
        }
        else if (!lux_sampler_is_playing() && lux_sampler_is_passthrough())
        {
          /* No "→ LUXSTRAL" send anywhere: the raw tick is the additive
           * writer (engine disabled by the enable bridge — display paths
           * only). While a player runs, FramePlayerThread owns the commit. */
          db->preprocessed_data = preprocessed_temp;
          db->dataReady = 1;
        }
      }
#else
        db->preprocessed_data = preprocessed_temp;
        db->dataReady = 1;
#endif

      /* LuxSynth polyphonic independent write path — plan-driven (M7): the
       * Path-B products are committed exactly when the pb chain ran this
       * line (pb_found → recomputed, pb_no_signal → zeroed). A player-owned
       * pb chain never runs here — FramePlayerThread owns polyphonic then. */
#ifdef VST_MODE
      if (s_udp_frame_pb_ran)
      {
          db->preprocessed_data.polyphonic = preprocessed_temp.polyphonic;
          if (db->dataReady == 0)
            db->dataReady = 1; /* polyphonic data is now valid */
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
  /* MOD-BUS OWNER: the FIRST chain (model order) hosting a sampler — the
   * modulated channel is built from ITS recipe and ITS OWN source (mirror of
   * udpThread's owner rule). */
  const SynthChainPlan *spSmp = NULL;
  int smp_owner_chain = -1;
  for (int c = 0; c < frame_plan.num_chains && !spSmp; c++)
      if (frame_plan.chain[c].present && frame_plan.chain[c].has_sampler)
      { spSmp = &frame_plan.chain[c]; smp_owner_chain = c; }

  /* Set when the idle build below ran: the sampler chain's pre-marker stream
   * (its modulated channel). The per-synth blocks further down consume it
   * instead of re-running the chain (stateful FX tick once per line). */
  const uint8_t *smpMod_R = NULL, *smpMod_G = NULL, *smpMod_B = NULL;
  int smp_premarker_tap_done = 0;
  if (spSmp)
  {
      static uint32_t s_feeder_line_id = 0;   /* debug/sync id (no UDP line) */
      const uint8_t *sbR, *sbG, *sbB;
      const int sbSig = synth_source_base(spSmp,
                                          CHAIN_SYNTH_COUNT + smp_owner_chain,
                                          db, nb_pixels, &sbR, &sbG, &sbB);

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
              int owner_engine = 0;
              for (int i = 0; i < spSmp->num_inserts; i++)
                  if (spSmp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER)
                  { owner_engine = spSmp->insert_state_idx[i]; break; }
              lux_sampler_on_modulated_frame_ready(owner_engine,
                                                   smpMod_R, smpMod_G,
                                                   smpMod_B,
                                                   (uint16_t)nb_pixels,
                                                   ++s_feeder_line_id);
          }
      }
  }
#endif

  if (!internal_source_any_active())
    return;

  /* ── M3: uniform per-chain loop (internal-source ticks) ───────────────────
   * Mirror of udpThread's chain loop — every internally-fed chain runs once
   * when the device does not stream: probes capture, LuxStral OUTs stage,
   * the Path-B OUT captures its feed. Player-owned LuxStral chains belong to
   * FramePlayerThread; live-sourced chains are skipped (sig == 0 — no fresh
   * frames to observe here). */
  const uint8_t *pb_R = NULL, *pb_G = NULL, *pb_B = NULL;         /* at OUT */
  const uint8_t *pb_base_R = NULL, *pb_base_G = NULL, *pb_base_B = NULL;
  int pb_found = 0;
  int pb_chain = -1, pb_marker = -1;
  int pb_has_score = 0;
  for (int c = 0; c < frame_plan.num_chains; c++)
  {
    const SynthChainPlan *sp = &frame_plan.chain[c];
    if (!sp->present) continue;
    for (int i = 0; i < sp->num_inserts; i++)
    {
      if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
          && pb_marker != IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
      { pb_chain = c; pb_marker = IMAGE_CHAIN_INSERT_OUT_LUXSYNTH; }
      else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE
               && pb_chain < 0)
      { pb_chain = c; pb_marker = IMAGE_CHAIN_INSERT_OUT_LUXWAVE; }
    }
  }
  if (pb_chain >= 0)
    pb_has_score = frame_plan.chain[pb_chain].has_score;
  const int first_send_chain =
      frame_plan.num_ls_sends > 0 ? frame_plan.ls_send[0].chain_idx : -1;

  for (int c = 0; c < frame_plan.num_chains; c++)
  {
    const SynthChainPlan *sp = &frame_plan.chain[c];
    if (!sp->present) continue;

    int ls_bank = -1, has_lx = 0, has_lw = 0;
    for (int i = 0; i < sp->num_inserts; i++)
    {
      if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL && ls_bank < 0)
        ls_bank = sp->insert_state_idx[i];
      else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
        has_lx = 1;
      else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
        has_lw = 1;
    }

    const int stream_player_owned =
        (sp->has_sampler && sampler_playing)
        || (sp->has_score && score_playing);
    if (ls_bank >= 0 && stream_player_owned)
      continue;   /* staged + post-marker probes by FramePlayerThread */

    const uint8_t *sbR, *sbG, *sbB;
    const int sig = synth_source_base(sp, CHAIN_SYNTH_COUNT + c, db,
                                      nb_pixels, &sbR, &sbG, &sbB);
    if (synth_chain_has_no_signal(sp, sig) || sig <= 0)
    {
      /* sig==0 → the live path owns this chain's frames (device streaming
       * edge) — leave everything as-is; a true no-signal chain goes silent. */
      if (synth_chain_has_no_signal(sp, sig))
      {
        if (ls_bank >= 0)
        {
          synth_staging_set_inactive(c);
          if (c == first_send_chain)
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                NULL, NULL, NULL, nb_pixels);
        }
        if (has_lx)
          synth_staging_luxsynth_set_inactive(c);
        if (has_lw)
          synth_staging_luxwave_set_inactive(c);
        if (c == pb_chain)
          audio_image_buffers_publish_engine_input(
              audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
              NULL, NULL, NULL, nb_pixels);
      }
      continue;
    }
    if (c == pb_chain)
    { pb_base_R = sbR; pb_base_G = sbG; pb_base_B = sbB; }

    ChainExecOut ex;
    const int pb_here = (c == pb_chain) ? pb_marker : -1;
    float *lx_line = stream_player_owned ? NULL : s_lx_line_feeder;
#ifdef VST_MODE
    if (c == smp_owner_chain && smpMod_R)
    {
      /* MOD-BUS OWNER chain: consume the pre-marker stream built by the
       * sampler block above (stateful FX tick once per line). Other sampler
       * chains run positionally on their own stream. */
      chain_shortcut_walk(sp, c, /*skip_post_marker_probes*/ 0, pb_here,
                          lx_line,
                          sbR, sbG, sbB,
                          smpMod_R, smpMod_G, smpMod_B, nb_pixels, &ex);
      publish_viz_tap_sampler_shortcut(sp, audioBuffers, /*player_running*/ 0,
                                       smp_premarker_tap_done,
                                       sbR, sbG, sbB,
                                       smpMod_R, smpMod_G, smpMod_B, nb_pixels);
      if (ls_bank >= 0)
      {
        PipelineConfig scfg =
            pipeline_build_config_ls_send(ls_bank, c, /*player_fed*/ 0);
        pipeline_path_luxstral(ex.lsR, ex.lsG, ex.lsB, &scfg,
                               &s_ls_send_pp_feeder);
        int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                  ? scfg.pixels_per_note : 1);
        if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
        synth_staging_stage_luxstral(c, ls_bank, &s_ls_send_pp_feeder,
                                     nnotes, scfg.stereo_enabled);
        ex.ls_staged = 1;
      }
    }
    else
#endif
    {
      chain_execute_positional(sp, c, audioBuffers, sbR, sbG, sbB,
                               nb_pixels, &s_ls_send_pp_feeder, lx_line,
                               /*player_fed*/ 0, pb_here,
                               /*allow_sampler_record*/ !sampler_playing, &ex);
    }

    if (c == pb_chain && ex.pb_found)
    { pb_R = ex.pbR; pb_G = ex.pbG; pb_B = ex.pbB; pb_found = 1; }

    /* MODULATED display bus + engine tap A — from the first send (mirror of
     * udpThread; the sampler chain owns the modulated bus when present). */
    if (ex.ls_staged && c == first_send_chain
        && !(sp->has_score && score_playing))
    {
#ifdef VST_MODE
      if (!smpMod_R)   /* sampler chain owns the bus — published above */
#endif
      audio_image_buffers_snapshot_modulated(audioBuffers,
                                             ex.lsR, ex.lsG, ex.lsB,
                                             nb_pixels);
      audio_image_buffers_publish_engine_input(
          audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
          ex.lsR, ex.lsG, ex.lsB, nb_pixels);
    }
  }

  /* ── Path B (LuxSynth + LuxWave) — fed at its OUT marker position ────────── */
  int pb_done = 0;
  if (pb_chain >= 0 && pb_found)
  {
    /* Same MODULATED publish for a Path-B-only session (legacy CHAIN-1
     * panel): only when no LuxStral send exists — with sends, the loop above
     * owns the (single, global) bus. */
    if (frame_plan.num_ls_sends == 0 && !sampler_playing
        && !(pb_has_score && score_playing)
#ifdef VST_MODE
        && !smpMod_R
#endif
       )
      audio_image_buffers_snapshot_modulated(audioBuffers, pb_R, pb_G, pb_B,
                                             nb_pixels);

    /* Per-engine input tap Path-B (per-chain display). A player-owned pb
     * chain never reaches here (skipped in the loop → pb_found stays 0) —
     * the plan is the routing authority (M7). */
    audio_image_buffers_publish_engine_input(
        audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
        pb_R, pb_G, pb_B, nb_pixels);

    PipelineConfig cfg = pipeline_build_config_live();
    pipeline_path_luxsynth_luxwave(pb_R, pb_G, pb_B, &cfg, &s_feeder_pp);
    pb_done = 1;
  }

  if (!pb_done)
    return;

  /* ── Display / visual buses — mirror the device behaviour so the CIS
   * visualizer, waterfall and RAW snapshot follow the internal source. The
   * display line is the Path-B chain's source frame. */
  const uint8_t *dispR = pb_base_R;
  const uint8_t *dispG = pb_base_G;
  const uint8_t *dispB = pb_base_B;

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

  /* Path-B products only: the audio-thread mixer owns the additive sections
   * (LuxStral sends), never clobber them with stale scratch. */
  db->preprocessed_data.polyphonic = s_feeder_pp.polyphonic;
  if (db->dataReady == 0)
    db->dataReady = 1;

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
    /* Synth-split P3: sends present → pull-mix into the single engine feed;
     * no send → legacy single-engine render. */
    {
      ChainPlan planB_render;
      chain_plan_get(&planB_render);

      /* M4 — LuxSynth engine feed: mix the staged "→ LUXSYNTH" sends, run ONE
       * FFT and push the spectral data (cheap no-op when nothing changed).
       * Replaces the UI-thread bridge — the engine sounds with the editor
       * closed. */
      luxsynth_feed_tick(&planB_render);

      /* M5 — LuxWave wavetable feed: bipolar mix of the staged "→ LUXWAVE"
       * sends + Chain-2 transport envelope. */
      pipeline_luxwave_feed_tick(&planB_render);

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
        /* M7 — dataReady is a plain has-data flag (source tags removed). */
        mdb->dataReady = 1;
        pthread_mutex_unlock(&mdb->mutex);

        synth_AudioProcess(audio_read_R, audio_read_G, audio_read_B,
                           context->doubleBuffer);
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
