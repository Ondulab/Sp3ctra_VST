/*
 * image_chain.h
 *
 * Image insert chain — ordered execution of the Modulated-channel inserts
 * (LuxPitch, LuxMask) with a configurable order and per-insert visual taps.
 *
 * This is the M1 core of the modular pipeline (see docs/PLAN_DEVELOPPEMENT.md):
 * the hardcoded Pitch ► Mask invocation becomes a loop over an ordered
 * descriptor, so M6 can later extend the slot list with new effect blocks
 * (Tone, EQ, Mixer…) without touching the executor call sites.
 *
 * Threading model:
 *   - image_chain_process_inserts() runs on the synthesis thread only.
 *   - Order / tap demand are set from the UI or message thread through
 *     atomics (no locks, RT-safe).
 *   - Tap snapshots are published into AudioImageBuffers (single producer,
 *     multi reader) so visualizers draw EXACTLY what the engines consume —
 *     no UI-side re-simulation.
 *
 * Author: zhonx
 * Created: 2026-06-12
 */

#ifndef IMAGE_CHAIN_H
#define IMAGE_CHAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AudioImageBuffers;

/* Insert identifiers.
 * IDs 0..1 (LUXPITCH/LUXMASK) double as AudioImageBuffers tap indices
 * (AUDIO_IMAGE_NUM_INSERT_TAPS == 2). VIDEOSCROLL (2) is a PASS-THROUGH PROBE
 * with NO tap slot: it captures into its own VideoScrollState ring and forwards
 * the frame unchanged. It is ONLY consumed by image_chain_run(), never by the
 * tap path (image_chain_process_inserts). */
#define IMAGE_CHAIN_INSERT_LUXPITCH    0
#define IMAGE_CHAIN_INSERT_LUXMASK     1
#define IMAGE_CHAIN_INSERT_VIDEOSCROLL 2
#define IMAGE_CHAIN_INSERT_SAMPLER     3   /* position marker only — pass-through in
                                            * image_chain_run; lets the executor know
                                            * where the sampler sits so VideoScroll
                                            * probes capture pre/post-sampler correctly */
#define IMAGE_CHAIN_NUM_INSERTS        4

/* Chain order (APVTS param "chainInsertOrder"). */
#define IMAGE_CHAIN_ORDER_PITCH_MASK 0   /* default — historical order */
#define IMAGE_CHAIN_ORDER_MASK_PITCH 1

/* ── Configuration (UI / message thread → synthesis thread) ───────────────── */

void image_chain_set_order(int order);
int  image_chain_get_order(void);

/* Visual tap demand: a consumer (visualizer) declares interest in the output
 * of one insert.  When no demand is set the chain skips the tap snapshot
 * memcpy entirely (zero overhead).  Demands also force the modulated chain
 * to run even when no synth engine consumes it. */
void image_chain_set_tap_demand(int insert_id, int on);
int  image_chain_tap_demand(int insert_id);
int  image_chain_any_tap_demand(void);

/* ── Execution (synthesis thread) ─────────────────────────────────────────── */

/*
 * Run the insert chain (g_lux_pitch_proc / g_lux_mask_proc) on one RGB frame
 * in the configured order.  Auto-bypassing inserts return their input
 * pointers, so the idle cost stays O(1).
 *
 * If `taps` is non-NULL, the output of each demanded insert is snapshotted
 * into its tap slot (audio_image_buffers_snapshot_insert_tap).
 */
void image_chain_process_inserts(
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b,
    struct AudioImageBuffers *taps);

/*
 * M6 Phase 2 — generalised executor driven by an explicit ordered insert list
 * with per-stage state pointers (no globals, no fixed order). Used by the
 * per-chain synthesis loop: each chain runs its own Pitch/Mask instances in its
 * own order. Auto-bypassing inserts stay O(1). `insert_states[i]` is a
 * LuxPitchState* or LuxMaskState* matching `insert_ids[i]`.
 */
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
    const uint8_t **out_b);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_CHAIN_H */
