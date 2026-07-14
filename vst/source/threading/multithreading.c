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
static int s_udp_frame_pb_ran = 0;     /* udpThread-only: Path-B products of
                                        * preprocessed_temp valid this line */

/* ── Per-chain playback (2026-07-12) ─────────────────────────────────────────
 * Does this chain host `engine`'s SAMPLER? (marker slot = engine A=0/B=1).
 * The player-ownership gates match the PLAYING engine against the chain's
 * own marker — a chain hosting the idle engine keeps its positional run.
 * (Outside VST_MODE the engine is always -1 → never player-owned.) */
static int chain_hosts_sampler_engine(const SynthChainPlan *sp, int engine)
{
    if (engine < 0 || !sp->has_sampler)
        return 0;
    for (int i = 0; i < sp->num_inserts; i++)
        if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
            && sp->insert_state_idx[i] == engine)
            return 1;
    return 0;
}

/* Multi-chain split (2026-07-13): does this chain host ANY driving engine?
 * Per-chain player-ownership gate — with the arbiter scoped to shared-chain
 * topologies, TWO engines on TWO chains may drive simultaneously; matching
 * only the "first playing engine" starved the second chain's gates.
 * (Outside VST_MODE no engine ever drives → never player-owned.) */
static int chain_hosts_driving_engine(const SynthChainPlan *sp)
{
#ifdef VST_MODE
    if (!sp->has_sampler)
        return 0;
    for (int i = 0; i < sp->num_inserts; i++)
        if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER
            && lux_sampler_engine_is_driving(sp->insert_state_idx[i]))
            return 1;
    return 0;
#else
    (void)sp;
    return 0;
#endif
}

#ifdef VST_MODE
/* One predicate for every player-ownership gate: SCORE relay matches
 * has_score (channel-wide, never engine-matched); sampler playback matches
 * the chain's own SAMPLER marker against the playing engine. */
static int chain_player_owned(const SynthChainPlan *sp, int is_score,
                              int engine_slot)
{
    return is_score ? sp->has_score
                    : chain_hosts_sampler_engine(sp, engine_slot);
}

/* (P4-M2) The player-side execution — post-marker OUT staging, FX/probes,
 * downstream record, taps — is chain_player_execute_owned(), defined below
 * chain_execute_span (the one chain walker). */

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

  // Initialize every image buffer with WHITE (blank paper = the empty-chain
  // contract). active/processing were previously left as malloc garbage.
  memset(db->activeBuffer_R, 0xFF, nb_pixels);
  memset(db->activeBuffer_G, 0xFF, nb_pixels);
  memset(db->activeBuffer_B, 0xFF, nb_pixels);
  memset(db->processingBuffer_R, 0xFF, nb_pixels);
  memset(db->processingBuffer_G, 0xFF, nb_pixels);
  memset(db->processingBuffer_B, 0xFF, nb_pixels);
  memset(db->lastValidImage_R, 0xFF, nb_pixels);
  memset(db->lastValidImage_G, 0xFF, nb_pixels);
  memset(db->lastValidImage_B, 0xFF, nb_pixels);

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
  
  /* (photowave init removed with the dead struct section) */
  
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
    // No valid image yet → WHITE (blank paper — silence under the permanent
    // inverse-dB law, matching the unfed-chain contract).
    memset(out_R, 0xFF, nb_pixels);
    memset(out_G, 0xFF, nb_pixels);
    memset(out_B, 0xFF, nb_pixels);
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

/* Shared blank-paper line — the empty-chain contract is WHITE, never black. */
static const uint8_t *chain_white_line(void)
{
    static uint8_t s_white[INTERNAL_SRC_MAX_PIXELS];
    static volatile int s_init = 0;
    if (!s_init)
    {
        memset((void *) s_white, 0xFF, sizeof(s_white));
        s_init = 1;   /* benign race: every writer stores the same bytes */
    }
    return s_white;
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
 *        out_* point at the shared WHITE line (P4-M1): a caller that forgets
 *        to test the code walks blank paper — a chain can never leak the live
 *        device feed (only an explicit SP3CTRA source, or a sampler/score
 *        player actually running, may carry signal).
 *
 * The scratch is written by whichever thread currently drives the per-synth
 * processing (udpThread while the device streams, the feeder tick otherwise —
 * see internal_source_live_streaming()). The 250 ms hand-over hysteresis makes
 * concurrent writes to the same slot practically impossible; a glitched frame
 * at the boundary is acceptable.
 *
 * One slot per MODEL CHAIN (P4-M4 — the legacy synth[] recipes are gone). */
static uint8_t s_synth_src_scratch[CHAIN_MAX_CHAINS][3][INTERNAL_SRC_MAX_PIXELS];

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
        if (synth_slot < 0 || synth_slot >= CHAIN_MAX_CHAINS)
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
    /* Internal module placed but empty/inactive, or no source module at all →
     * the chain has no signal of its own: blank-paper pointers, NEVER the
     * live device feed (P4-M1 — the old live fallback was the trap behind
     * the 2026-07-13 idle-score leak). */
    if (kind >= 0 || sp->source_kind == CHAIN_SRC_NONE)
    {
        const uint8_t *w = chain_white_line();
        *out_r = w; *out_g = w; *out_b = w;
        return -1;
    }
    *out_r = db->activeBuffer_R;
    *out_g = db->activeBuffer_G;
    *out_b = db->activeBuffer_B;
    return 0;
}

