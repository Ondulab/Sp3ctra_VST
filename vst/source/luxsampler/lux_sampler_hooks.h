#ifndef LUX_SAMPLER_HOOKS_H
#define LUX_SAMPLER_HOOKS_H

/*
 * lux_sampler_hooks.h
 *
 * C/C++ compatible declarations for the LuxSampler interception hooks.
 * These functions are implemented in LuxSampler.cpp (C++ side) and called
 * from multithreading.c (C side) by whichever thread drives the per-synth
 * processing: udpThread() while the SP3CTRA device streams, the internal
 * source feeder tick (internal_sources_process_tick, MediaSourceService
 * thread) otherwise — the 250 ms hand-over hysteresis keeps the two
 * exclusive. Where the docs below say "udpThread()", read "the current
 * per-line producer".
 *
 * Only active in VST_MODE (standalone build has no LuxSampler).
 */

#ifdef VST_MODE

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Phase 1 — live frame assembled (before LuxPitch/LuxMask).
 *
 * Image chain: Live ► LuxPitch ► LuxMask ► LuxSampler.
 * Called by udpThread() right after a scanline has been reassembled but
 * BEFORE LuxPitch and LuxMask run.  Drains pending start/stop record
 * commands and caches the live frame for FramePlayerThread's darken-blend.
 * Does NOT capture a frame into the recording slot — capture is positional
 * (the chain executor records at each SAMPLER marker, P4-M3).
 */
void lux_sampler_on_live_frame_assembled(const uint8_t* R,
                                          const uint8_t* G,
                                          const uint8_t* B,
                                          uint16_t       pixel_count);

/**
 * @brief Cache ONE engine's MIX/darken-blend reference: the chain stream
 *        arriving at ITS SAMPLER marker. Called by the chain executor at that
 *        exact position (P4 — the blend uses the chain's own flux, never the
 *        device line). Non-RT producer threads + the player's downstream walk.
 */
void lux_sampler_cache_input_frame(int engine_slot,
                                   const uint8_t* R,
                                   const uint8_t* G,
                                   const uint8_t* B,
                                   uint16_t       pixel_count);

/**
 * @brief Per-chain sampler capture — record ONE chain's stream into ITS
 *        engine's armed recording slot. A DRIVING engine (its own playback
 *        owns the channel) is skipped: it self-records inside its
 *        FramePlayerThread (multi-chain split, 2026-07-13).
 *
 * Called by the chain executor (udpThread / feeder, Non-RT) at a SAMPLER
 * position marker executed positionally — the stream at that position is
 * the chain's OWN flux (its source + its upstream processors), never the
 * shared modulated bus. Also used by the playback capture for SAME-chain
 * engines (their chain's output stream IS the playback frame).
 */
void lux_sampler_record_chain_frame(int engine_slot,
                                    const uint8_t* R,
                                    const uint8_t* G,
                                    const uint8_t* B,
                                    uint16_t       pixel_count);

/**
 * @brief Per-engine driving query — is THIS engine's playback
 *        currently driving its stream?
 *
 * Multi-chain split (2026-07-13): the chain executors gate player-ownership
 * PER CHAIN by matching the chain's own SAMPLER marker against this — two
 * engines on two chains may both be driving simultaneously.
 *
 * Thread: any Non-RT producer. Atomic reads only.
 */
int lux_sampler_engine_is_driving(int engine);

/**
 * @brief Returns non-zero if any LuxSampler slot is currently in PLAYING state.
 *
 * Used by udpThread() to decide whether to bypass live UDP data to the
 * synthesis engine (AudioImageBuffers). When playing, FramePlayerThread feeds
 * recorded frames to the synthesis engine instead.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if playback active (bypass live feed), 0 otherwise (passthrough)
 */
int lux_sampler_is_playing(void);

/**
 * @brief Which sampler ENGINE's playback drives the modulated channel.
 *
 * Per-chain playback (2026-07-12): the chain executors gate a chain's
 * player-ownership on ITS OWN engine (the SAMPLER marker's slot) matching
 * this value — a chain hosting the idle engine keeps running positionally
 * on its own stream while the other engine plays.
 *
 * Thread: any Non-RT producer. Atomic reads only.
 *
 * @return engine slot (0=A, 1=B) whose SAMPLER playback owns the channel;
 *         -1 when idle.
 */
int lux_sampler_playing_engine(void);

/**
 * @brief Returns non-zero if any LuxSampler slot is currently RECORDING.
 *
 * Used by udpThread() to allow preprocessed_data writes for Source=Sampler
 * during recording.  The sampler snapshot is updated by the display owner
 * so the pipeline can read from it even while recording.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if recording active, 0 otherwise
 */
int lux_sampler_is_recording(void);

/**
 * @brief Capture one INPUT frame into an engine's armed recording slot.
 *
 * REC records the module INPUT (2026-07-13): the chain stream arriving AT the
 * sampler's marker (chain source → pre-marker processors) — never the
 * engine's own playback mix. Called by the chain executor's CHAIN_REC_INPUT
 * spans (udpThread/feeder pre-marker + FramePlayerThread post-marker walks);
 * the idle capture path goes
 * through the positional chain capture. Unlike
 * lux_sampler_record_chain_frame this does NOT skip a driving engine.
 * No-op unless a slot is armed.
 *
 * Thread: udpThread / MediaSourceService (Non-RT).
 */
void lux_sampler_record_input_frame(int engine,
                                    const uint8_t *R,
                                    const uint8_t *G,
                                    const uint8_t *B,
                                    uint16_t pixel_count);


/**
 * @brief Returns non-zero ONLY when the sequencer is running and the current
 *        step is STEP_LIVE.
 *
 * Unlike a plain idle test, this flag is set exclusively by triggerStep(STEP_LIVE) and
 * cleared by triggerStep(anything_else) and rtStop().
 *
 * Used by udpThread() to route live CIS data through the Source=S path
 * ONLY when the sequencer explicitly requests it.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if sequencer STEP_LIVE is active, 0 otherwise
 */
int lux_sampler_is_seq_live_step(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VST_MODE */
#endif /* LUX_SAMPLER_HOOKS_H */
