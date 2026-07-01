/*
 * image_chain.c
 *
 * Image insert chain executor — see image_chain.h.
 *
 * Author: zhonx
 * Created: 2026-06-12
 */

#include "image_chain.h"
#include "lux_pitch.h"
#include "lux_mask.h"
#include "video_scroll.h"
#include "../audio/buffers/audio_image_buffers.h"

#include <stdatomic.h>

/* ── Atomic configuration ──────────────────────────────────────────────────── */

static _Atomic int s_chain_order = IMAGE_CHAIN_ORDER_PITCH_MASK;
static _Atomic int s_tap_demand[IMAGE_CHAIN_NUM_INSERTS] = {0};  /* VIDEOSCROLL never demands a tap */

void image_chain_set_order(int order)
{
    if (order != IMAGE_CHAIN_ORDER_PITCH_MASK &&
        order != IMAGE_CHAIN_ORDER_MASK_PITCH)
        order = IMAGE_CHAIN_ORDER_PITCH_MASK;
    atomic_store_explicit(&s_chain_order, order, memory_order_relaxed);
}

int image_chain_get_order(void)
{
    return atomic_load_explicit(&s_chain_order, memory_order_relaxed);
}

void image_chain_set_tap_demand(int insert_id, int on)
{
    if (insert_id < 0 || insert_id >= IMAGE_CHAIN_NUM_INSERTS) return;
    atomic_store_explicit(&s_tap_demand[insert_id], on ? 1 : 0,
                          memory_order_relaxed);
}

int image_chain_tap_demand(int insert_id)
{
    if (insert_id < 0 || insert_id >= IMAGE_CHAIN_NUM_INSERTS) return 0;
    return atomic_load_explicit(&s_tap_demand[insert_id], memory_order_relaxed);
}

int image_chain_any_tap_demand(void)
{
    int i;
    for (i = 0; i < IMAGE_CHAIN_NUM_INSERTS; i++)
        if (atomic_load_explicit(&s_tap_demand[i], memory_order_relaxed))
            return 1;
    return 0;
}

/* ── Executor ──────────────────────────────────────────────────────────────── */

/* Run one insert, then snapshot its output tap if a consumer asked for it. */
static void run_insert(
    int             insert_id,
    const uint8_t  *in_r, const uint8_t *in_g, const uint8_t *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r, const uint8_t **out_g, const uint8_t **out_b,
    struct AudioImageBuffers *taps)
{
    switch (insert_id)
    {
        case IMAGE_CHAIN_INSERT_LUXPITCH:
            lux_pitch_process_frame(&g_lux_pitch_proc,
                                    in_r, in_g, in_b,
                                    pixel_count, luxstral_num_octaves,
                                    out_r, out_g, out_b);
            break;
        case IMAGE_CHAIN_INSERT_LUXMASK:
            lux_mask_process_frame(&g_lux_mask_proc,
                                   in_r, in_g, in_b,
                                   pixel_count, luxstral_num_octaves,
                                   out_r, out_g, out_b);
            break;
        default:
            *out_r = in_r; *out_g = in_g; *out_b = in_b;
            return;
    }

    if (taps &&
        atomic_load_explicit(&s_tap_demand[insert_id], memory_order_relaxed))
    {
        audio_image_buffers_snapshot_insert_tap(taps, insert_id,
                                                *out_r, *out_g, *out_b,
                                                pixel_count);
    }
}

void image_chain_process_inserts(
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b,
    struct AudioImageBuffers *taps)
{
    const uint8_t *mid_r, *mid_g, *mid_b;
    const int order = atomic_load_explicit(&s_chain_order, memory_order_relaxed);
    const int first  = (order == IMAGE_CHAIN_ORDER_MASK_PITCH)
                     ? IMAGE_CHAIN_INSERT_LUXMASK  : IMAGE_CHAIN_INSERT_LUXPITCH;
    const int second = (order == IMAGE_CHAIN_ORDER_MASK_PITCH)
                     ? IMAGE_CHAIN_INSERT_LUXPITCH : IMAGE_CHAIN_INSERT_LUXMASK;

    run_insert(first,  in_r,  in_g,  in_b,
               pixel_count, luxstral_num_octaves,
               &mid_r, &mid_g, &mid_b, taps);
    run_insert(second, mid_r, mid_g, mid_b,
               pixel_count, luxstral_num_octaves,
               out_r, out_g, out_b, taps);
}

/* ── M6 Phase 2 — per-chain executor (explicit ordered state list) ──────────── */
void image_chain_run(
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const int      *insert_ids,
    void   * const *insert_states,
    int             num_inserts,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    const uint8_t *cr = in_r, *cg = in_g, *cb = in_b;

    for (int i = 0; i < num_inserts; i++)
    {
        const uint8_t *nr = cr, *ng = cg, *nb = cb;
        switch (insert_ids[i])
        {
            case IMAGE_CHAIN_INSERT_LUXPITCH:
                lux_pitch_process_frame((LuxPitchState *)insert_states[i],
                                        cr, cg, cb, pixel_count,
                                        luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXMASK:
                lux_mask_process_frame((LuxMaskState *)insert_states[i],
                                       cr, cg, cb, pixel_count,
                                       luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_VIDEOSCROLL:
                /* PASS-THROUGH PROBE: capture then forward unchanged. nr/ng/nb
                 * already equal cr/cg/cb. RT-safe SPSC push, no alloc/lock. */
                video_scroll_capture_line((VideoScrollState *)insert_states[i],
                                          cr, cg, cb, pixel_count);
                break;
            default:
                break;   /* unknown insert → pass-through */
        }
        cr = nr; cg = ng; cb = nb;
    }

    *out_r = cr; *out_g = cg; *out_b = cb;
}
