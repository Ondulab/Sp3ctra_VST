/*
 * chain_plan.h
 *
 * M6 Phase 2 — RT-safe per-synth chain descriptor.
 *
 * The processor derives a ChainPlan from the editable ChainModel (message
 * thread) and publishes it via a lock-free double buffer. The synthesis thread
 * reads a consistent snapshot once per frame and processes each synth engine's
 * input through ONLY the modules on that synth's chain, in that chain's order.
 *
 * Most synth engines are singletons → at most one chain per synth (enforced in
 * the model). LuxStral is the exception since P3-M1: N sends across chains are
 * described by ls_send[] and pull-mixed by the audio thread (the synth[] slot
 * stays filled for visualizer compatibility). Pitch/Mask are per-instance:
 * each chain uses its own pool slot (insert_state_idx) so chains never share
 * state.
 *
 * Author: zhonx
 */
#ifndef CHAIN_PLAN_H
#define CHAIN_PLAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max ordered inserts in ONE chain recipe. Worst case allowed by the model:
 * Pitch + Mask + Reverb + Echo + EQ + up to CHAIN_MAX_CHAINS (8) VideoScroll
 * probes (the per-chain duplicate rule is relaxed for VideoScroll only) + up
 * to 2 Sampler position markers + 1 Score position marker + 3 OUT send
 * markers (M3: 1 per type per chain) = 19. The `num_inserts <
 * CHAIN_PLAN_MAX_INSERTS` gate in deriveAndPublishChainPlan is a defensive
 * cap; at 20 it is unreachable for any legal model (an overflow would
 * silently drop entries — a dropped marker misroutes every probe/FX placed
 * after it). states[] locals in multithreading.c grow to 20*sizeof(void*) =
 * 160 B — negligible stack. */
#define CHAIN_PLAN_MAX_INSERTS 20   /* max ordered processors/probes/markers per chain */
#define CHAIN_MAX_CHAINS       8    /* per-instance state pool size (Pitch/Mask/VideoScroll) */

/* Where a chain's input frame comes from. */
typedef enum {
    CHAIN_SRC_LIVE = 0,   /* SP3CTRA raw UDP feed */
    CHAIN_SRC_SAMPLER,    /* sampler playback frames */
    CHAIN_SRC_SCORE,      /* score playback frames */
    CHAIN_SRC_IMAGE,      /* loaded still image (internal source, M9) */
    CHAIN_SRC_VIDEO,      /* video file frames (internal source, M9) */
    CHAIN_SRC_NONE,       /* no source placed (treated as live fallback) */
    CHAIN_SRC_CAMERA      /* camera device frames (internal source, M9).
                           * Appended AFTER NONE to keep persisted ordinals
                           * of older sessions valid. */
} ChainSourceKind;

/* Synth slot indices in ChainPlan.synth[].
 * Slot 3 was the removed LuxStral engine B — kept as a hole until the M3
 * uniform-recipe rewrite so scratch-index maths (CHAIN_SYNTH_COUNT offsets)
 * stay untouched. Never filled: present == 0. */
#define CHAIN_SYNTH_LUXSTRAL   0
#define CHAIN_SYNTH_LUXSYNTH   1
#define CHAIN_SYNTH_LUXWAVE    2
#define CHAIN_SYNTH_COUNT      4

/* Recipe to build one synth engine's input from its chain. */
typedef struct {
    int present;        /* 1 if this synth is placed in a chain */
    int source_kind;    /* ChainSourceKind */
    int has_sampler;    /* a Sampler module sits upstream in the chain */
    int has_score;      /* a Score module sits upstream in the chain */
    int num_inserts;    /* ordered Pitch/Mask inserts before the synth */
    int insert_id[CHAIN_PLAN_MAX_INSERTS];        /* IMAGE_CHAIN_INSERT_* in order */
    int insert_state_idx[CHAIN_PLAN_MAX_INSERTS]; /* pool slot per insert:
                                                   *  LUXPITCH/LUXMASK → chain-derived
                                                   *  state idx; VIDEOSCROLL → per-
                                                   *  instance slot (ModuleInstance.slot,
                                                   *  0..7). */

    /* Contextual visualizer tap: -1 = none; k in [0..num_inserts] = publish
     * the stream frame AFTER the first k inserts of THIS chain into the
     * selection-tap bus (audio_image_buffers_publish_selection_tap). k = the
     * SELECTED module's output position; 0 = the chain's base/source frame.
     * Set on at most ONE plan entry (the chain hosting the selection). */
    int viz_tap_insert;
} SynthChainPlan;

/* Synth-split P3 — one LuxStral SEND: a chain feeding the (single) LuxStral
 * engine through its → LUXSTRAL OUT module. `recipe` is the chain compiled up
 * to the OUT's position (same shape as a synth chain); `bank_slot` picks the
 * send's conditioning bank (g_sp3ctra_config.luxstral_out[bank_slot]:
 * negative/DC/gamma/contrastMin/rangeDb/intensity/enabled) and its envelope
 * state. Every send is staged by its producer thread (synth_staging.h) and
 * the engine feed is the intensity-weighted MIX of all active sends, pulled
 * by the audio thread. */
typedef struct {
    int chain_idx;              /* model chain hosting this send (0..7) */
    int bank_slot;              /* luxstral_out[] / luxstralOut{N}_* bank (0..7) */
    SynthChainPlan recipe;      /* chain compiled up to the OUT position */
} LsSendPlan;

typedef struct {
    SynthChainPlan synth[CHAIN_SYNTH_COUNT];

    /* M3 — uniform per-chain recipes: chain[i] mirrors MODEL chain i (index-
     * stable across republishes while the model topology holds). A chain is
     * executed once per frame by its producer thread (udpThread / feeder):
     * source → ordered inserts, with probes capturing and OUT markers tapping
     * the stream AT THEIR POSITION. present=1 only when the chain needs that
     * execution (it carries an OUT, a probe, or the zone-1 selection);
     * `source_kind`/`has_sampler`/`has_score` follow the same rules as synth
     * chains, so a source-less monitor chain stays static (no live leak). */
    int num_chains;
    SynthChainPlan chain[CHAIN_MAX_CHAINS];

    /* Synth-split P3 — LuxStral sends (N-chain mix). When num_ls_sends > 0
     * the audio thread's mixer owns db->preprocessed_data (additive/stereo/
     * strokeforge sections) and synth[CHAIN_SYNTH_LUXSTRAL] is IGNORED by
     * the audio path (kept filled for visualizer compatibility). The mixer
     * reads the send list (chain_idx + bank weights) from here; the recipes
     * are executed via chain[] above. */
    int num_ls_sends;
    LsSendPlan ls_send[CHAIN_MAX_CHAINS];
} ChainPlan;

/* Message thread: publish a new plan (lock-free double buffer + atomic flip). */
void chain_plan_publish(const ChainPlan* plan);

/* Synth thread: copy the current published plan into `out` (consistent snapshot,
 * zero-filled until the first publish). */
void chain_plan_get(ChainPlan* out);

#ifdef __cplusplus
}
#endif

#endif /* CHAIN_PLAN_H */
