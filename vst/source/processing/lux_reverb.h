/*
 * lux_reverb.h
 *
 * LuxReverb — visual reverberation on the image-line stream.
 *
 * The module keeps a per-pixel "tail" (float, energy space) that every incoming
 * line refreshes: material brighter than the tail re-excites it, then the tail
 * fades over `decay_s` (-60 dB, RT60-like) and spreads to neighbouring pixels
 * (`diffusion`), prolonging the visual matter exactly like a room prolongs
 * sound. The output blends the dry line with the decaying tail (`mix`).
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
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_REVERB_MAX_PIXELS 8192

/* Background mode — which pole is the "material" (mirrors LUX_MASK_BG_*).
 * AUTO resolves per frame from the line's mean level (material is sparse, so
 * the dominant pole IS the background) with hysteresis — the chain inserts see
 * the RAW image (the synth's Negative/inversion runs downstream), so a scanned
 * score is dark-on-white here even when the synth plays dark as loud. */
#define LUX_REVERB_BG_BLACK  0   /* bright material on black background */
#define LUX_REVERB_BG_WHITE  1   /* dark material on white background   */
#define LUX_REVERB_BG_AUTO   2   /* detect from the stream (default)    */

/* ============================================================================
 * LuxReverbConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   background_mode;   /* LUX_REVERB_BG_* */
    float decay_s;           /* tail fade time (-60 dB), seconds */
    float diffusion;         /* 0..1 — spatial spread rate of the tail */
    float mix;               /* 0..1 — wet (tail) level in the output */
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

    /* dt clock (same pattern as LuxMask envelopes). */
    uint64_t last_frame_ts_us;

    /* Line-geometry guard: a pixel-count change invalidates the tail. */
    int      last_pixel_count;

    /* Nonzero while the tail may hold energy — lazily cleared on disable. */
    int      tail_active;

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

/* ── Global instance + per-chain pool (mirrors LuxPitch/LuxMask) ───────────── */
extern LuxReverbState g_lux_reverb_proc;
LuxReverbState *lux_reverb_instance(int idx);  /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void            lux_reverb_init_all(void);     /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_REVERB_H */
