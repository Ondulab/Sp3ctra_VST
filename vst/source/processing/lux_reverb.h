/*
 * lux_reverb.h
 *
 * LuxReverb — visual reverberation on the image-line stream.
 *
 * Each pixel holds a decaying material envelope after subtracting the
 * per-colour background level. A soft excitation knee keeps low-level sensor
 * noise out of the tail.
 *
 * Decay law: the envelope fades LINEARLY in energy space — a full-scale
 * tail (255 LSB) reaches the pole in `decay_s`, every pixel losing the same
 * LSB per second. The synth's dB decode law (img_stage_apply_db_decode:
 * level_dB = -range · (1 - e/255)) turns that into a true exponential audio
 * decay of range_dB/decay_s dB per second, whatever the excitation level.
 * The former exponential fade in ENERGY space (-60 dB over decay_s) sat on a
 * plateau in AUDIO: every tail spent most of its life within 2 dB of the
 * -range_dB floor before being cut, and hundreds of such tails summed to a
 * broadband noise carpet.
 *
 * Damping: the fade rate rises toward the treble edge of the pixel axis
 * (`damping` = strength, `damp_type` = the law — see lux_reverb_damp_rate),
 * so the high end dies first exactly as in a room, instead of ringing as
 * long as the bass. Off at damping 0.
 *
 * `diffusion` smooths only the wet output over neighbouring pixels, with a
 * fixed width: it never spreads the stored tail into new pitches over time.
 * `mix` sets the tail level on the pixel's own pedestal, preserving the dry
 * line and avoiding accumulation of paper brightness in the envelope.
 *
 * Energy space: `background_mode` picks which pole carries the material
 * (BLACK = bright-on-black, WHITE = dark-on-white, mirroring LuxPitch/LuxMask).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *            No JUCE deps, no mutex, no logging.
 *            Runs on the synthesis thread only; config is copied wholesale from
 *            the message thread (same discipline as LuxMask).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#ifndef LUX_REVERB_H
#define LUX_REVERB_H

#include <stdint.h>
#include <math.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_REVERB_MAX_PIXELS 8192

/* Background mode — which pole is the "material" (mirrors LUX_MASK_BG_*).
 * AUTO learns the source pole briefly, then locks until reset. Inserts see
 * the RAW image (the synth's Negative/inversion runs downstream), so a scanned
 * score is dark-on-white here even when the synth plays dark as loud. */
#define LUX_REVERB_BG_BLACK  0   /* bright material on black background */
#define LUX_REVERB_BG_WHITE  1   /* dark material on white background   */
#define LUX_REVERB_BG_AUTO   2   /* detect from the stream (default)    */

/* Damping law — how the fade rate reads along the pixel/frequency axis
 * (u = pos/(px-1), 0 = bass edge, 1 = treble edge). Both laws meet at the
 * treble edge, where the tail fades 2^(LUX_REVERB_DAMP_OCTAVES · damping)
 * times faster than Decay (64× at full strength). */
#define LUX_REVERB_DAMP_LINEAR 0  /* straight line on the log-rate axis: each
                                   * step toward the treble fades the same
                                   * factor faster */
#define LUX_REVERB_DAMP_AIR    1  /* air absorption: the extra fade rate grows
                                   * with f², nil over the bass, kneeing up
                                   * over the top octaves (a hall) — default */
#define LUX_REVERB_DAMP_OCTAVES 6.0f   /* log2 of the treble-edge ratio at 100 % */

/* Fade-rate multiplier r >= 1 at axis position u for (law, strength): the
 * tail there fades r times faster than `decay_s` says. SINGLE SOURCE OF
 * TRUTH — the RT LUT builder and the DAMPING editor both sample this, so
 * the drawn curve IS the applied law. `span_oct` = the axis span in octaves
 * (only the AIR law reads it: its knee is a physical distance from the top). */
static inline float lux_reverb_damp_rate(int type, float amount, float u,
                                         float span_oct)
{
    if (!(amount > 0.0f)) return 1.0f;
    if (amount > 1.0f) amount = 1.0f;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    const float top_oct = LUX_REVERB_DAMP_OCTAVES * amount;   /* log2 r(1) */
    if (type == LUX_REVERB_DAMP_AIR)
    {
        /* (f / f_top)^2 = 4^((u-1)·span): the extra rate scaled so that
         * the treble edge lands on the same ratio as the LINEAR law. */
        if (span_oct < 1.0f) span_oct = 1.0f;
        return 1.0f + (exp2f(top_oct) - 1.0f)
                      * exp2f(2.0f * (u - 1.0f) * span_oct);
    }
    return exp2f(top_oct * u);
}

/* ============================================================================
 * LuxReverbConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   background_mode;   /* LUX_REVERB_BG_* */
    float decay_s;           /* seconds for a full-scale tail to reach the
                              * pole (linear in energy = range_dB/decay_s
                              * dB/s in audio) */
    float diffusion;         /* 0..1 — bounded spatial smoothing of wet output */
    float mix;               /* 0..1 — wet (tail) level in the output */
    float damping;           /* 0..1 — treble fade-rate strength (0 = flat) */
    int   damp_type;         /* LUX_REVERB_DAMP_* */
} LuxReverbConfig;

