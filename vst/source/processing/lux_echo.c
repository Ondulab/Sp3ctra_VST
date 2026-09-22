/*
 * lux_echo.c
 *
 * LuxEcho — echo / delay implementation (see lux_echo.h).
 *
 * Per-frame pipeline (energy space, per channel):
 *   1. delayed = ring[write_pos - delay_lines]   (silent until the ring fills)
 *   2. out     = sat255(in + mix      * delayed)
 *   3. ring[write_pos++] = sat255(in + feedback * delayed)
 *
 * The ring is never memset: reads are gated by `lines_pushed >= delay`, so a
 * re-anchor (write_pos = lines_pushed = 0) makes stale slots unreachable
 * without touching the history on the RT thread.
 *
 * Ring ownership (see lux_echo.h): allocated / freed on the MESSAGE thread,
 * adopted / retired on the SYNTHESIS thread through two one-slot mailboxes.
 *
 * RT-safety: Pure C, allocation-free on the synthesis thread, bounded O(N).
 *
 * Author: zhonx
 * Created: 2026-07-03
 */

#include "lux_echo.h"
#include <stdlib.h>
#include <string.h>

/* ── Instance pool (mirrors lux_mask.c) ────────────────────────────────────────
 * Slot 0 is g_lux_echo_proc (also read by the UI). Slots 1.. are the
 * independent per-chain instances. */
LuxEchoState g_lux_echo_proc;
static LuxEchoState s_lux_echo_extra[CHAIN_MAX_CHAINS - 1];

/* Heap held by every ring of the pool, in flight included. MESSAGE thread. */
static size_t s_pool_bytes = 0;

/* Width the last capacity request was sized for, per instance — what
 * lux_echo_wants_resync compares the synthesis thread's report against, so a
 * request the budget refused does not re-trigger a sync every timer tick. */
static int s_asked_px[CHAIN_MAX_CHAINS];

LuxEchoState *lux_echo_instance(int idx)
{
    if (idx <= 0)
        return &g_lux_echo_proc;
    if (idx >= CHAIN_MAX_CHAINS)
        idx = CHAIN_MAX_CHAINS - 1;
    return &s_lux_echo_extra[idx - 1];
}

static int lux_echo_index_of(const LuxEchoState *state)
{
    if (state == &g_lux_echo_proc) return 0;
    for (int i = 1; i < CHAIN_MAX_CHAINS; ++i)
        if (state == &s_lux_echo_extra[i - 1]) return i;
    return 0;
}