/* True when a synth chain carries NO signal THIS LINE: its base source
 * resolved to "no signal" AND no player is ACTUALLY substituting a stream
 * right now. The substitution test must be dynamic: the score player feeds
 * every has_score chain only while it RUNS, a sampler feeds a chain only
 * while that chain's own engine is DRIVING. The old static exclusion (the
 * has_sampler/has_score plan flags) declared a SCORE/sampler chain "fed"
 * even with its player idle — the executor then ran the chain on
 * synth_source_base's live fallback pointers, leaking the SP3CTRA device
 * feed into a chain with no input of its own (audible on its OUT sends,
 * visible in its probes). */
static int synth_chain_has_no_signal(const SynthChainPlan *sp, int base_sig,
                                     int score_playing)
{
    if (!sp->present || base_sig >= 0)
        return 0;
    if (sp->has_score && score_playing)
        return 0;
    if (chain_hosts_driving_engine(sp))
        return 0;
    return 1;
}

/* ── Chain "no signal" publication ───────────────────────────────────────────
 * A chain lost its feed (source module removed / deactivated / emptied): make
 * the silence OBSERVABLE everywhere the chain is. Idempotent and cheap (a few
 * memsets + seqlocked flag stores) — safe to call every line/tick while the
 * chain stays silent. WHITE, never black, is the empty-chain contract: an
 * unfed chain streams blank paper.
 *   • staging slots → inactive (the audio-thread mixers commit silence),
 *   • LUXSTRAL-A engine tap → white when this chain owns it (blank paper);
 *     the Path-B tap stays at the call sites (udpThread batches it after the
 *     loop, the feeder publishes it in-loop via `is_pb_chain`),
 *   • zone-1 selection tap → white when the selected module lives here,
 *   • VideoScroll probes → capture a WHITE line so the waterfall scrolls to
 *     blank instead of freezing on the removed source's last frames. */
static void chain_publish_no_signal(const SynthChainPlan *sp, int chain_idx,
                                    AudioImageBuffers *audioBuffers,
                                    int nb_pixels,
                                    int is_first_send_chain, int is_pb_chain)
{
    const uint8_t *s_white_line = chain_white_line();

    int ls_bank = -1, has_lx = 0, has_lw = 0;
    for (int i = 0; i < sp->num_inserts; i++)
    {
        const int id = sp->insert_id[i];
        if (id == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL && ls_bank < 0)
            ls_bank = sp->insert_state_idx[i];
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
            has_lx = 1;
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
            has_lw = 1;
        else if (id == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
            video_scroll_capture_line(
                video_scroll_instance(sp->insert_state_idx[i]),
                s_white_line, s_white_line, s_white_line, nb_pixels);
    }

    if (ls_bank >= 0)
    {
        synth_staging_set_inactive(chain_idx);
        if (is_first_send_chain && audioBuffers != NULL)
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                NULL, NULL, NULL, nb_pixels);
    }
    if (has_lx)
        synth_staging_luxsynth_set_inactive(chain_idx);
    if (has_lw)
        synth_staging_luxwave_set_inactive(chain_idx);
    if (is_pb_chain && audioBuffers != NULL)
        audio_image_buffers_publish_engine_input(
            audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
            NULL, NULL, NULL, nb_pixels);
    if (sp->viz_tap_insert >= 0 && audioBuffers != NULL)
        audio_image_buffers_clear_selection_tap(audioBuffers);
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
    const uint8_t *endR, *endG, *endB; /* stream AFTER the last processed insert
                                        * (== the walk's input when the span is
                                        * empty) — the chain's outgoing flux */
} ChainExecOut;

/* ── P4-M2 — THE chain executor ──────────────────────────────────────────────
 * chain_execute_span() is the only code that knows how to run a chain: walk
 * the recipe positions [from, to) on one stream, handling EVERY insert id at
 * its exact position — selection tap, OUT staging (LuxStral/LuxSynth/LuxWave),
 * SAMPLER record, probes/processors via image_chain_run, pass-through markers.
 * Every producer (udpThread, feeder, FramePlayerThread) calls it with its own
 * span and scratches; the walk itself is identical everywhere — that is the
 * doctrine: one flux per chain, module order is the law, no special cases. */

/* SAMPLER-marker record behaviour inside a span. NONE is the zero value so a
 * designated-initialized ChainExecCtx that omits rec_mode records nothing —
 * no call site selects it explicitly today. */
#define CHAIN_REC_NONE  0   /* markers pass through silently */
#define CHAIN_REC_IDLE  1   /* idle per-chain record (lux_sampler_record_chain_frame) */
#define CHAIN_REC_INPUT 2   /* playback input record (lux_sampler_record_input_frame) */

typedef struct {
    AudioImageBuffers     *viz_bus;      /* selection tap (NULL = no taps) */
    PreprocessedImageData *pp_scratch;   /* LuxStral send preprocess scratch */
    float                 *lx_line;      /* LuxSynth/LuxWave line scratch;
                                          * NULL = skip their staging */
    int player_fed;          /* LS send conditioning base (sampler transport) */
    int force_play;          /* player transport override (sequencer/score) */
    int pb_marker_id;        /* Path-B OUT id to report in `out`, -1 = none */
    int rec_mode;            /* CHAIN_REC_* */
    int rec_skip_engine;     /* CHAIN_REC_INPUT: engine that never records
                              * (the driving engine must not record its own
                              * playback); -1 = record every armed marker */
    int publish_tap_at_end;  /* 1 = a tap at position `to` (== num_inserts)
                              * publishes the final stream; 0 = the span owner
                              * below `to` publishes it (pre-marker segments) */
} ChainExecCtx;

