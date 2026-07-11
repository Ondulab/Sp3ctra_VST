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
 * @brief Called by udpThread() after a complete CIS line has been assembled
 *        from all UDP fragments (pre-sequencer raw data).
 *
 * Implemented in LuxSampler.cpp — forwards to the active LuxSampler
 * instance for recording if a slot is in RECORDING state.
 *
 * Thread: UDP receiver thread (Non-RT). Allocation is allowed here.
 *
 * @param R           Red channel buffer (nb_pixels bytes valid)
 * @param G           Green channel buffer
 * @param B           Blue channel buffer
 * @param pixel_count Number of valid pixels (1728 @200DPI or 3456 @400DPI)
 * @param line_id     Original UDP line identifier (for debug/sync)
 */
void lux_sampler_on_frame_assembled(const uint8_t* R,
                                       const uint8_t* G,
                                       const uint8_t* B,
                                       uint16_t       pixel_count,
                                       uint32_t       line_id);

/**
 * @brief Phase 1 — live frame assembled (before LuxPitch/LuxMask).
 *
 * Image chain: Live ► LuxPitch ► LuxMask ► LuxSampler.
 * Called by udpThread() right after a scanline has been reassembled but
 * BEFORE LuxPitch and LuxMask run.  Drains pending start/stop record
 * commands and caches the live frame for FramePlayerThread's darken-blend.
 * Does NOT capture a frame into the recording slot — that happens later in
 * lux_sampler_on_modulated_frame_ready() so the recorded content includes
 * Pitch + Mask processing.
 */
void lux_sampler_on_live_frame_assembled(const uint8_t* R,
                                          const uint8_t* G,
                                          const uint8_t* B,
                                          uint16_t       pixel_count);

/**
 * @brief Phase 2 — modulated frame ready (after LuxPitch + LuxMask).
 *
 * Called by udpThread()/the feeder AFTER the mod-bus OWNER CHAIN's
 * pre-marker processors have produced the post-mask frame.
 * Responsibilities (OWNER ENGINE only — per-chain feed, 2026-07-11):
 *   • Mirror the post-mask frame into the sampler snapshot (so the
 *     Modulated channel stays alive in idle / REC / STEP_LIVE).
 *   • Write the post-mask frame into the OWNER engine's active recording
 *     slot, so recorded samples include LuxPitch + LuxMask processing.
 * `owner_engine` = the engine slot of the owner chain's SAMPLER marker.
 */
void lux_sampler_on_modulated_frame_ready(int            owner_engine,
                                          const uint8_t* R,
                                          const uint8_t* G,
                                          const uint8_t* B,
                                          uint16_t       pixel_count,
                                          uint32_t       line_id);

/**
 * @brief Per-chain sampler capture — record ONE chain's stream into ITS
 *        engine's armed recording slot (idle only; during playback the
 *        resampling path lux_samplers_record_modulated() owns recording).
 *
 * Called by the chain executor (udpThread / feeder, Non-RT) at a SAMPLER
 * position marker executed positionally — the stream at that position is
 * the chain's OWN flux (its source + its upstream processors), never the
 * shared modulated bus.
 */
void lux_sampler_record_chain_frame(int engine_slot,
                                    const uint8_t* R,
                                    const uint8_t* G,
                                    const uint8_t* B,
                                    uint16_t       pixel_count);

/**
 * @brief Resampling capture — record the FINAL modulated channel into every
 *        sampler engine that has an armed recording slot.
 *
 * Called by udpThread() on the PLAYING path (a slot is playing into the
 * modulated channel) so a downstream sampler records the combination
 * (e.g. sampler B records sampler A's playback). Unlike
 * lux_sampler_on_modulated_frame_ready(), it performs NO sampler-snapshot
 * mirror — the engine that is playing owns the snapshot.
 *
 * Thread: UDP receiver thread (Non-RT).
 */
void lux_samplers_record_modulated(const uint8_t* R,
                                   const uint8_t* G,
                                   const uint8_t* B,
                                   uint16_t       pixel_count,
                                   uint32_t       line_id);

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
 * @brief Returns non-zero if any LuxSampler slot is currently RECORDING.
 *
 * Used by udpThread() to allow preprocessed_data writes for Source=Sampler
 * during recording.  The sampler snapshot is updated by onFrameAssembled()
 * so the pipeline can read from it even while recording.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if recording active, 0 otherwise
 */
int lux_sampler_is_recording(void);

/**
 * @brief Returns non-zero if the LuxSampler passthrough flag is active.
 *
 * Passthrough is enabled during STEP_LIVE sequencer steps and after rtStop().
 * When active, the live CIS stream should flow through all synthesis paths
 * (including Source=S) because no sampler slot is providing content.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if passthrough enabled, 0 otherwise
 */
int lux_sampler_is_passthrough(void);

/**
 * @brief Returns non-zero ONLY when the sequencer is running and the current
 *        step is STEP_LIVE.
 *
 * Unlike lux_sampler_is_passthrough() (which is also true during normal
 * idle/stop), this flag is set exclusively by triggerStep(STEP_LIVE) and
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

/**
 * @brief Returns non-zero while the SCORE player is actively playing back
 *        (any engine's dedicated scoreSlot is in playback).
 *
 * Used by udpThread() / the feeder tick to leave a score-fed chain's input
 * alone while FramePlayerThread owns it, and to commit silence to a score-fed
 * chain only when the score is idle.
 *
 * Thread: UDP receiver thread / feeder (Non-RT). Atomic read only.
 *
 * @return 1 if score playback active, 0 otherwise
 */
int lux_sampler_is_score_playing(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VST_MODE */
#endif /* LUX_SAMPLER_HOOKS_H */
