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
 * Memory — the ring lives on the HEAP and is sized to the delay in use:
 *   slots  = delay + 1 rounded up to LUX_ECHO_SLOT_QUANTUM lines
 *   stride = the widest line the instance has met (CIS = 1728 / 3456 px)
 *   bytes  = slots × 3 × stride     (30 000 lines × 3456 px ≈ 311 MB)
 * The MESSAGE thread owns allocation (lux_echo_ensure_capacity, called from
 * the config sync) and hands the new ring to the synthesis thread through a
 * one-slot mailbox (`pending`); the synthesis thread adopts it at its next
 * frame and hands the previous ring back through `retired`, freed by the
 * message thread on its next call. Every instance across the pool is charged
 * against LUX_ECHO_POOL_BUDGET; past it the request is refused and the delay
 * is clamped to the ring in place (`capacity`, read by the editor).
 * Untouched pages of a fresh ring stay virtual until the writer reaches them.
 *
 * RT-safety: Pure C, allocation-free on the synthesis thread, bounded O(N).
 *            No JUCE deps, no mutex, no logging. Ring produced AND consumed
 *            on the synthesis thread only; the mailboxes are lock-free
 *            single-producer / single-consumer pointer exchanges.
 *
 * Author: zhonx
 * Created: 2026-07-03
 * 2026-09-08: heap ring sized to the delay (1 → 30 000 lines).
 */

#ifndef LUX_ECHO_H
#define LUX_ECHO_H

#include <stdint.h>
#include <stddef.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-chain instance pool size */

/* Atomic discipline mirrors video_scroll.h's VS_ATOMIC: on the C++ side the
 * mailbox fields are plain `volatile T` (the C++ code never touches them —
 * it goes through the functions below), on the C side real _Atomic T. The
 * pointer typedef keeps `_Atomic LuxEchoRingPtr` an atomic POINTER (the
 * naive `_Atomic LuxEchoRing *` would be a pointer to an atomic struct). */
#ifdef __cplusplus
  #include <atomic>
  #define LE_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define LE_ATOMIC(T) _Atomic T
#endif

/* Capacity matches LuxPitch/LuxMask (>6912 for 400 DPI CIS). */
#define LUX_ECHO_MAX_PIXELS    8192

/* Delay range, in lines. 30 000 lines ≈ 30 s at the CIS's ~1000 lps (two
 * minutes on a 250 lps source). */
#define LUX_ECHO_MAX_DELAY     30000

/* Ring sizing (lines). The smallest ring = the legacy 256-slot footprint. */
#define LUX_ECHO_MIN_SLOTS     256
#define LUX_ECHO_SLOT_QUANTUM  256

/* Heap ceiling for the whole pool (all instances, rings in flight included). */
#define LUX_ECHO_POOL_BUDGET   ((size_t) 1536u << 20)   /* 1.5 GB */

/* Background mode — which pole is the "material" (mirrors LUX_MASK_BG_*).
 * AUTO resolves per frame from the line's mean level (see lux_reverb.h — the
 * chain inserts see the RAW image, upstream of the synth's Negative). */
#define LUX_ECHO_BG_BLACK  0   /* bright material on black background */
#define LUX_ECHO_BG_WHITE  1   /* dark material on white background   */
#define LUX_ECHO_BG_AUTO   2   /* detect from the stream (default)    */

/* ============================================================================
 * LuxEchoRing — one heap ring of stored feedback lines (ENERGY space, bg
 * conversion already applied). Slot i, channel c (0=R,1=G,2=B) starts at
 * bytes + ((size_t) i * 3 + c) * stride; px[i] = pixels written in slot i.
 * ============================================================================ */
typedef struct LuxEchoRing {
    uint8_t *bytes;
    int     *px;
    int      slots;    /* lines the ring holds — delay ≤ slots - 1 */
    int      stride;   /* pixels per channel per slot */
    size_t   nbytes;   /* charged against LUX_ECHO_POOL_BUDGET */
} LuxEchoRing;

typedef LuxEchoRing *LuxEchoRingPtr;

/* ============================================================================
 * LuxEchoConfig — Parameters synced from APVTS (image thread copy).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   background_mode;   /* LUX_ECHO_BG_* */
    int   delay_lines;       /* 1..LUX_ECHO_MAX_DELAY (clamped to the ring) */
    float feedback;          /* 0..0.95 — repeat regeneration */
    float mix;               /* 0..1 — repeat level in the output */
} LuxEchoConfig;

/* ============================================================================
 * LuxEchoState — Complete runtime state.
 * ============================================================================ */
