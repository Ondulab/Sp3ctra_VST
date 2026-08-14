/*
 * image_chain.h
 *
 * Image insert chain — ordered execution of the Modulated-channel inserts
 * (LuxPitch, LuxMask) with a configurable order and per-insert visual taps.
 *
 * M1 core of the modular pipeline: the hardcoded Pitch ► Mask invocation
 * becomes a loop over an ordered descriptor (the slot list has since grown
 * FX blocks — Reverb/Echo/EQ — without touching the executor call sites).
 *
 * Threading model:
 *   - image_chain_run() executes on the producer threads only.
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

/* Insert identifiers.
 * IDs 0..1 (LUXPITCH/LUXMASK) double as AudioImageBuffers tap indices
 * (AUDIO_IMAGE_NUM_INSERT_TAPS == 2). VIDEOSCROLL (2) is a PASS-THROUGH PROBE
 * with NO tap slot: it captures into its own VideoScrollState ring and forwards
 * the frame unchanged. It is ONLY consumed by image_chain_run(), never by the
 * tap path (the contextual selection tap). */
#define IMAGE_CHAIN_INSERT_LUXPITCH    0
#define IMAGE_CHAIN_INSERT_LUXMASK     1
#define IMAGE_CHAIN_INSERT_VIDEOSCROLL 2
#define IMAGE_CHAIN_INSERT_SAMPLER     3   /* position marker only — pass-through in
                                            * image_chain_run; lets the executor know
                                            * where the sampler sits so VideoScroll
                                            * probes capture pre/post-sampler correctly */
#define IMAGE_CHAIN_INSERT_LUXREVERB   4   /* FX — visual reverberation (per-chain pool) */
#define IMAGE_CHAIN_INSERT_LUXECHO     5   /* FX — echo, regenerated repeats (per-chain pool) */
#define IMAGE_CHAIN_INSERT_SCORE       6   /* position marker only — pass-through in
                                            * image_chain_run; locates the SCORE module so
                                            * the player thread can apply the inserts BELOW
                                            * it to the score playback frames */
#define IMAGE_CHAIN_INSERT_LUXEQ       7   /* FX — graphic EQ on the pixel/frequency axis
                                            * (per-chain pool) */
/* OUT send markers (synth-split M3) — pass-through in image_chain_run; they
 * locate each "→ ENGINE" module so the chain executor can tap the stream AT
 * the send's position. insert_state_idx = the send's conditioning-bank slot
 * (luxstral_out[]/luxsynth_out[]/luxwave_out[]). */
#define IMAGE_CHAIN_INSERT_OUT_LUXSTRAL 8
#define IMAGE_CHAIN_INSERT_OUT_LUXSYNTH 9
#define IMAGE_CHAIN_INSERT_OUT_LUXWAVE  10
#define IMAGE_CHAIN_INSERT_LUXHARMO    11  /* FX — SCALE musical quantizer on the
                                            * pixel/frequency axis (per-chain pool) */
#define IMAGE_CHAIN_INSERT_OUT_LUXGRAIN 12 /* OUT send marker — "→ LUXGRAIN"
                                            * granular engine (luxgrain_out[]) */
#define IMAGE_CHAIN_INSERT_MIDITAP     13  /* PASS-THROUGH PROBE with NO tap slot,
                                            * like VIDEOSCROLL: extracts MIDI notes
                                            * from the line and forwards the image
                                            * unchanged (see midi_tap.h). Consumed
                                            * ONLY by image_chain_run(). */
#define IMAGE_CHAIN_INSERT_LUXCENTRO   14  /* FX — CENTROID: floor écrêtage + mass →
                                            * barycentre line simplifier (per-chain
                                            * pool) */
#define IMAGE_CHAIN_INSERT_LUXDRIVE    15  /* FX — LEVELS: floor écrêtage + gain +
                                            * tanh saturation (per-chain pool) */
#define IMAGE_CHAIN_INSERT_LUXDCBLOCK  16  /* FX — DC BLOCK: per-line mean removal
                                            * (per-chain pool) */
#define IMAGE_CHAIN_NUM_INSERTS        17

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