void lux_echo_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_echo_init(lux_echo_instance(i));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxEchoConfig lux_echo_config_default(void)
{
    LuxEchoConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled         = 0;
    cfg.background_mode = LUX_ECHO_BG_AUTO;
    cfg.delay_lines     = 48;
    cfg.feedback        = 0.35f;
    cfg.mix             = 0.6f;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
/* AUTO learning window (lines) — mirrors LUX_REVERB_BG_LOCK_LINES. */
#define LUX_ECHO_BG_LOCK_LINES 96

/* Re-anchor only: the ring's bytes are left alone (see the file comment). */
void lux_echo_reset(LuxEchoState *state)
{
    if (!state) return;
    state->write_pos    = 0;
    state->lines_pushed = 0;
    state->ring_active  = 0;
    state->last_bg_mode = -1;
    /* Nothing recorded → nothing to run out (see lux_echo_tail_alive). */
    state->quiet_lines  = LUX_ECHO_MAX_DELAY + 1;
    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = LUX_ECHO_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;
}

/* The ring and its mailboxes are deliberately NOT touched here: on first
 * init they are the zeroed BSS (no ring — the first config sync supplies
 * one), and a second plugin instance re-runs init_all under a core whose
 * synthesis thread may be using the rings (freeing is the destructor's job,
 * lux_echo_shutdown_all, last core user only). */
void lux_echo_init(LuxEchoState *state)
{
    if (!state) return;
    state->config = lux_echo_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */
    state->active_ticks  = 0;   /* seeded HERE, never in reset (see lux_reverb.c) */
    lux_echo_reset(state);
}

/* ── Ring allocation — MESSAGE THREAD ──────────────────────────────────────── */
static void lux_echo_ring_free(LuxEchoRing *r)
{
    if (!r) return;
    s_pool_bytes -= (r->nbytes <= s_pool_bytes) ? r->nbytes : s_pool_bytes;
    free(r->bytes);
    free(r->px);
    free(r);
}

/* `replacing` = bytes of the ring this one will stand in for: a replacement
 * no bigger than what it retires is always granted (that is how memory is
 * given back), a growth must fit under the pool budget. Pages are not
 * touched here — a fresh ring stays virtual until the writer reaches it. */
static LuxEchoRing *lux_echo_ring_alloc(int slots, int stride, size_t replacing)
{
    const size_t data = (size_t) slots * 3u * (size_t) stride;
    const size_t meta = (size_t) slots * sizeof(int) + sizeof(LuxEchoRing);
    const size_t nbytes = data + meta;
    if (nbytes > replacing && s_pool_bytes + nbytes > LUX_ECHO_POOL_BUDGET)
        return NULL;

    LuxEchoRing *r = (LuxEchoRing *) calloc(1, sizeof(LuxEchoRing));
    if (!r) return NULL;
    r->bytes = (uint8_t *) malloc(data);
    r->px    = (int *) calloc((size_t) slots, sizeof(int));
    if (!r->bytes || !r->px)
    {
        free(r->bytes);
        free(r->px);
        free(r);
        return NULL;
    }
    r->slots  = slots;
    r->stride = stride;
    r->nbytes = nbytes;
    s_pool_bytes += nbytes;
    return r;
}

void lux_echo_collect(LuxEchoState *state)
{
    if (!state) return;
    LuxEchoRing *r = atomic_exchange_explicit(&state->retired, NULL, memory_order_acq_rel);
    if (r) lux_echo_ring_free(r);
}

void lux_echo_collect_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        lux_echo_collect(lux_echo_instance(i));
}

int lux_echo_ensure_capacity(LuxEchoState *state, int delay_lines, int px_hint)
{
    if (!state) return 0;
    lux_echo_collect(state);

    /* Stride: the wider of the caller's hint and what the stream showed. */
    int want_px = px_hint;
    const int seen = atomic_load_explicit(&state->seen_px, memory_order_relaxed);
    if (seen > want_px)                want_px = seen;
    if (want_px < 64)                  want_px = 64;
    if (want_px > LUX_ECHO_MAX_PIXELS) want_px = LUX_ECHO_MAX_PIXELS;
    want_px = (want_px + 63) & ~63;
    s_asked_px[lux_echo_index_of(state)] = want_px;

    /* Length: delay + 1 lines, quantised; the minimum ring when off. */
    int want_slots = LUX_ECHO_MIN_SLOTS;
    if (delay_lines > 0)
    {
        const int d = (delay_lines > LUX_ECHO_MAX_DELAY) ? LUX_ECHO_MAX_DELAY : delay_lines;
        want_slots = ((d + 1) + LUX_ECHO_SLOT_QUANTUM - 1) / LUX_ECHO_SLOT_QUANTUM
                   * LUX_ECHO_SLOT_QUANTUM;
        if (want_slots < LUX_ECHO_MIN_SLOTS) want_slots = LUX_ECHO_MIN_SLOTS;
    }

    /* The ring the synthesis thread will be using next: a pending one wins.
     * Whichever we read stays valid — only this thread frees rings. */
    LuxEchoRing *cur = atomic_load_explicit(&state->pending, memory_order_acquire);
    if (!cur) cur = atomic_load_explicit(&state->ring, memory_order_acquire);

    const int fits    = (cur != NULL) && cur->stride >= want_px && cur->slots >= want_slots;
    const int bloated = (cur != NULL) && delay_lines <= 0 && cur->slots > LUX_ECHO_MIN_SLOTS;
    if (fits && !bloated)
        return 1;

    int stride = want_px;
    if (cur && cur->stride > stride) stride = cur->stride;   /* never narrow a ring */

    LuxEchoRing *nr = lux_echo_ring_alloc(want_slots, stride, cur ? cur->nbytes : 0);
    if (!nr)
        return fits;   /* budget refused — the ring in place stays, delay clamps */

    /* Publish. A ring still waiting in the mailbox was never adopted, so it
     * is ours to free; the exchange keeps that from racing the adoption. */
    LuxEchoRing *unadopted = atomic_exchange_explicit(&state->pending, nr, memory_order_acq_rel);
    if (unadopted) lux_echo_ring_free(unadopted);
    return 1;
}