typedef struct {
    LuxEchoConfig config;

    /* ── Ring + mailboxes (see the header comment) ──────────────────────────
     * ring     : the ring in use — written by the synthesis thread only;
     *            the message thread reads the pointer to size its requests.
     * pending  : message → synthesis, a ring to adopt (one slot).
     * retired  : synthesis → message, the ring just replaced (one slot).
     * seen_px  : widest line the synthesis thread met — the stride request.
     * capacity : lines the adopted ring can delay (editor readout). */
    LE_ATOMIC(LuxEchoRingPtr) ring;
    LE_ATOMIC(LuxEchoRingPtr) pending;
    LE_ATOMIC(LuxEchoRingPtr) retired;
    LE_ATOMIC(int)            seen_px;
    LE_ATOMIC(int)            capacity;

    uint32_t      write_pos;      /* next slot to write (wrapped index)   */
    uint32_t      lines_pushed;   /* lines stored since the last re-anchor —
                                   * gates the reads (no memset of the ring) */
    int           ring_active;    /* nonzero while the ring may hold energy —
                                   * a latch, cleared only by a reset */

    /* Rack-LED heartbeat: bumped once per line the module actually CHANGED
     * (output != input), i.e. a repeat printed. See lux_reverb.h. */
    uint32_t      active_ticks;
    int           last_bg_mode;   /* RESOLVED polarity the history was built in */

    /* Consecutive lines whose STORED feedback held no material (peak below one
     * uint8 LSB). While it stays under `delay_lines` the delay window still
     * carries a repeat that has not been printed yet — that is exactly what
     * lux_echo_tail_alive() answers. Saturates at LUX_ECHO_MAX_DELAY + 1. */
    int           quiet_lines;

    /* AUTO background — learned over a short window after each reset, then
     * LOCKED (see lux_reverb.h: polarity is a property of the SOURCE; a dense
     * fortissimo line must not flip it and invalidate the ring). */
    int           auto_bg_white;  /* current AUTO verdict (init: white/paper) */
    int           auto_locked;
    int           auto_lock_countdown;
    int           auto_max_mean;
    int           auto_min_mean;

    /* Paper level — EMA of the per-line 10th-percentile energy (see
     * lux_drive.c: the gated mean-based floor stored the paper pedestal
     * into the ring on dense streams). -1 = unseeded. */
    float         floor_ema;

    /* Preallocated output buffers. */
    uint8_t       out_r[LUX_ECHO_MAX_PIXELS];
    uint8_t       out_g[LUX_ECHO_MAX_PIXELS];
    uint8_t       out_b[LUX_ECHO_MAX_PIXELS];
} LuxEchoState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void          lux_echo_init(LuxEchoState *state);    /* once, before any thread runs */
void          lux_echo_reset(LuxEchoState *state);   /* ring re-anchored, config untouched */
LuxEchoConfig lux_echo_config_default(void);

/* ── Ring capacity — MESSAGE THREAD ONLY ───────────────────────────────────── */
/*
 * Make sure the instance can delay `delay_lines` lines of at least `px_hint`
 * pixels (the wider of px_hint and what the synthesis thread reported wins).
 * delay_lines <= 0 = the module is off: a ring larger than the minimum is
 * traded for a minimum one (memory given back). Allocates and hands off a
 * new ring only when the one in place (or already pending) does not fit.
 * Also frees whatever the synthesis thread retired. Returns 1 when the
 * capacity is, or is about to be, sufficient; 0 when the pool budget refused
 * the request (the delay then clamps to `capacity`).
 */
int    lux_echo_ensure_capacity(LuxEchoState *state, int delay_lines, int px_hint);

/* 1 when the synthesis thread met lines wider than the ring can store — the
 * caller should re-run its config sync (which calls ensure_capacity). */
int    lux_echo_wants_resync(const LuxEchoState *state);

/* Free the ring(s) the synthesis thread retired. Cheap; call from a timer. */
void   lux_echo_collect(LuxEchoState *state);
void   lux_echo_collect_all(void);

/* Release every ring of every instance — synthesis thread STOPPED. */
void   lux_echo_shutdown_all(void);

/* Heap bytes currently held by the pool (rings in flight included). */
size_t lux_echo_pool_bytes(void);

/* Lines the ring in use can delay (0 = no ring yet). Any thread. */
int    lux_echo_capacity(const LuxEchoState *state);

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

/* ── Tail runout ───────────────────────────────────────────────────────────── */
/*
 * 1 while the delay window still holds a repeat to print. An echo is defined
 * by what it keeps printing after its input goes silent, so the chain executor
 * keeps walking a chain that lost its feed until every tail-bearing insert
 * answers 0 here — without it, stopping the source cuts the repeat train on
 * the spot. O(1).
 */
int lux_echo_tail_alive(const LuxEchoState *state);

/* ── Global instance + per-chain pool (mirrors LuxPitch/LuxMask) ───────────── */
extern LuxEchoState g_lux_echo_proc;
LuxEchoState *lux_echo_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void          lux_echo_init_all(void);      /* init every pool instance */

#ifdef __cplusplus
}
#endif

#endif /* LUX_ECHO_H */
