#ifndef FRAME_SAMPLER_HOOKS_H
#define FRAME_SAMPLER_HOOKS_H

/*
 * frame_sampler_hooks.h
 *
 * C/C++ compatible declarations for the FrameSampler interception hooks.
 * These functions are implemented in FrameSampler.cpp (C++ side) and called
 * from udpThread() in multithreading.c (C side).
 *
 * Only active in VST_MODE (standalone build has no FrameSampler).
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
 * Implemented in FrameSampler.cpp — forwards to the active FrameSampler
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
void frame_sampler_on_frame_assembled(const uint8_t* R,
                                       const uint8_t* G,
                                       const uint8_t* B,
                                       uint16_t       pixel_count,
                                       uint32_t       line_id);

/**
 * @brief Returns non-zero if any FrameSampler slot is currently in PLAYING state.
 *
 * Used by udpThread() to decide whether to bypass live UDP data to the
 * synthesis engine (AudioImageBuffers). When playing, FramePlayerThread feeds
 * recorded frames to the synthesis engine instead.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if playback active (bypass live feed), 0 otherwise (passthrough)
 */
int frame_sampler_is_playing(void);

/**
 * @brief Returns non-zero if any FrameSampler slot is currently RECORDING.
 *
 * Used by udpThread() to allow preprocessed_data writes for Source=Sampler
 * during recording.  The sampler snapshot is updated by onFrameAssembled()
 * so the pipeline can read from it even while recording.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if recording active, 0 otherwise
 */
int frame_sampler_is_recording(void);

/**
 * @brief Returns non-zero if the FrameSampler passthrough flag is active.
 *
 * Passthrough is enabled during STEP_LIVE sequencer steps and after rtStop().
 * When active, the live CIS stream should flow through all synthesis paths
 * (including Source=S) because no sampler slot is providing content.
 *
 * Thread: UDP receiver thread (Non-RT). Must be fast (atomic read only).
 *
 * @return 1 if passthrough enabled, 0 otherwise
 */
int frame_sampler_is_passthrough(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VST_MODE */
#endif /* FRAME_SAMPLER_HOOKS_H */