int lux_echo_wants_resync(const LuxEchoState *state)
{
    if (!state) return 0;
    const int seen = atomic_load_explicit(&state->seen_px, memory_order_relaxed);
    return seen > s_asked_px[lux_echo_index_of(state)] ? 1 : 0;
}

void lux_echo_shutdown_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
    {
        LuxEchoState *st = lux_echo_instance(i);
        lux_echo_ring_free(atomic_exchange_explicit(&st->ring,    NULL, memory_order_acq_rel));
        lux_echo_ring_free(atomic_exchange_explicit(&st->pending, NULL, memory_order_acq_rel));
        lux_echo_ring_free(atomic_exchange_explicit(&st->retired, NULL, memory_order_acq_rel));
        atomic_store_explicit(&st->capacity, 0, memory_order_relaxed);
        s_asked_px[i] = 0;
    }
}

size_t lux_echo_pool_bytes(void)
{
    return s_pool_bytes;
}

int lux_echo_capacity(const LuxEchoState *state)
{
    return state ? atomic_load_explicit(&state->capacity, memory_order_relaxed) : 0;
}

/* ── Ring adoption — SYNTHESIS THREAD ──────────────────────────────────────── */
static inline uint8_t *lux_echo_slot(const LuxEchoRing *ring, uint32_t slot, int ch)
{
    return ring->bytes + ((size_t) slot * 3u + (size_t) ch) * (size_t) ring->stride;
}

/* Take the ring the message thread handed over, if any, and hand back the
 * one in use. History restarts: the new ring is empty by construction. */
static void lux_echo_adopt(LuxEchoState *state)
{
    LuxEchoRing *pend = atomic_exchange_explicit(&state->pending, NULL, memory_order_acq_rel);
    if (!pend) return;

    LuxEchoRing *old = atomic_exchange_explicit(&state->ring, pend, memory_order_acq_rel);
    state->write_pos    = 0;
    state->lines_pushed = 0;
    state->quiet_lines  = LUX_ECHO_MAX_DELAY + 1;
    atomic_store_explicit(&state->capacity, pend->slots - 1, memory_order_relaxed);

    if (old)
    {
        /* Invariant: the retired slot is empty here — ensure_capacity collects
         * before it publishes, and one publish yields at most one adoption.
         * Never free on this thread. */
        LuxEchoRing *stale = atomic_exchange_explicit(&state->retired, old, memory_order_acq_rel);
        (void) stale;
    }
}

/* Resolve the background pole for this frame (see lux_reverb_resolve_bg — AUTO
 * learns over a short window then LOCKS: a dense fortissimo line must never
 * flip the polarity and invalidate the ring) and report the background's OWN
 * energy (*out_floor): the paper is never exactly at the zero pole (white ≈
 * 230, energy ≈ 25), and adding that offset onto itself at every repeat would
 * veil the whole image — the ring must carry the MATERIAL energy only (input
 * minus floor). The floor is an EMA of the per-line 10th-PERCENTILE energy —
 * the canonical grey-bands fix (see lux_drive_resolve_bg): the previous
 * gated mean-based EMA seeded at 0 on dense passages, so the paper pedestal
 * was stored into the ring and re-printed as delayed grey bands. A low
 * percentile finds the paper between the strokes even on dense lines. */
