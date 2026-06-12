#ifndef LUX_SAMPLER_HOOKS_H
#define LUX_SAMPLER_HOOKS_H

/*
 * lux_sampler_hooks.h
 *
 * C/C++ compatible declarations for the LuxSampler interception hooks.
 * These functions are implemented in LuxSampler.cpp (C++ side) and called
 * from udpThread() in multithreading.c (C side).
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
 * Called by udpThread() AFTER LuxPitch + LuxMask have produced the
 * post-mask frame.  Responsibilities:
 *   • Mirror the post-mask frame into the sampler snapshot (so the
 *     Modulated channel stays alive in idle / REC / STEP_LIVE).
 *   • Write the post-mask frame into the active recording slot, so
 *     recorded samples include LuxPitch + LuxMask processing.
 */
void lux_sampler_on_modulated_frame_ready(const uint8_t* R,
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VST_MODE */
#endif /* LUX_SAMPLER_HOOKS_H */
