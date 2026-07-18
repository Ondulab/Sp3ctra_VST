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
#include "lux_reverb.h"
#include "lux_echo.h"
#include "lux_eq.h"
#include "lux_harmo.h"
#include "video_scroll.h"

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
            case IMAGE_CHAIN_INSERT_LUXREVERB:
                lux_reverb_process_frame((LuxReverbState *)insert_states[i],
                                         cr, cg, cb, pixel_count,
                                         luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXECHO:
                lux_echo_process_frame((LuxEchoState *)insert_states[i],
                                       cr, cg, cb, pixel_count,
                                       luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXEQ:
                lux_eq_process_frame((LuxEqState *)insert_states[i],
                                     cr, cg, cb, pixel_count,
                                     luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXHARMO:
                lux_harmo_process_frame((LuxHarmoState *)insert_states[i],
                                        cr, cg, cb, pixel_count,
                                        luxstral_num_octaves, &nr, &ng, &nb);
                break;
            default:
                break;   /* unknown insert → pass-through */
        }
        cr = nr; cg = ng; cb = nb;
    }

    *out_r = cr; *out_g = cg; *out_b = cb;
}