static int lux_echo_resolve_bg(LuxEchoState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int px, float *out_floor)
{
    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        sum += (uint32_t)in_r[i] + in_g[i] + in_b[i];
        n   += 3;
    }
    const int mean = (n > 0) ? (int)(sum / (uint32_t)n) : 255;

    int bg_white;
    const int mode = state->config.background_mode;
    if (mode == LUX_ECHO_BG_BLACK)      bg_white = 0;
    else if (mode == LUX_ECHO_BG_WHITE) bg_white = 1;
    else if (state->auto_locked)        bg_white = state->auto_bg_white;
    else
    {
        if (mean > state->auto_max_mean) state->auto_max_mean = mean;
        if (mean < state->auto_min_mean) state->auto_min_mean = mean;
        state->auto_bg_white = (state->auto_max_mean + state->auto_min_mean > 255) ? 1 : 0;
        if (--state->auto_lock_countdown <= 0)
            state->auto_locked = 1;
        bg_white = state->auto_bg_white;
    }

    /* 10th-percentile energy over a 32-bin histogram of sampled pixels. */
    int hist[32] = { 0 };
    int ns = 0;
    for (int i = 0; i < px; i += 4)
    {
        const int v = ((int)in_r[i] + in_g[i] + in_b[i]) / 3;
        const int e = bg_white ? 255 - v : v;
        hist[e >> 3]++;
        ns++;
    }
    const int target = ns / 10;
    int acc = 0, bin = 0;
    for (; bin < 31; ++bin)
    {
        acc += hist[bin];
        if (acc > target)
            break;
    }
    const float inst_floor = (float)(bin * 8 + 4);

    /* EMA every line (1/16) — a percentile needs no "line is background"
     * gate, and reseeds honestly right after a reset. */
    if (state->floor_ema < 0.0f)
        state->floor_ema = inst_floor;
    else
        state->floor_ema += (inst_floor - state->floor_ema) * (1.0f / 16.0f);

    *out_floor = state->floor_ema;
    return bg_white;
}

/* ── Frame processing ──────────────────────────────────────────────────────── */

/* Mix + feedback for one channel. `del`/`del_px` may be NULL/0 (ring not yet
 * filled up to the delay): repeats are silent but the line is still recorded.
 * The ring carries MATERIAL energy only (input minus the background floor), so
 * repeats re-print the strokes without stacking the paper's own level.
 * `fb_px` (≤ px) is what the slot can hold — a line wider than the ring's
 * stride is echoed on its first `fb_px` pixels only, until the message thread
 * supplies a wider ring. `*peak` accumulates the max STORED feedback energy
 * across channels — the runout's "this slot still carries a repeat" measure. */
static int lux_echo_channel(
    const uint8_t *in, const uint8_t *del, int del_px,
    uint8_t *out, uint8_t *fb,
    int px, int fb_px, int bg_white, float floor_e, float mix, float feedback,
    float *peak)
{
    int diff = 0;   /* OR of out^in — no audible repeat leaves the line intact */

    for (int i = 0; i < px; i++)
    {
        const float e_in  = bg_white ? (float)(255 - in[i]) : (float)in[i];
        const float e_del = (del && i < del_px) ? (float)del[i] : 0.0f;

        float e_out = e_in + mix * e_del;
        if (e_out > 255.0f) e_out = 255.0f;

        out[i] = bg_white ? (uint8_t)(255.0f - e_out) : (uint8_t)e_out;
        diff  |= out[i] ^ in[i];

        if (i < fb_px)
        {
            float e_mat = e_in - floor_e;
            if (e_mat < 0.0f) e_mat = 0.0f;
            float e_fb = e_mat + feedback * e_del;
            if (e_fb > 255.0f) e_fb = 255.0f;
            fb[i] = (uint8_t)e_fb;
            if (e_fb > *peak) *peak = e_fb;
        }
    }
    return diff;
}

