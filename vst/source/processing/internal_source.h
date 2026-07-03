/*
 * internal_source.h
 *
 * M9 — Internal SRC modules (IMAGE / VIDEO / CAMERA).
 *
 * Each internal source owns one "current line" buffer in the exact format the
 * chain expects from the SP3CTRA device: three planar uint8_t channels of
 * get_cis_pixels_nb() pixels (allocated at the 3456 fixed cap).
 *
 * Producers are the JUCE-side engines (ImageSourceEngine / VideoSourceEngine /
 * CameraSourceEngine): they publish a new line whenever their transport moves
 * (line position, video frame, camera frame). Consumers are the per-synth
 * routing blocks: udpThread substitutes the chain base frame when the device
 * streams, and internal_sources_process_tick() (SourceFeederThread) drives the
 * whole per-synth processing when the device is silent.
 *
 * Threading: every caller is Non-RT (message thread, engine service thread,
 * udpThread, feeder thread) → a plain mutex per source is fine. The audio RT
 * path never touches this module.
 *
 * Author: zhonx
 */
#ifndef INTERNAL_SOURCE_H
#define INTERNAL_SOURCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal source kinds — index into the source pool. */
#define INTERNAL_SRC_IMAGE  0
#define INTERNAL_SRC_VIDEO  1
#define INTERNAL_SRC_CAMERA 2
#define INTERNAL_SRC_COUNT  3

#define INTERNAL_SRC_MAX_PIXELS 3456   /* == FIXED_BUFFER_PIXELS */

/* Producer side (engine threads / message thread) ─────────────────────────── */

/* Mark the source as usable: a media/device is loaded AND a module of that
 * kind is placed in a chain. Inactive sources fall back to the live feed. */
void internal_source_set_active(int kind, int active);
int  internal_source_is_active(int kind);
int  internal_source_any_active(void);

/* Publish the current line (copied). n is clamped to INTERNAL_SRC_MAX_PIXELS. */
void internal_source_publish(int kind,
                             const uint8_t *r, const uint8_t *g, const uint8_t *b,
                             int n);

/* Consumer side (udpThread / feeder tick) ─────────────────────────────────── */

/* Copy the latest published line into caller buffers (sized >= max_pixels).
 * Returns the pixel count actually copied, or 0 when the source is inactive
 * or nothing has been published yet. When the published line is narrower or
 * wider than max_pixels it is nearest-resampled to max_pixels so consumers
 * always get a full chain-width line. */
int  internal_source_copy(int kind, uint8_t *r, uint8_t *g, uint8_t *b,
                          int max_pixels);

/* Live-feed activity: udpThread stamps every completed line; the feeder only
 * drives per-synth processing when the device has been silent for a while
 * (so both never process the same synth concurrently). */
void internal_source_note_live_line(void);
int  internal_source_live_streaming(void);   /* 1 = live line < 250 ms ago */

/* ChainSourceKind (chain_plan.h) → internal source kind, or -1. */
int  internal_source_kind_for_chain_src(int chain_src_kind);

#ifdef __cplusplus
}
#endif

#endif /* INTERNAL_SOURCE_H */
