/*
 * video_scroll.c — SPSC ring + per-instance pool for the VideoScroll probe.
 * RT-safety: pure C, allocation-free, lock-free. See video_scroll.h.
 *
 * Author: zhonx
 */
#include "video_scroll.h"
#include <string.h>

VideoScrollState g_video_scroll_proc;
static VideoScrollState s_video_scroll_extra[CHAIN_MAX_CHAINS - 1];

VideoScrollState *video_scroll_instance(int idx)
{
    if (idx <= 0) return &g_video_scroll_proc;
    if (idx >= CHAIN_MAX_CHAINS) idx = CHAIN_MAX_CHAINS - 1;
    return &s_video_scroll_extra[idx - 1];
}

void video_scroll_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        video_scroll_init(video_scroll_instance(i));
}

void video_scroll_init(VideoScrollState *state)
{
    if (!state) return;
    memset(state->slots, 0, sizeof(state->slots));   /* ~6.3 MB, INIT ONLY (never RT) */
    atomic_init(&state->write_index, 0u);
    atomic_init(&state->generation,  0u);
    atomic_init(&state->paused,      0);
}

/* ── Producer (synthesis thread) ───────────────────────────────────────────── */
void video_scroll_capture_line(VideoScrollState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int pixel_count)
{
    uint32_t w, slot;
    int px;

    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;
    if (atomic_load_explicit(&state->paused, memory_order_relaxed))
        return;   /* paused → freeze; don't consume ring bandwidth */

    px = pixel_count;
    if (px > VIDEO_SCROLL_MAX_PIXELS) px = VIDEO_SCROLL_MAX_PIXELS;

    w    = atomic_load_explicit(&state->write_index, memory_order_relaxed);
    slot = w & VIDEO_SCROLL_RING_MASK;

    memcpy(state->slots[slot].r, in_r, (size_t)px);
    memcpy(state->slots[slot].g, in_g, (size_t)px);
    memcpy(state->slots[slot].b, in_b, (size_t)px);
    state->slots[slot].pixel_count = px;

    /* Release: slot contents visible before the incremented write_index. */
    atomic_store_explicit(&state->write_index, w + 1u, memory_order_release);
}

/* ── Consumer (UI/timer thread) ────────────────────────────────────────────── */
uint32_t video_scroll_ring_writepos(const VideoScrollState *state)
{
    if (!state) return 0u;
    return atomic_load_explicit(&state->write_index, memory_order_acquire);
}

uint32_t video_scroll_ring_available(const VideoScrollState *state,
                                     uint32_t last_read, int *out_pixel_count)
{
    uint32_t w, avail;
    if (out_pixel_count) *out_pixel_count = 0;
    if (!state) return 0u;

    w = atomic_load_explicit(&state->write_index, memory_order_acquire);
    avail = w - last_read;                    /* wrap-safe unsigned subtraction */
    if (avail > VIDEO_SCROLL_RING_SLOTS)
        avail = VIDEO_SCROLL_RING_SLOTS;      /* overrun clamp: newest window only */

    if (out_pixel_count && w != last_read)
        *out_pixel_count = state->slots[(w - 1u) & VIDEO_SCROLL_RING_MASK].pixel_count;
    return avail;
}

static int video_scroll_idx_live(uint32_t w, uint32_t idx)
{
    uint32_t age;
    if ((int32_t)(idx - w) >= 0) return 0;    /* not yet produced */
    age = w - idx;
    if (age > VIDEO_SCROLL_RING_SLOTS) return 0;  /* already overwritten */
    return 1;
}

int video_scroll_ring_get(const VideoScrollState *state, uint32_t idx,
                          uint8_t *out_r, uint8_t *out_g, uint8_t *out_b,
                          int *out_px)
{
    uint32_t w;
    const VideoScrollSlot *s;
    int px;

    if (out_px) *out_px = 0;
    if (!state || !out_r || !out_g || !out_b) return 0;

    w = atomic_load_explicit(&state->write_index, memory_order_acquire);
    if (!video_scroll_idx_live(w, idx)) return 0;

    s  = &state->slots[idx & VIDEO_SCROLL_RING_MASK];
    px = s->pixel_count;
    if (px < 0) px = 0;
    if (px > VIDEO_SCROLL_MAX_PIXELS) px = VIDEO_SCROLL_MAX_PIXELS;

    memcpy(out_r, s->r, (size_t)px);
    memcpy(out_g, s->g, (size_t)px);
    memcpy(out_b, s->b, (size_t)px);

    /* Re-validate after the copy: reject a tear if the producer lapped mid-copy.
     * On tear out_* are INDETERMINATE and must be discarded by the caller. */
    if (!video_scroll_idx_live(video_scroll_ring_writepos(state), idx))
        return 0;

    if (out_px) *out_px = px;
    return 1;
}

uint32_t video_scroll_generation(const VideoScrollState *state)
{
    if (!state) return 0u;
    return atomic_load_explicit(&state->generation, memory_order_acquire);
}