static void chain_execute_span(const SynthChainPlan *sp, int chain_idx,
                               int from, int to, const ChainExecCtx *cx,
                               const uint8_t *inR, const uint8_t *inG,
                               const uint8_t *inB, int nb_pixels,
                               ChainExecOut *out)
{
    void *states[CHAIN_PLAN_MAX_INSERTS];
    chain_resolve_insert_states(sp, states);

    const uint8_t *cr = inR, *cg = inG, *cb = inB;
    int tap = sp->viz_tap_insert;
    if (tap > sp->num_inserts)
        tap = sp->num_inserts;
    if (from < 0) from = 0;
    if (to > sp->num_inserts) to = sp->num_inserts;

    memset(out, 0, sizeof(*out));

    for (int i = from; i <= to; i++)
    {
        if (cx->viz_bus != NULL && tap == i
            && (i < to || (cx->publish_tap_at_end && to == sp->num_inserts)))
            audio_image_buffers_publish_selection_tap(cx->viz_bus, cr, cg, cb,
                                                      nb_pixels);
        if (i == to)
            break;

        const int id = sp->insert_id[i];
        if (id == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL)
        {
            const int bank = sp->insert_state_idx[i];
            PipelineConfig scfg =
                pipeline_build_config_ls_send(bank, chain_idx, cx->player_fed);
            if (sp->has_sampler || sp->has_score)
            {
                /* M8 — sampler/score-relayed chain: RAW gate skipped, the
                 * crossfader opacity applies (legacy parity). */
                scfg.sampler_relayed = 1;
                scfg.stream_opacity  = g_sp3ctra_config.image_live_opacity;
            }
            if (cx->force_play)
                scfg.freeze_mode = 0; /* PLAY — sequencer/score drives transport */
            pipeline_path_luxstral(cr, cg, cb, &scfg, cx->pp_scratch);
            int nnotes = nb_pixels / (scfg.pixels_per_note > 0
                                      ? scfg.pixels_per_note : 1);
            if (nnotes > PREPROCESS_MAX_NOTES) nnotes = PREPROCESS_MAX_NOTES;
            synth_staging_stage_luxstral(chain_idx, bank, cx->pp_scratch,
                                         nnotes, scfg.stereo_enabled);
            if (!out->ls_staged)
            { out->lsR = cr; out->lsG = cg; out->lsB = cb; out->ls_staged = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
        {
            /* M4 — stage the conditioned line + RGB at the send's position;
             * the audio thread mixes and runs ONE FFT (luxsynth_feed_tick). */
            if (cx->lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxsynth_condition_line(cr, cg, cb, bank, cx->lx_line,
                                        nb_pixels);
                synth_staging_stage_luxsynth(chain_idx, bank, cx->lx_line,
                                             cr, cg, cb, nb_pixels);
            }
            if (cx->pb_marker_id == id && !out->pb_found)
            { out->pbR = cr; out->pbG = cg; out->pbB = cb; out->pb_found = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
        {
            /* M5 — stage the conditioned wavetable line at the send's
             * position; the audio thread pulls the bipolar mix. */
            if (cx->lx_line != NULL)
            {
                const int bank = sp->insert_state_idx[i];
                luxwave_condition_line(cr, cg, cb, bank, cx->lx_line,
                                       nb_pixels);
                synth_staging_stage_luxwave(chain_idx, bank, cx->lx_line,
                                            nb_pixels);
            }
            if (cx->pb_marker_id == id && !out->pb_found)
            { out->pbR = cr; out->pbG = cg; out->pbB = cb; out->pb_found = 1; }
        }
        else if (id == IMAGE_CHAIN_INSERT_SAMPLER)
        {
            /* SAMPLER marker = replacer module. Pass-through for the walk
             * (an idle sampler forwards the upstream flow; a DRIVING one owns
             * the span BELOW it — the caller's span simply ends there). Its
             * INPUT is what recording captures (module contract 2026-07-13):
             *   IDLE  — the chain's own stream into ITS engine's armed slot;
             *   INPUT — playback capture (pre-marker input / downstream
             *           bounce+resampling), never the driving engine itself. */
#ifdef VST_MODE
            if (cx->rec_mode == CHAIN_REC_IDLE)
                lux_sampler_record_chain_frame(sp->insert_state_idx[i],
                                               cr, cg, cb,
                                               (uint16_t) nb_pixels);
            else if (cx->rec_mode == CHAIN_REC_INPUT
                     && sp->insert_state_idx[i] != cx->rec_skip_engine)
                lux_sampler_record_input_frame(sp->insert_state_idx[i],
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
    out->endR = cr; out->endG = cg; out->endB = cb;
}

/* Position of the marker whose player OWNS the stream below it, -1 if none.
 * THE single split-point definition — producers and players MUST agree on it,
 * or the pre-marker and post-marker spans gap/overlap.
 *   score_owns   — the score channel plays AND owns this chain (has_score):
 *                  the first SCORE-type marker wins (SCORE/TIMBRE/MIDI SCORE/
 *                  VOICE share the relay, which owns the whole channel).
 *   engine_slot  — >= 0: that exact engine's SAMPLER marker (the caller IS
 *                  its player); -1: any DRIVING engine's marker (producers,
 *                  which only know "some player owns this stream").
 * KNOWN GAP (pre-existing arbitration hole, see PLAN_P4 M3): a chain hosting
 * a playing score AND a driving sampler whose engine does not share a chain
 * with the score's engine is not arbitrated — both players walk overlapping
 * spans and double-stage the same OUTs. The relay eviction covers the shared-
 * chain cases only. */
static int chain_owning_marker_pos(const SynthChainPlan *sp, int score_owns,
                                   int engine_slot)
{
    for (int i = 0; i < sp->num_inserts; i++)
    {
        const int id = sp->insert_id[i];
        if (score_owns)
        {
            if (id == IMAGE_CHAIN_INSERT_SCORE)
                return i;
        }
        else if (id == IMAGE_CHAIN_INSERT_SAMPLER)
        {
            if (engine_slot >= 0)
            {
                if (sp->insert_state_idx[i] == engine_slot)
                    return i;
            }
#ifdef VST_MODE
            else if (lux_sampler_engine_is_driving(sp->insert_state_idx[i]))
                return i;
#endif
        }
    }
    return -1;
}

#ifdef VST_MODE
/* ── P4-M2 — FramePlayerThread: ONE positional walk per owned chain ──────────
 * Replaces the four player-side paths (per-send private copies + engine-A
 * in-place inserts + LuxSynth first-OUT staging + downstream record): for
 * every chain owned by THIS player (its driving engine's SAMPLER marker, or
 * the SCORE-type marker during score playback), walk the span BELOW the
 * owning marker on the blended playback frame. The walk stages every OUT at
 * its exact position, ticks post-marker FX/probes exactly ONCE per frame
 * (the old per-send copies + in-place engine-A run double-ticked stateful FX
 * and double-captured probes between marker and OUT), records the downstream
 * SAMPLER markers at their position (SCORE→sampler bounce, A→B resampling —
 * never the driving engine itself) and publishes the exact selection tap.
 * The stream at the FIRST owned LuxStral OUT is copied back into r/g/b AFTER
 * the loop and published as engine tap A, so the caller's display mix bus and
 * legacy audio commits see the post-FX frame (parity with the old in-place
 * engine-A inserts). Returns plan.num_ls_sends (legacy-path gate). */
int chain_player_execute_owned(int is_score, int engine_slot, int force_play,
                               struct AudioImageBuffers *viz_bus,
                               uint8_t *r, uint8_t *g, uint8_t *b,
                               int nb_pixels)
{
    /* Per-thread: engines A and B each run their OWN FramePlayerThread and
     * may tick simultaneously (cross-chain playback) — a shared scratch would
     * mix their conditioning mid-frame. */
    static _Thread_local PreprocessedImageData s_pp_player;
    static _Thread_local float s_lx_player[CIS_MAX_PIXELS_NB];

    ChainPlan plan;
    chain_plan_get(&plan);
    if (nb_pixels > CIS_MAX_PIXELS_NB)
        nb_pixels = CIS_MAX_PIXELS_NB;

    const uint8_t *dispR = NULL, *dispG = NULL, *dispB = NULL;
    for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; c++)
    {
        const SynthChainPlan *sp = &plan.chain[c];
        if (!sp->present || !chain_player_owned(sp, is_score, engine_slot))
            continue;
        /* Same split-point definition as the producers' pre-marker spans. */
        const int own_mk = chain_owning_marker_pos(sp, is_score, engine_slot);
        if (own_mk < 0)
            continue;

        ChainExecCtx cx = {
            .viz_bus            = viz_bus,
            .pp_scratch         = &s_pp_player,
            .lx_line            = s_lx_player,
            .player_fed         = 1,
            .force_play         = force_play,
            .pb_marker_id       = -1,
            .rec_mode           = CHAIN_REC_INPUT,
            .rec_skip_engine    = is_score ? -1 : engine_slot,
            .publish_tap_at_end = 1,
        };
        ChainExecOut ex;
        chain_execute_span(sp, c, own_mk + 1, sp->num_inserts, &cx,
                           r, g, b, nb_pixels, &ex);

        if (dispR == NULL && ex.ls_staged)
        {
            /* First owned LS-OUT chain = the display/commit chain. The engine
             * tap shows the stream AT the OUT (what the engine hears); the
             * display/commit write-back is the chain's END stream — inserts
             * placed BELOW the OUT included (parity with the old in-place
             * engine-A run over the full post-marker sub-plan). Pool-instance
             * output buffers stay valid across the remaining iterations
             * (per-instance pools — no other chain touches them). */
            dispR = ex.endR; dispG = ex.endG; dispB = ex.endB;
            if (viz_bus != NULL)
                audio_image_buffers_publish_engine_input(
                    viz_bus, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                    ex.lsR, ex.lsG, ex.lsB, nb_pixels);
        }
    }
    /* Write-back AFTER the loop: r/g/b are every owned chain's BASE — mutating
     * them mid-loop would leak chain 1's FX into chain 2's walk. */
    if (dispR != NULL)
    {
        if (dispR != r) memcpy(r, dispR, (size_t) nb_pixels);
        if (dispG != g) memcpy(g, dispG, (size_t) nb_pixels);
        if (dispB != b) memcpy(b, dispB, (size_t) nb_pixels);
    }
    return plan.num_ls_sends;
}
#endif /* VST_MODE */


/* ── M7 — plan-driven ownership queries (replace the *_source_type gates) ────
 * Used by FramePlayerThread/LuxSampler: which db sections may the player own?
 * Per-chain playback: the sampler case matches the chain's SAMPLER marker
 * against the CALLING player's engine — the other engine's playback never
 * grants ownership of this chain. Cheap (one plan snapshot + scan); Non-RT
 * callers only. */
int chain_additive_player_candidate(int is_score, int engine_slot)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    if (plan.num_ls_sends > 0)
        return chain_player_owned(&plan.ls_send[0].recipe, is_score,
                                  engine_slot);
    /* (P4-M4) 0 sends → the mixer commits SILENCE (D1); the additive
     * sections are never player-owned. */
    return 0;
}

int chain_pathb_player_candidate(int is_score, int engine_slot)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    for (int c = 0; c < plan.num_chains; c++)
    {
        const SynthChainPlan *sp = &plan.chain[c];
        if (!sp->present) continue;
        for (int i = 0; i < sp->num_inserts; i++)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                return chain_player_owned(sp, is_score, engine_slot);
    }
    return 0;
}

/* ── Player stop → staging silence (scrub/stop contract, 2026-07-13) ─────────
 * The synth-split stagings have NO freshness timeout: the audio-thread mixer
 * keeps blending the LAST staged column until someone re-stages or deactivates
 * it. On a sourceless player chain (e.g. MIDI SCORE with no source module) the
 * per-line producers skip the chain entirely (sig <= 0), so when the player
 * stops feeding, the last column would ring FOREVER. Called from
 * injectWhiteFrame(): deactivate the stagings of every chain THIS player
 * relayed (its own engine's sampler chains + any score chain). Live/internal
 * chains keep their own producers. Same cross-engine imprecision as the
 * addOwned/pbOwned gates there: if the OTHER engine still plays one of these
 * chains, its next 1 ms tick re-stages — a self-healing one-tick dropout. */
void chain_player_stagings_set_inactive(int engine_slot)
{
    ChainPlan plan;
    chain_plan_get(&plan);
    for (int k = 0; k < plan.num_ls_sends && k < CHAIN_MAX_CHAINS; k++)
    {
        const LsSendPlan *snd = &plan.ls_send[k];
        if (chain_player_owned(&snd->recipe, 0, engine_slot)
            || chain_player_owned(&snd->recipe, 1, engine_slot))
            synth_staging_set_inactive(snd->chain_idx);
    }
    for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; c++)
    {
        const SynthChainPlan *sp = &plan.chain[c];
        if (!sp->present) continue;
        if (!chain_player_owned(sp, 0, engine_slot)
            && !chain_player_owned(sp, 1, engine_slot))
            continue;
        for (int i = 0; i < sp->num_inserts; i++)
        {
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                synth_staging_luxsynth_set_inactive(c);
            else if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
                synth_staging_luxwave_set_inactive(c);
        }
    }
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
  /* NOTE(write bus): the line is assembled fragment-by-fragment in
   * db->activeBuffer only; the AudioImageBuffers write bus is filled in ONE
   * shot at line completion.  The old scheme held write_mutex from fragment 1
   * to fragment 12 (start_write at new-line, complete_write at completion) —
   * across up to 11 blocking recvfrom calls.  A stream stall mid-line then
   * kept the mutex held indefinitely, freezing every other start_write caller
   * (FramePlayerThread injection, feeder tick).  Same bytes copied in total;
   * lock hold is now ~µs. */
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
      /* (No write_mutex to release: the write bus is only touched — lock,
       * memcpy, swap, unlock — at line completion now.) */
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
        /* Incomplete lines are simply never published to the write bus now
         * (the old scheme had to complete_write a PARTIAL line just to
         * release the mutex — readers then saw a torn line; they now keep
         * the last complete one). */
      }

      // New line started - prepare for assembly (db->activeBuffer only; the
      // audio write bus is filled in one locked shot at line completion).
      currentLineId = packet.line_id;
      memset(receivedFragments, 0, UDP_MAX_NB_PACKET_PER_LINE * sizeof(int));
      fragmentCount = 0;
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

      // Assemble the line in the double buffer (display + single source for
      // the one-shot audio-bus publish at completion below).
      memcpy(&db->activeBuffer_R[offset], packet.imageData_R,
             packet.fragment_size);
      memcpy(&db->activeBuffer_G[offset], packet.imageData_G,
             packet.fragment_size);
      memcpy(&db->activeBuffer_B[offset], packet.imageData_B,
             packet.fragment_size);
    }

#ifdef DEBUG_UDP
    log_debug("UDP", "Fragment count: %u/%u for line %u", fragmentCount, packet.total_fragments, packet.line_id);
#endif

    if (fragmentCount == packet.total_fragments) {
      PreprocessedImageData preprocessed_temp;

      // 🔧 CRITICAL FIX: Abort heavy processing if stop requested
      if (!ctx->running) {
        log_info("THREAD", "ctx->running=0 detected before heavy processing, exiting");
        /* (No write_mutex held here anymore — nothing to release.) */
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
           * waterfall, display, AudioImageBuffers) sees the same held data —
           * the audio-bus publish below copies FROM activeBuffer, so it sees
           * the held frame too. */
          memcpy(db->activeBuffer_R, held_R, nb_pixels);
          memcpy(db->activeBuffer_G, held_G, nb_pixels);
          memcpy(db->activeBuffer_B, held_B, nb_pixels);
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

      /* Publish the complete line to the audio write bus — ONE locked shot
       * (start_write → 3 memcpy → complete_write ≈ µs), copying from the
       * just-assembled (and possibly gate-held) activeBuffer.  The lock is
       * no longer held across packet reception (see the note at the top of
       * this function).  During LuxSampler playback the live publish is
       * skipped: FramePlayerThread is the sole writer (same ownership rule
       * as before, now decided at completion instead of at line start).
       * FIX(routing): Snapshot raw BEFORE complete_write so that raw_R/G/B
       * always contains pure UDP data (post-swap, FramePlayerThread could
       * race the two calls and contaminate raw_R/G/B with sampler data). */
      {
        int published = 0;
#ifdef VST_MODE
        if (!lux_sampler_is_playing())
#endif
        {
          uint8_t *wR = NULL, *wG = NULL, *wB = NULL;
          if (audio_image_buffers_start_write(audioBuffers, &wR, &wG, &wB) == 0) {
            memcpy(wR, db->activeBuffer_R, nb_pixels);
            memcpy(wG, db->activeBuffer_G, nb_pixels);
            memcpy(wB, db->activeBuffer_B, nb_pixels);
            audio_image_buffers_snapshot_raw_before_swap(audioBuffers);
            audio_image_buffers_complete_write(audioBuffers);
            published = 1;
          } else {
            log_warning("THREAD", "Failed to start audio buffer write");
          }
        }
        if (!published) {
          /* FIX(raw): Write bus not published (sampler is playing and owns
           * AudioImageBuffers), but the RAW snapshot must still reflect the
           * live UDP data so the RAW visualizer and Source=L pipeline path
           * stay live during sampler playback. */
          audio_image_buffers_snapshot_raw_external(audioBuffers,
              db->activeBuffer_R, db->activeBuffer_G, db->activeBuffer_B,
              nb_pixels);
        }
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

        /* (P4-M3) The global MODULATED bus is DEAD as a routing channel: every
         * sampler chain — owner included — executes through the uniform
         * per-chain loop below. Its SAMPLER marker records the chain stream
         * at its exact position (CHAIN_REC_IDLE → lux_sampler_record_chain_
         * frame), and the zone-1 view follows the selection tap (D2). No
         * dedicated modulated build, no shortcut walk, no PLAYING re-copy. */

        /* (P4-M4) The legacy no-send live tick is DEAD in VST: the audio
         * mixer is the additive writer in EVERY topology (0 sends → silence,
         * D1 — no "→ LUXSTRAL" module = unfed engine). The full pipeline
         * only survives for the non-VST standalone, fed by the raw live
         * frame. */
        src_R = db->activeBuffer_R;
        src_G = db->activeBuffer_G;
        src_B = db->activeBuffer_B;
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

            /* This chain's LuxStral OUT bank (V1: at most one OUT per type
             * per chain). LuxSynth/LuxWave presence is rescanned by
             * chain_publish_no_signal() on the silent branch. */
            int ls_bank = -1;
            for (int i = 0; i < sp->num_inserts; i++)
                if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL)
                { ls_bank = sp->insert_state_idx[i]; break; }

            /* Per-chain playback: this chain is player-owned when ITS OWN
             * engine is driving (or the SCORE relay for a score chain) — a
             * chain hosting an idle engine keeps its positional run on its
             * own stream. Per-engine query: BOTH chains may be player-owned
             * simultaneously (multi-chain split). */
            const int stream_player_owned =
                chain_hosts_driving_engine(sp)
                || (sp->has_score && score_playing_now);

            if (stream_player_owned)
            {
                /* P4-M2 — EVERY player-owned chain (LS OUT or not) splits at
                 * its owning marker: FramePlayerThread walks the span BELOW
                 * it on the playback frames (OUT staging, FX/probes, taps,
                 * downstream record — chain_player_execute_owned); the
                 * segment ABOVE keeps running here at line rate: the chain
                 * source feeds the upstream probes/OUTs and every SAMPLER
                 * marker records its INPUT into its armed engine. (The old
                 * ls_bank gate sent non-LS owned chains through the shortcut
                 * walk — their post-marker probes are player-captured now.) */
                {
                    const uint8_t *pmR, *pmG, *pmB;
                    const int pmSig = synth_source_base(
                        sp, c, db, nb_pixels,
                        &pmR, &pmG, &pmB);
                    const int own_mk = chain_owning_marker_pos(
                        sp, score_playing_now && sp->has_score, -1);
                    if (pmSig >= 0 && own_mk >= 0)
                    {
                        /* Pre-marker span [0, marker]: upstream probes/FX run
                         * at line rate, OUTs above the marker stage HERE (the
                         * player only owns — and stages — the stream BELOW
                         * its marker), and every SAMPLER marker in the span
                         * records its INPUT. Designated init zero-fills the
                         * rest (publish_tap_at_end=0, force_play=0…). */
                        ChainExecCtx pcx = {
                            .viz_bus         = audioBuffers,
                            .pp_scratch      = &s_ls_send_pp,
                            .lx_line         = s_lx_line,
                            .pb_marker_id    = -1,
                            .rec_mode        = CHAIN_REC_INPUT,
                            .rec_skip_engine = -1,
                        };
                        ChainExecOut pex;
                        chain_execute_span(sp, c, 0, own_mk + 1, &pcx,
                                           pmR, pmG, pmB, nb_pixels, &pex);
                        /* Above-marker LS OUT staged here → keep the head
                         * panel/waterfall live at line rate (the player's
                         * walk only publishes tap A for BELOW-marker OUTs). */
                        if (pex.ls_staged && c == first_send_chain)
                            audio_image_buffers_publish_engine_input(
                                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                                pex.lsR, pex.lsG, pex.lsB, nb_pixels);
                    }
                    else if (own_mk >= 0 && audioBuffers != NULL
                             && sp->viz_tap_insert >= 0
                             && sp->viz_tap_insert <= own_mk)
                        /* Sourceless owned chain: the pre-marker span carries
                         * no stream — the selection tap shows blank paper
                         * instead of freezing on its last frame. */
                        audio_image_buffers_clear_selection_tap(audioBuffers);
                }
                if (c == pb_chain) pb_player_owned = 1;
                continue;
            }

            const uint8_t *sbR, *sbG, *sbB;
            const int sig = synth_source_base(sp, c, db,
                                              nb_pixels, &sbR, &sbG, &sbB);
            if (synth_chain_has_no_signal(sp, sig, score_playing_now))
            {
                /* Path-B tap: published by the pb_no_signal block after the
                 * loop (is_pb_chain = 0 here — no double publish). */
                chain_publish_no_signal(sp, c, audioBuffers, nb_pixels,
                                        c == first_send_chain,
                                        /*is_pb_chain*/ 0);
                if (c == pb_chain)
                    pb_no_signal = 1;
                continue;   /* no stream at any position of this chain */
            }
            /* sig < 0 with a player substituting: sbR/G/B already point at
             * the white line (synth_source_base P4-M1 contract) — pre-marker
             * positions observe blank paper, never the device feed. */

            ChainExecOut ex;
            const int pb_here = (c == pb_chain) ? pb_marker : -1;
            /* (P4-M3) ONE path: the full positional span [0, N] — sampler
             * chains included (their marker records the chain stream at its
             * position, an idle sampler is a plain pass-through). Player-
             * owned chains continue'd above (single staging writer: the
             * player's walk stages ITS chains, this one stages the rest).
             * rec IDLE: an un-owned chain records ITS OWN stream even while
             * another engine plays (multi-chain — the old !playing gate was
             * global); owned chains record via the CHAIN_REC_INPUT spans. */
            ChainExecCtx cxp = {
                .viz_bus            = audioBuffers,
                .pp_scratch         = &s_ls_send_pp,
                .lx_line            = s_lx_line,
                .pb_marker_id       = pb_here,
                .rec_mode           = CHAIN_REC_IDLE,
                .publish_tap_at_end = 1,
            };
            chain_execute_span(sp, c, 0, sp->num_inserts, &cxp,
                               sbR, sbG, sbB, nb_pixels, &ex);

            if (c == pb_chain && ex.pb_found)
            { pb_R = ex.pbR; pb_G = ex.pbG; pb_B = ex.pbB; pb_found = 1; }

            /* Head-panel engine tap A — published from the FIRST send (M1
             * approximation: the head panel shows one engine input line). */
            if (ex.ls_staged && c == first_send_chain)
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                    ex.lsR, ex.lsG, ex.lsB, nb_pixels);
        }

#ifndef VST_MODE
        /* Legacy standalone full preprocess — unconditional commit below. */
        if (pipeline_process_frame(src_R, src_G, src_B, &live_cfg, &preprocessed_temp) != 0) {
          log_error("THREAD", "Pipeline processing failed");
        }
#else
        (void) src_R; (void) src_G; (void) src_B;
        /* No "→ LUXSTRAL" module anywhere → the engine is UNFED (D1): white
         * tap. With sends, the chain loop above published it from the first
         * send; while a player runs, its walk owns the tap. */
        if (frame_plan.num_ls_sends == 0 && !player_running_now)
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                NULL, NULL, NULL, nb_pixels);
#endif

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

      /* ── Plan-driven commits (P4-M4) ──────────────────────────────────────
       * VST: the audio-thread MIXER owns the additive/stereo/strokeforge
       * sections of db->preprocessed_data in EVERY topology (0 sends →
       * silence, D1); polyphonic has its dedicated block below. Nothing to
       * commit from here. Standalone keeps the legacy whole-struct commit. */