/* ============================================================================
 * LuxReverbState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxReverbConfig config;

    /* Decaying tail, float in energy space (0..255) for smooth exponential
     * fades well below 1 LSB of the uint8 line. */
    float    tail_r[LUX_REVERB_MAX_PIXELS];
    float    tail_g[LUX_REVERB_MAX_PIXELS];
    float    tail_b[LUX_REVERB_MAX_PIXELS];

    /* Monotonic microsecond clock; tail_active marks an initialized clock. */
    uint64_t last_frame_ts_us;

    /* Line-geometry guard: a pixel-count change invalidates the tail. */
    int      last_pixel_count;

    /* Nonzero while the tail may hold energy — lazily cleared on disable.
     * NOT an activity indicator: it latches on the first processed line and
     * only a reset clears it (see active_ticks for the rack LED). */
    int      tail_active;

    /* Rack-LED heartbeat: bumped once per line the module actually CHANGED
     * (output != input). A value that stopped moving between two UI refreshes
     * means "enabled but producing nothing" — either the chain feeds it blank
     * paper, or it is not being walked at all (unfed chain), which a latch
     * cannot express. Free-running: never reset, so a wrap is at worst one
     * stale LED refresh. */
    uint32_t active_ticks;

    /* Peak diffused wet MATERIAL energy AFTER the last processed line (energy
     * LSB above the tracked paper, pre-mix). Read by lux_reverb_tail_alive()
     * — the chain runout needs to know whether this module still has
     * something to print once its feed stops, and a latch (tail_active)
     * cannot express that. */
    float    tail_peak;

    /* AUTO background: the polarity is a property of the SOURCE, not of the
     * content — a dense fortissimo line (mostly ink) must never flip it and
     * wipe the tail. The verdict is learned over a short window after each
     * reset (extremes rule: whichever pole the min/max means lean toward),
     * then LOCKED until the next reset. last_bg_resolved tracks the polarity
     * the tail was built in (a flip invalidates it). */
    int      auto_bg_white;      /* current AUTO verdict (init: white/paper) */
    int      auto_locked;        /* 1 = verdict latched for this stream */
    int      auto_lock_countdown;/* learning-window frames remaining */
    int      auto_max_mean;      /* extremes observed during the window */
    int      auto_min_mean;
    int      last_bg_resolved;   /* -1 = none yet */

    /* Per-colour paper energy (exact 10th percentile), immediate upward
     * tracking and 16 ms downward smoothing. -1 = unseeded. */
    float    floor_ema[3];

    /* Per-pixel fade-rate multipliers (lux_reverb_damp_rate over the line),
     * a config-derived cache rebuilt only when its keys move — never on
     * reset. damp_key_span is also what the DAMPING editor reads to draw
     * the AIR knee where the RT puts it (0 until the first line). */
    float    damp_rate[LUX_REVERB_MAX_PIXELS];
    int      damp_key_type;
    float    damp_key_amount;
    int      damp_key_px;        /* -1 = never built */
    float    damp_key_span;

    /* Preallocated output buffers. */
    uint8_t  out_r[LUX_REVERB_MAX_PIXELS];
    uint8_t  out_g[LUX_REVERB_MAX_PIXELS];
    uint8_t  out_b[LUX_REVERB_MAX_PIXELS];
} LuxReverbState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void            lux_reverb_init(LuxReverbState *state);
void            lux_reverb_reset(LuxReverbState *state);   /* tail cleared, config untouched */
LuxReverbConfig lux_reverb_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or fully dry) the input pointers are returned as-is
 * (O(1) pass-through after the one-shot lazy tail clear).
 */
void lux_reverb_process_frame(
    LuxReverbState *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* Explicit monotonic microsecond timestamp for deterministic offline renders.
 * Same semantics as process_frame; all calls for an instance use one clock. */
void lux_reverb_process_frame_at(
    LuxReverbState *state,
    const uint8_t *in_r, const uint8_t *in_g, const uint8_t *in_b,
    int pixel_count, int luxstral_num_octaves, uint64_t now_us,
    const uint8_t **out_r, const uint8_t **out_g, const uint8_t **out_b);

/* ── Tail runout ───────────────────────────────────────────────────────────── */
/*
 * 1 while the tail still has something VISIBLE to print (peak * mix ≥ half an
 * LSB) on a blank-paper line. A reverb is defined by what it keeps printing
 * after its input goes silent, so the chain executor keeps walking a chain
 * that lost its feed until every tail-bearing insert answers 0 here — without
 * it, stopping the source truncates the decay on the spot. O(1).
 */
int lux_reverb_tail_alive(const LuxReverbState *state);

/* ── Global instance + per-chain pool (mirrors LuxPitch/LuxMask) ───────────── */
extern LuxReverbState g_lux_reverb_proc;
LuxReverbState *lux_reverb_instance(int idx);  /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void            lux_reverb_init_all(void);     /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_REVERB_H */
