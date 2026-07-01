/*
 * video_scroll.h — pass-through RT image PROBE for the chain pipeline.
 *
 * A VideoScroll insert captures the RGB scanline AS IT FLOWS at its position in
 * a synth chain (IMAGE_CHAIN_INSERT_VIDEOSCROLL) into a per-instance preallocated
 * SPSC ring, then forwards the image downstream UNCHANGED. Display-only params
 * (speed/zoom/fade/invert/colour) live in APVTS and are applied UI-side.
 *
 * Threading:
 *   PRODUCER (single) : synthesis thread, video_scroll_capture_line().
 *                       RT-safe: bounded memcpy + one release store. No alloc/lock.
 *                       Owns write_index (release) AND generation (bumped on
 *                       discontinuity). On overrun it simply advances (newest wins).
 *   CONSUMER (single) : message/timer thread, video_scroll_ring_*().
 *                       Lock-free acquire loads. The drain cursor is owned by the
 *                       consumer component (not stored in this struct).
 *   CONTROL           : paused is written by the message thread (plain volatile
 *                       store) and read by the synth thread.
 *
 * Memory: per slot = 8192*3 + 4 ~= 24,580 B; per instance (256 slots) ~= 6.29 MB;
 *         pool (CHAIN_MAX_CHAINS=8) ~= 50.3 MB BSS. Tune VIDEO_SCROLL_RING_SLOTS.
 *
 * Author: zhonx
 */
#ifndef VIDEO_SCROLL_H
#define VIDEO_SCROLL_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-instance pool size */

/* Atomic discipline mirrors lux_pitch.h's LP_ATOMIC: on the C++ side fields are
 * plain `volatile T` (atomic for int/uint32 on all targets, written as plain
 * stores) and <atomic> is pulled in only for std::memory_order names; on the C
 * side they are real _Atomic T with <stdatomic.h>. This lets PluginProcessor.cpp
 * (C++) write `paused` with a simple volatile store while video_scroll.c (C11)
 * uses atomic_*_explicit. */
#ifdef __cplusplus
  #include <atomic>
  #define VS_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define VS_ATOMIC(T) _Atomic T
#endif

#define VIDEO_SCROLL_MAX_PIXELS  8192
#define VIDEO_SCROLL_RING_SLOTS  256                 /* power of two */
#define VIDEO_SCROLL_RING_MASK   (VIDEO_SCROLL_RING_SLOTS - 1)

/* One captured scanline (r/g/b only; UI derives luma for B&W). */
typedef struct {
    uint8_t r[VIDEO_SCROLL_MAX_PIXELS];
    uint8_t g[VIDEO_SCROLL_MAX_PIXELS];
    uint8_t b[VIDEO_SCROLL_MAX_PIXELS];
    int     pixel_count;
} VideoScrollSlot;

/* One capture instance (one pool slot). */
typedef struct VideoScrollState {
    VideoScrollSlot     slots[VIDEO_SCROLL_RING_SLOTS];
    VS_ATOMIC(uint32_t) write_index;   /* producer->consumer total pushes (release) */
    VS_ATOMIC(uint32_t) generation;    /* producer-only: bumped on discontinuity   */
    VS_ATOMIC(int)      paused;        /* message-thread write, synth-thread read   */
} VideoScrollState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void video_scroll_init(VideoScrollState *state);

/* ── Producer (synthesis thread — RT-safe) ─────────────────────────────────── */
/* Push one scanline (no-op when paused, NULL, or pixel_count<=0). */
void video_scroll_capture_line(VideoScrollState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int pixel_count);

/* ── Consumer (UI/timer thread — lock-free, single consumer) ───────────────── */
/* New lines available since last_read; clamped to RING_SLOTS (newest wins).
 * Writes the newest line's pixel count into *out_pixel_count (may be NULL). */
uint32_t video_scroll_ring_available(const VideoScrollState *state,
                                     uint32_t last_read, int *out_pixel_count);

/* Current total-pushes counter (acquire). Use as new read cursor after draining. */
uint32_t video_scroll_ring_writepos(const VideoScrollState *state);

/* Copy the slot at ABSOLUTE index `idx` (a total-pushes value) into caller
 * buffers (each >= VIDEO_SCROLL_MAX_PIXELS). Returns 1 on success, 0 if `idx`
 * is outside the live window. ON A 0 RETURN out_* ARE INDETERMINATE AND MUST NOT
 * BE RENDERED; *out_px is set to 0. Re-validates AFTER the copy to reject tears. */
int video_scroll_ring_get(const VideoScrollState *state, uint32_t idx,
                          uint8_t *out_r, uint8_t *out_g, uint8_t *out_b,
                          int *out_px);

/* ── Clear / generation ────────────────────────────────────────────────────── */
/* generation is PRODUCER-OWNED. The consumer reads it to blank history; it never
 * writes it (UI Stop wipes its own image by re-anchoring its cursor). */
uint32_t video_scroll_generation(const VideoScrollState *state);

/* ── Per-instance pool (mirrors lux_pitch) ─────────────────────────────────── */
extern VideoScrollState g_video_scroll_proc;
VideoScrollState *video_scroll_instance(int idx);  /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void              video_scroll_init_all(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_SCROLL_H */