#ifndef VST_MODE
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

      /* (cond_signal removed: nothing ever waits on db->cond — consumers poll
       * dataReady/atomics; the signal was a futex syscall per line under the
       * commit mutex.) */
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
      break;
    }
  }

  /* (No write_mutex safety-release needed anymore: the lock is only ever
   * held inside the one-shot publish block at line completion.) */

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

  /* (P4-M3) Phase 1 only — drain start/stop REC commands + cache the first
   * sampler chain's source frame for the player's darken-blend. The idle
   * capture and the zone-1 view are POSITIONAL now: the uniform loop below
   * records at each SAMPLER marker (CHAIN_REC_IDLE) and publishes the
   * selection tap; the global modulated build is gone. */
  if (spSmp)
  {
      const uint8_t *sbR, *sbG, *sbB;
      (void) synth_source_base(spSmp, smp_owner_chain,
                               db, nb_pixels, &sbR, &sbG, &sbB);
      lux_sampler_on_live_frame_assembled(sbR, sbG, sbB, (uint16_t)nb_pixels);
  }
#endif

  /* NO early-out on !internal_source_any_active() here. The loop below is
   * ALSO the silence sweep: when the last internal source deactivates
   * (module removed / media unloaded / source disabled), every chain takes
   * the cheap no-signal branch — staging goes inactive (the drone stops),
   * the taps whiten, the waterfalls scroll to blank. The old early-out
   * skipped all of that, freezing the removed source's last frame in the
   * engines and the visualizers. Idle cost: a few flag stores + memsets at
   * the service's 20 Hz idle rate. */

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

    /* LuxSynth/LuxWave OUT presence is rescanned by chain_publish_no_signal()
     * on the silent branch — only the LuxStral bank is needed here. */
    int ls_bank = -1;
    for (int i = 0; i < sp->num_inserts; i++)
      if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSTRAL)
      { ls_bank = sp->insert_state_idx[i]; break; }

    /* Per-chain playback: owned when ITS OWN engine is driving (or the
     * SCORE relay for a score chain) — mirror of udpThread's per-engine
     * gate; both chains may be player-owned simultaneously. */
    const int stream_player_owned =
        chain_hosts_driving_engine(sp)
        || (sp->has_score && score_playing);
    if (stream_player_owned)
    {
      /* P4-M2 — EVERY player-owned chain splits at its owning marker (mirror
       * of udpThread): FramePlayerThread walks the span BELOW it; the segment
       * ABOVE keeps running here — the chain source (IMAGE/VIDEO/CAMERA line)
       * feeds the upstream probes/OUTs and every SAMPLER marker records its
       * INPUT into its armed engine. */
      const uint8_t *pmR, *pmG, *pmB;
      const int pmSig = synth_source_base(sp, c, db,
                                          nb_pixels, &pmR, &pmG, &pmB);
      const int own_mk = chain_owning_marker_pos(
          sp, score_playing && sp->has_score, -1);
      if (pmSig >= 0 && own_mk >= 0)
      {
          /* Pre-marker span [0, marker] — mirror of udpThread's. */
          ChainExecCtx pcx = {
              .viz_bus         = audioBuffers,
              .pp_scratch      = &s_ls_send_pp_feeder,
              .lx_line         = s_lx_line_feeder,
              .pb_marker_id    = -1,
              .rec_mode        = CHAIN_REC_INPUT,
              .rec_skip_engine = -1,
          };
          ChainExecOut pex;
          chain_execute_span(sp, c, 0, own_mk + 1, &pcx,
                             pmR, pmG, pmB, nb_pixels, &pex);
          /* Above-marker LS OUT staged here → keep tap A live (mirror). */
          if (pex.ls_staged && c == first_send_chain)
              audio_image_buffers_publish_engine_input(
                  audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                  pex.lsR, pex.lsG, pex.lsB, nb_pixels);
      }
      else if (own_mk >= 0 && audioBuffers != NULL
               && sp->viz_tap_insert >= 0 && sp->viz_tap_insert <= own_mk)
          /* Sourceless owned chain: blank paper on the selection tap. */
          audio_image_buffers_clear_selection_tap(audioBuffers);
      continue;
    }

    const uint8_t *sbR, *sbG, *sbB;
    const int sig = synth_source_base(sp, c, db,
                                      nb_pixels, &sbR, &sbG, &sbB);
    const int no_sig = synth_chain_has_no_signal(sp, sig, score_playing);
    if (no_sig || sig <= 0)
    {
      /* sig==0 → the live path owns this chain's frames (device streaming
       * edge) — leave everything as-is; a true no-signal chain goes silent. */
      if (no_sig)
        chain_publish_no_signal(sp, c, audioBuffers, nb_pixels,
                                c == first_send_chain, c == pb_chain);
      continue;
    }
    if (c == pb_chain)
    { pb_base_R = sbR; pb_base_G = sbG; pb_base_B = sbB; }

    ChainExecOut ex;
    const int pb_here = (c == pb_chain) ? pb_marker : -1;
    /* (P4-M3) ONE path — full positional span [0, N], sampler chains
     * included (the feeder's dedicated owner-chain shortcut is gone; the
     * SAMPLER marker records at its position, stateful FX still tick once
     * per line since each chain runs exactly once). */
    ChainExecCtx cxp = {
        .viz_bus            = audioBuffers,
        .pp_scratch         = &s_ls_send_pp_feeder,
        .lx_line            = s_lx_line_feeder,
        .pb_marker_id       = pb_here,
        .rec_mode           = CHAIN_REC_IDLE,
        .publish_tap_at_end = 1,
    };
    chain_execute_span(sp, c, 0, sp->num_inserts, &cxp,
                       sbR, sbG, sbB, nb_pixels, &ex);

    if (c == pb_chain && ex.pb_found)
    { pb_R = ex.pbR; pb_G = ex.pbG; pb_B = ex.pbB; pb_found = 1; }

    /* Engine tap A — from the first send (mirror of udpThread). */
    if (ex.ls_staged && c == first_send_chain
        && !(sp->has_score && score_playing))
      audio_image_buffers_publish_engine_input(
          audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
          ex.lsR, ex.lsG, ex.lsB, nb_pixels);
  }

  /* ── Path B (LuxSynth + LuxWave) — fed at its OUT marker position ────────── */
  int pb_done = 0;
  if (pb_chain >= 0 && pb_found)
  {
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

  /* (cond_signal removed: no waiter on db->cond exists — see udpThread commit) */
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

      {
        /* ── PULL MIX — THE additive writer (P4-M4, D1) ───────────────────
         * Blend every active LuxStral send (intensity-weighted) into the
         * single engine feed. The mixer is db->preprocessed_data's SOLE
         * writer for the additive/stereo/strokeforge sections — always
         * (producers stage; udp/feeder commit Path-B products only). With
         * ZERO "→ LUXSTRAL" modules placed, mixed == 0 → TRUE silence is
         * committed: no module OUT = the engine is unfed, doctrine D1 (the
         * legacy no-send live tick is gone).
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
