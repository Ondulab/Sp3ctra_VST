/*
 * lux_echo.h
 *
 * LuxEcho — echo / delay on the image-line stream.
 *
 * The module keeps a ring of past FEEDBACK lines: each incoming line is mixed
 * with the line played `delay_lines` frames earlier, producing a repeat of the
 * visual material further down the stream. `feedback` re-injects the delayed
 * line into the ring, turning the single repeat (delay) into a decaying train
 * of repeats (echo). `mix` sets the audible/visible level of the repeats.
 *
 *   out = in ⊕ mix      * delayed      (⊕ = saturating add, energy space)
 *   fb  = in ⊕ feedback * delayed  →  stored in the ring
 *
 * The delay is expressed in LINES (deterministic and visual — the line rate is
 * a device property, freqLps). Energy space: `background_mode` picks which
 * pole carries the material (mirrors LuxPitch/LuxMask/LuxReverb).
 *
 * Memory: per slot = 8192*3 + 4 ≈ 24,580 B; per instance (256 slots) ≈ 6.3 MB;
 *         pool (CHAIN_MAX_CHAINS=8) ≈ 50 MB BSS — same budget as VideoScroll.
 *         Untouched instances stay virtual (zero-fill pages).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N).
 *            No JUCE deps, no mutex, no logging.
 *            Ring produced AND consumed on the synthesis thread only (no
 *            atomics needed, unlike VideoScroll's cross-thread ring).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#ifndef LUX_ECHO_H
#define LUX_ECHO_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_ECHO_MAX_PIXELS  8192
#define LUX_ECHO_RING_SLOTS  256                 /* power of two */
#define LUX_ECHO_RING_MASK   (LUX_ECHO_RING_SLOTS - 1)
#define LUX_ECHO_MAX_DELAY   (LUX_ECHO_RING_SLOTS - 1)

/* Background mode — which pole is the "material" (mirrors LUX_MASK_BG_*).
 * AUTO resolves per frame from the line's mean level (see lux_reverb.h — the
 * chain inserts see the RAW image, upstream of the synth's Negative). */
#define LUX_ECHO_BG_BLACK  0   /* bright material on black background */
#define LUX_ECHO_BG_WHITE  1   /* dark material on white background   */
#define LUX_ECHO_BG_AUTO   2   /* detect from the stream (default)    */

/* One stored feedback line (ENERGY space — bg conversion already applied). */
typedef struct {
    uint8_t r[LUX_ECHO_MAX_PIXELS];
    uint8_t g[LUX_ECHO_MAX_PIXELS];
    uint8_t b[LUX_ECHO_MAX_PIXELS];
    int     pixel_count;
} LuxEchoSlot;

/* ============================================================================
 * LuxEchoConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   background_mode;   /* LUX_ECHO_BG_* */
    int   delay_lines;       /* 1..LUX_ECHO_MAX_DELAY */
    float feedback;          /* 0..0.95 — repeat regeneration */
    float mix;               /* 0..1 — repeat level in the output */
} LuxEchoConfig;

/* ============================================================================
 * LuxEchoState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxEchoConfig config;

    LuxEchoSlot   ring[LUX_ECHO_RING_SLOTS];
    uint32_t      write_pos;      /* total lines pushed since last clear */
    int           ring_active;    /* nonzero while the ring may hold energy */
    int           last_bg_mode;   /* RESOLVED polarity the history was built in */

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE; a dense
     * fortissimo line must not flip it and invalidate the ring). */
    int           auto_bg_white;  /* current AUTO verdict (init: white/paper) */
    int           auto_locked;
    int           auto_lock_countdown;
    int           auto_max_mean;
    int           auto_min_mean;

    /* Background's own energy, slow EMA updated ONLY on near-background lines
     * — a per-frame estimate would balloon during dense passages and shave the
     * material out of the ring. -1 = unseeded. */
    float         floor_ema;

    /* Preallocated output buffers. */
    uint8_t       out_r[LUX_ECHO_MAX_PIXELS];
    uint8_t       out_g[LUX_ECHO_MAX_PIXELS];
    uint8_t       out_b[LUX_ECHO_MAX_PIXELS];
} LuxEchoState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void          lux_echo_init(LuxEchoState *state);
void          lux_echo_reset(LuxEchoState *state);   /* ring cleared, config untouched */
LuxEchoConfig lux_echo_config_default(void);

/* ── Frame processing ──────────────────────────────────────────────────────── */
/*
 * Process one RGB line. Output is allocated inside `state` (out_r/g/b). When
 * the module is disabled (or fully dry) the input pointers are returned as-is
 * (O(1) pass-through after the one-shot lazy ring re-anchor).
 */
void lux_echo_process_frame(
    LuxEchoState   *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b);

/* ── Global instance + per-chain pool (mirrors LuxPitch/LuxMask) ───────────── */
extern LuxEchoState g_lux_echo_proc;
LuxEchoState *lux_echo_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void          lux_echo_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_ECHO_H */