void lux_echo_process_frame(
    LuxEchoState   *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    (void)luxstral_num_octaves;

    *out_r = in_r; *out_g = in_g; *out_b = in_b;
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    /* A ring handed over by the message thread is taken up first — even a
     * disabled module adopts, so a shrink (module off) reclaims its memory. */
    lux_echo_adopt(state);

    const LuxEchoConfig *cfg = &state->config;
    if (!cfg->enabled || (cfg->mix <= 0.001f && cfg->feedback <= 0.001f))
    {
        /* Lazy one-shot re-anchor so a re-enable doesn't replay stale lines. */
        if (state->ring_active)
            lux_echo_reset(state);
        return;
    }

    int px = pixel_count;
    if (px > LUX_ECHO_MAX_PIXELS) px = LUX_ECHO_MAX_PIXELS;

    /* Report the line width — the message thread sizes the stride from it. */
    if (px > atomic_load_explicit(&state->seen_px, memory_order_relaxed))
        atomic_store_explicit(&state->seen_px, px, memory_order_relaxed);

    LuxEchoRing *ring = atomic_load_explicit(&state->ring, memory_order_relaxed);
    if (!ring)
        return;   /* no ring yet — pass-through until the first config sync */

    /* The ring stores energy-space lines: a polarity flip invalidates it.
     * Re-anchor the ring + floor ONLY — a full reset would re-arm the AUTO
     * learning window and throw away the extremes it just learned from. */
    float floor_e = 0.0f;
    const int bg_white = lux_echo_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    if (state->last_bg_mode != bg_white)
    {
        state->write_pos    = 0;
        state->lines_pushed = 0;
        state->floor_ema    = -1.0f;
        state->last_bg_mode = bg_white;
    }
    state->ring_active = 1;

    const int slots = ring->slots;
    int delay = cfg->delay_lines;
    if (delay < 1)                  delay = 1;
    if (delay > LUX_ECHO_MAX_DELAY) delay = LUX_ECHO_MAX_DELAY;
    if (delay > slots - 1)          delay = slots - 1;   /* budget-refused growth */

    const uint8_t *dR = NULL, *dG = NULL, *dB = NULL;
    int del_px = 0;
    if (state->lines_pushed >= (uint32_t) delay)
    {
        uint32_t rd = state->write_pos + (uint32_t) slots - (uint32_t) delay;
        if (rd >= (uint32_t) slots) rd -= (uint32_t) slots;
        dR = lux_echo_slot(ring, rd, 0);
        dG = lux_echo_slot(ring, rd, 1);
        dB = lux_echo_slot(ring, rd, 2);
        del_px = ring->px[rd];
    }

    const uint32_t wp = state->write_pos;
    uint8_t *wR = lux_echo_slot(ring, wp, 0);
    uint8_t *wG = lux_echo_slot(ring, wp, 1);
    uint8_t *wB = lux_echo_slot(ring, wp, 2);
    const int fb_px = (px < ring->stride) ? px : ring->stride;

    const float mix = (cfg->mix > 1.0f) ? 1.0f : cfg->mix;
    float feedback = cfg->feedback;
    if (feedback < 0.0f)   feedback = 0.0f;
    if (feedback > 0.95f)  feedback = 0.95f;

    float fb_peak = 0.0f;
    int diff = lux_echo_channel(in_r, dR, del_px, state->out_r, wR, px, fb_px,
                                bg_white, floor_e, mix, feedback, &fb_peak);
    diff |= lux_echo_channel(in_g, dG, del_px, state->out_g, wG, px, fb_px,
                             bg_white, floor_e, mix, feedback, &fb_peak);
    diff |= lux_echo_channel(in_b, dB, del_px, state->out_b, wB, px, fb_px,
                             bg_white, floor_e, mix, feedback, &fb_peak);
    if (diff)
        state->active_ticks++;

    /* Runout bookkeeping: the slot just written holds nothing above one LSB
     * (what (uint8_t)e_fb keeps) → count it quiet. Once the whole delay window
     * is quiet there is no repeat left anywhere in the ring. */
    if (fb_peak >= 1.0f)
        state->quiet_lines = 0;
    else if (state->quiet_lines <= LUX_ECHO_MAX_DELAY)
        state->quiet_lines++;

    ring->px[wp] = fb_px;
    state->write_pos = (wp + 1u >= (uint32_t) slots) ? 0u : wp + 1u;
    if (state->lines_pushed != 0xFFFFFFFFu)
        state->lines_pushed++;

    *out_r = state->out_r;
    *out_g = state->out_g;
    *out_b = state->out_b;
}

/* ── Tail runout ───────────────────────────────────────────────────────────── */
int lux_echo_tail_alive(const LuxEchoState *state)
{
    if (!state) return 0;

    const LuxEchoConfig *cfg = &state->config;
    /* A dry echo (mix 0) prints nothing however loud the ring is — feedback
     * alone is inaudible, so there is no tail to wait for. */
    if (!cfg->enabled || cfg->mix <= 0.001f || !state->ring_active)
        return 0;

    const int cap = atomic_load_explicit(&state->capacity, memory_order_relaxed);
    if (cap <= 0) return 0;

    int delay = cfg->delay_lines;
    if (delay < 1)   delay = 1;
    if (delay > cap) delay = cap;
    return (state->quiet_lines < delay) ? 1 : 0;
}
