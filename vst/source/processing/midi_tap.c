/*
 * midi_tap.c
 *
 * MIDI TAP — pass-through RT image probe extracting MIDI notes.
 * See midi_tap.h for the threading model, the RT-safety contract and the
 * extraction pipeline.
 *
 * Author: zhonx
 */
#include "midi_tap.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* ==========================================================================
 * Clock
 * ========================================================================== */
uint64_t midi_tap_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

/* ==========================================================================
 * Master transport
 * ========================================================================== */
MidiTapTransport g_midi_tap_transport;

uint64_t midi_tap_transport_arm(void)
{
    const uint64_t t0 = midi_tap_now_us();
    atomic_store_explicit(&g_midi_tap_transport.t0_us, t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_midi_tap_transport.run_id, 1u, memory_order_relaxed);
    /* Release: publishes t0_us + run_id to any sink that acquires `armed`. */
    atomic_store_explicit(&g_midi_tap_transport.armed, 1, memory_order_release);
    return t0;
}

void midi_tap_transport_disarm(void)
{
    /* t0_us is deliberately NOT cleared: a sink still draining its tail needs
     * it to stamp the last events of the take. */
    atomic_store_explicit(&g_midi_tap_transport.armed, 0, memory_order_release);
}

uint64_t midi_tap_transport_t0_us(void)
{
    return atomic_load_explicit(&g_midi_tap_transport.t0_us, memory_order_acquire);
}

int midi_tap_transport_armed(void)
{
    return atomic_load_explicit(&g_midi_tap_transport.armed, memory_order_acquire);
}

uint32_t midi_tap_transport_run_id(void)
{
    return atomic_load_explicit(&g_midi_tap_transport.run_id, memory_order_acquire);
}

/* ==========================================================================
 * Pool
 * ========================================================================== */
MidiTapState        g_midi_tap_proc;
static MidiTapState s_midi_tap_extra[CHAIN_MAX_CHAINS - 1];

MidiTapState *midi_tap_instance(int idx)
{
    if (idx <= 0) return &g_midi_tap_proc;
    if (idx >= CHAIN_MAX_CHAINS) idx = CHAIN_MAX_CHAINS - 1;
    return &s_midi_tap_extra[idx - 1];
}

void midi_tap_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        midi_tap_init(midi_tap_instance(i));
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */
MidiTapConfig midi_tap_config_default(void)
{
    MidiTapConfig c;
    memset(&c, 0, sizeof(c));
    c.enabled          = 0;
    c.source           = MIDI_TAP_SRC_LUMA;
    c.mode             = MIDI_TAP_MODE_BANDS;
    c.max_poly         = 8;
    c.thresh           = 12.0f;
    c.hyst             = 0.6f;
    c.rel              = 0.25f;
    c.smooth           = 0.5f;
    c.peak_only        = 1;
    c.attack_ms        = 12.0f;
    c.release_ms       = 40.0f;
    c.min_on_ms        = 60.0f;
    c.max_on_ms        = 8000.0f;
    c.vel_span         = 96.0f;
    c.vel_curve        = MIDI_TAP_VEL_SOFT;
    c.vel_fixed        = 100;
    c.note_lo          = 0;
    c.note_hi          = 127;
    c.transpose        = 0;
    c.range_policy     = MIDI_TAP_RANGE_CLAMP;
    c.background_mode  = MIDI_TAP_BG_AUTO;
    c.axis_low_hz      = 65.406f;
    c.max_events_per_s = 200.0f;
    c.dense            = 0;
    c.retrig_ms        = 10.0f;
    c.retrig_delta     = 5;
    return c;
}

void midi_tap_reset(MidiTapState *state)
{
    if (!state) return;
    state->tap_active   = 0;
    state->last_bg_mode = -1;
    state->lut_valid    = 0;
    state->lut_px       = 0;
    state->scan_lo      = 0;
    state->scan_hi      = -1;

    /* Re-arm the AUTO learning window + floor tracker. */
    state->auto_locked         = 0;
    state->auto_lock_countdown = MIDI_TAP_BG_LOCK_LINES;
    state->auto_max_mean       = 0;
    state->auto_min_mean       = 255;
    state->floor_ema           = -1.0f;

    memset(state->bands,  0, sizeof(state->bands));
    memset(state->is_top, 0, sizeof(state->is_top));
    memset(state->score,  0, sizeof(state->score));

    state->last_line_us  = 0;
    state->lps           = 250.0f;   /* the live scanner's nominal rate */
    state->lps_derived   = 0.0f;     /* forces a derive on the first line */
    state->attack_lines  = 1;
    state->release_lines = 1;
    state->min_on_lines  = 1;
    state->max_on_lines  = 0;
    state->budget        = 0.0f;

    atomic_store_explicit(&state->voice_count, 0u, memory_order_relaxed);
}

void midi_tap_init(MidiTapState *state)
{
    if (!state) return;
    state->config        = midi_tap_config_default();
    state->auto_bg_white = 1;   /* paper is the typical Sp3ctra stream */

    memset(state->ring, 0, sizeof(state->ring));

    /* atomic_store, NOT atomic_init: this also runs as a RE-init (module
     * removed from a chain) on atomics that sinks may still be loading —
     * atomic_init on a live atomic is undefined behaviour. */
    atomic_store_explicit(&state->write_index, 0u, memory_order_release);
    atomic_store_explicit(&state->active_ticks, 0u, memory_order_relaxed);
    atomic_store_explicit(&state->limited,      0ull, memory_order_relaxed);
    /* generation is bumped, never reset: a sink comparing generations across a
     * re-init must SEE a change (write_index goes back to 0 under it). */
    atomic_fetch_add_explicit(&state->generation, 1u, memory_order_release);

    midi_tap_reset(state);
}

/* ==========================================================================
 * Ring — single producer, N independent consumers (broadcast window)
 * ========================================================================== */
static inline void midi_tap_push(MidiTapState *state, uint64_t t_us,
                                 uint8_t status, uint8_t note, uint8_t vel)
{
    const uint32_t w = atomic_load_explicit(&state->write_index, memory_order_relaxed);
    MidiTapEvent  *e = &state->ring[w & MIDI_TAP_RING_MASK];
    e->t_us   = t_us;
    e->status = status;
    e->note   = note;
    e->vel    = vel;
    e->flags  = 0;
    e->pad    = 0;
    /* Single release store publishes the slot. */
    atomic_store_explicit(&state->write_index, w + 1u, memory_order_release);
}

uint32_t midi_tap_ring_writepos(const MidiTapState *state)
{
    if (!state) return 0;
    return atomic_load_explicit(&state->write_index, memory_order_acquire);
}

uint32_t midi_tap_ring_available(const MidiTapState *state, uint32_t cursor,
                                 uint32_t *out_dropped)
{
    if (out_dropped) *out_dropped = 0;
    if (!state) return 0;

    const uint32_t w = atomic_load_explicit(&state->write_index, memory_order_acquire);
    uint32_t avail = w - cursor;          /* wrap-safe unsigned subtraction */
    if ((int32_t) avail < 0) return 0;    /* cursor ahead of the writer (re-init) */
    if (avail > (uint32_t) MIDI_TAP_RING_SLOTS)
    {
        if (out_dropped) *out_dropped = avail - (uint32_t) MIDI_TAP_RING_SLOTS;
        avail = (uint32_t) MIDI_TAP_RING_SLOTS;   /* overrun: newest window wins */
    }
    return avail;
}

/* Is absolute index `idx` inside the live window given write position `w`? */
static inline int midi_tap_idx_live(uint32_t w, uint32_t idx)
{
    if ((int32_t) (idx - w) >= 0) return 0;                   /* not produced yet */
    if ((uint32_t) (w - idx) > (uint32_t) MIDI_TAP_RING_SLOTS) return 0; /* lapped */
    return 1;
}

int midi_tap_ring_get(const MidiTapState *state, uint32_t idx, MidiTapEvent *out)
{
    if (!state || !out) return 0;
    if (!midi_tap_idx_live(midi_tap_ring_writepos(state), idx)) return 0;

    *out = state->ring[idx & MIDI_TAP_RING_MASK];

    /* Re-validate AFTER the copy: reject a tear if the producer lapped us
     * mid-copy. On a 0 return *out is INDETERMINATE (see the header). */
    if (!midi_tap_idx_live(midi_tap_ring_writepos(state), idx)) return 0;
    return 1;
}

uint32_t midi_tap_generation(const MidiTapState *state)
{
    if (!state) return 0;
    return atomic_load_explicit(&state->generation, memory_order_acquire);
}

uint32_t midi_tap_active_ticks(const MidiTapState *state)
{
    if (!state) return 0;
    return atomic_load_explicit(&state->active_ticks, memory_order_relaxed);
}

uint32_t midi_tap_voice_count(const MidiTapState *state)
{
    if (!state) return 0;
    return atomic_load_explicit(&state->voice_count, memory_order_relaxed);
}

uint64_t midi_tap_limited_count(const MidiTapState *state)
{
    if (!state) return 0;
    return atomic_load_explicit(&state->limited, memory_order_relaxed);
}

/* ==========================================================================
 * Note release helpers (shared by the line path, silence and panic)
 * ========================================================================== */
/* Release every held band, pushing its note-off. Returns the count released. */
static int midi_tap_release_all(MidiTapState *state, uint64_t t_us)
{
    int n = 0;
    for (int i = 0; i < MIDI_TAP_NUM_NOTES; ++i)
    {
        MidiTapBand *b = &state->bands[i];
        if (!b->held) continue;
        midi_tap_push(state, t_us, 0x80, (uint8_t) i, 0);
        b->held = 0; b->on_lines = 0; b->off_lines = 0; b->cand_lines = 0;
        ++n;
    }
    if (n) atomic_store_explicit(&state->voice_count, 0u, memory_order_relaxed);
    return n;
}

void midi_tap_silence(MidiTapState *state)
{
    if (!state) return;
    /* Cheap when nothing is held — the common case while a chain stays de-fed. */
    if (atomic_load_explicit(&state->voice_count, memory_order_relaxed) == 0u)
        return;
    midi_tap_release_all(state, midi_tap_now_us());
}

void midi_tap_panic(MidiTapState *state)
{
    if (!state) return;
    midi_tap_release_all(state, midi_tap_now_us());
}

/* ==========================================================================
 * Band geometry LUT
 * ========================================================================== */
static inline int midi_tap_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* band_start_px[n] = first pixel of EMITTED note n's Voronoi cell.
 *
 * Emitted note n corresponds to axis note (n - transpose), whose centre sits at
 * pixel (a - midi_low) * pps (LuxHarmo's degree-centre convention). The cell is
 * [a - 0.5, a + 0.5), so the boundary is at (a - 0.5 - midi_low) * pps.
 *
 * Notes whose cell falls entirely outside [0, px) collapse to a zero-width band
 * and are skipped by the scan window — that is how the 36..132 axis truncates
 * cleanly at MIDI 127 instead of folding an octave down. */
static void midi_tap_build_lut(MidiTapState *state, int px, int octaves)
{
    const MidiTapConfig *cfg = &state->config;
    const float low = (cfg->axis_low_hz > 0.0f) ? cfg->axis_low_hz : 65.406f;
    const int   oct = (octaves > 0) ? octaves : 8;
    const float pps = (float) px / ((float) oct * 12.0f);
    const float midi_low = 69.0f + 12.0f * log2f(low / 440.0f);

    for (int n = 0; n <= MIDI_TAP_NUM_NOTES; ++n)
    {
        const float a = (float) n - (float) cfg->transpose;
        const float x = (a - 0.5f - midi_low) * pps;
        state->band_start_px[n] = (int32_t) midi_tap_clampi((int) ceilf(x), 0, px);
    }

    int lo = midi_tap_clampi(cfg->note_lo, 0, MIDI_TAP_NUM_NOTES - 1);
    int hi = midi_tap_clampi(cfg->note_hi, 0, MIDI_TAP_NUM_NOTES - 1);
    if (lo > hi) { const int t = lo; lo = hi; hi = t; }

    /* Keep only the notes whose band actually covers pixels: that is what
     * truncates the 36..132 axis at MIDI 127 (zero-width bands are skipped). */
    state->scan_lo = 0;
    state->scan_hi = -1;
    int found = 0;
    for (int n = lo; n <= hi; ++n)
        if (state->band_start_px[n + 1] > state->band_start_px[n])
        {
            if (!found) { state->scan_lo = n; found = 1; }
            state->scan_hi = n;
        }

    state->lut_valid     = 1;
    state->lut_px        = px;
    state->lut_octaves   = oct;
    state->lut_transpose = cfg->transpose;
    state->lut_note_lo   = cfg->note_lo;
    state->lut_note_hi   = cfg->note_hi;
    state->lut_low_hz    = low;
}

/* ==========================================================================
 * Background polarity + floor. Polarity: mean-based AUTO learn-then-LOCK.
 * Paper level: EMA of the per-line 10th-PERCENTILE energy — the canonical
 * grey-bands fix (see lux_drive_resolve_bg): the previous sampled-minimum
 * estimator froze behind its "line is background" gate and seeded at 0 on
 * dense passages, so the paper pedestal counted as material in every band
 * (false triggers / inflated velocities tracking the ink mass).
 * ========================================================================== */
static int midi_tap_resolve_bg(MidiTapState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int px, float *out_floor)
{
    const int src = state->config.source;
    uint32_t sum = 0;
    int      n   = 0;
    for (int i = 0; i < px; i += 8)
    {
        const int pm = (src == MIDI_TAP_SRC_R) ? (int) in_r[i]
                     : (src == MIDI_TAP_SRC_G) ? (int) in_g[i]
                     : (src == MIDI_TAP_SRC_B) ? (int) in_b[i]
                     : ((int) in_r[i] + in_g[i] + in_b[i]) / 3;
        sum += (uint32_t) pm;
        n   += 1;
    }
    const int mean = (n > 0) ? (int) (sum / (uint32_t) n) : 255;

    int bg_white;
    const int mode = state->config.background_mode;
    if (mode == MIDI_TAP_BG_BLACK)      bg_white = 0;
    else if (mode == MIDI_TAP_BG_WHITE) bg_white = 1;
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

    /* A polarity flip invalidates the learned floor. */
    if (state->last_bg_mode >= 0 && state->last_bg_mode != bg_white)
        state->floor_ema = -1.0f;
    state->last_bg_mode = bg_white;

    /* 10th-percentile energy over a 32-bin histogram of sampled pixels —
     * same SOURCE channel as the band accumulation. */
    int hist[32] = { 0 };
    int ns = 0;
    for (int i = 0; i < px; i += 4)
    {
        const int pm = (src == MIDI_TAP_SRC_R) ? (int) in_r[i]
                     : (src == MIDI_TAP_SRC_G) ? (int) in_g[i]
                     : (src == MIDI_TAP_SRC_B) ? (int) in_b[i]
                     : ((int) in_r[i] + in_g[i] + in_b[i]) / 3;
        const int e = bg_white ? 255 - pm : pm;
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
    const float inst_floor = (float) (bin * 8 + 4);

    /* EMA every line (1/16) — a percentile needs no "line is background"
     * gate, and reseeds honestly right after a reset. */
    if (state->floor_ema < 0.0f)
        state->floor_ema = inst_floor;
    else
        state->floor_ema += (inst_floor - state->floor_ema) * (1.0f / 16.0f);

    *out_floor = state->floor_ema;
    return bg_white;
}

/* ==========================================================================
 * Harmonic Product Spectrum — the FUNDAMENTAL decision variable
 *
 * The pixel axis is LOG-frequency at a constant pixels-per-semitone density, so
 * harmonic k of a note always lands 12*log2(k) semitones above it whatever the
 * note is. The offsets below are therefore CONSTANTS, and the HPS collapses to
 * a handful of shifted multiplies — no search, no FFT.
 *
 * score[n] = geometric mean of the K harmonics of n that are inside the scan
 * window. The geometric mean (not the raw product) is what keeps the result in
 * the same 0..255 density units as BANDS mode, so `thresh` and `rel` mean the
 * same thing in both modes.
 *
 * Two deliberate details:
 *  - each factor is floored at 1.0 so ONE missing partial (very common on a
 *    voice, where the odd/even balance swings with the vowel) attenuates the
 *    score instead of annihilating it;
 *  - harmonics falling outside the window are DROPPED from the mean rather than
 *    counted as silence — otherwise every note in the top two octaves, whose
 *    harmonics are off-axis, would be crushed to nothing.
 * ========================================================================== */
static void midi_tap_build_hps(MidiTapState *state, int lo, int hi)
{
    /* round(12*log2(k)) for k = 1,2,3,4 */
    static const int kOff[MIDI_TAP_HARMONICS] = { 0, 12, 19, 24 };

    for (int n = lo; n <= hi; ++n)
    {
        float p = 1.0f;
        int   k = 0;
        for (int h = 0; h < MIDI_TAP_HARMONICS; ++h)
        {
            const int m = n + kOff[h];
            if (m > hi) break;              /* offsets ascend — nothing further fits */
            const float v = state->bands[m].e;
            p *= (v > 1.0f) ? v : 1.0f;
            ++k;
        }
        /* Geometric mean without powf: k is 1..4 by construction. */
        state->score[n] = (k == 4) ? sqrtf(sqrtf(p))
                        : (k == 3) ? cbrtf(p)
                        : (k == 2) ? sqrtf(p)
                        : (k == 1) ? p
                                   : 0.0f;
    }
}

/* ==========================================================================
 * Line rate → derived line counts
 * ========================================================================== */
static void midi_tap_derive_counts(MidiTapState *state)
{
    const MidiTapConfig *c = &state->config;
    const float lps = (state->lps > 1.0f) ? state->lps : 1.0f;
    state->attack_lines  = midi_tap_clampi((int) (c->attack_ms  * lps * 1e-3f), 1, 65535);
    state->release_lines = midi_tap_clampi((int) (c->release_ms * lps * 1e-3f), 1, 65535);
    state->min_on_lines  = midi_tap_clampi((int) (c->min_on_ms  * lps * 1e-3f), 1, 65535);
    state->max_on_lines  = (c->max_on_ms > 0.0f)
                         ? midi_tap_clampi((int) (c->max_on_ms * lps * 1e-3f), 1, 65535)
                         : 0;
    state->retrig_lines  = midi_tap_clampi((int) (c->retrig_ms  * lps * 1e-3f), 1, 65535);
    state->lps_derived = lps;
}

/* Velocity from the decision variable — shared by the strike and the DENSE
 * restrike so both quantize identically. */
static inline uint8_t midi_tap_velocity(const MidiTapConfig *cfg, float sc,
                                        float thr_on, float span)
{
    if (cfg->vel_curve == MIDI_TAP_VEL_FIXED)
        return (uint8_t) midi_tap_clampi(cfg->vel_fixed, 1, 127);
    float x = (sc - thr_on) / span;
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (cfg->vel_curve == MIDI_TAP_VEL_SOFT) x = sqrtf(x);
    return (uint8_t) midi_tap_clampi(1 + (int) (126.0f * x), 1, 127);
}

/* ==========================================================================
 * Per-line extraction
 * ========================================================================== */
void midi_tap_process_line(MidiTapState *state,
                           const uint8_t *in_r, const uint8_t *in_g,
                           const uint8_t *in_b, int pixel_count,
                           int luxstral_num_octaves)
{
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;

    const uint64_t now_us = midi_tap_now_us();

    /* Disabled: release anything held ONCE (lazy re-arm), then stay O(1). */
    if (!state->config.enabled)
    {
        if (state->tap_active)
        {
            midi_tap_release_all(state, now_us);
            midi_tap_reset(state);
        }
        return;
    }
    state->tap_active = 1;

    int px = pixel_count;
    if (px > MIDI_TAP_MAX_PIXELS) px = MIDI_TAP_MAX_PIXELS;

    /* ── Line-rate EMA + derived counts ───────────────────────────────────── */
    if (state->last_line_us != 0)
    {
        const uint64_t dt = now_us - state->last_line_us;
        if (dt > 0 && dt < 1000000ull)
        {
            state->lps += (1.0e6f / (float) dt - state->lps) * (1.0f / 64.0f);
            /* Token bucket for the runaway limiter (note-ONs only). */
            const float cap = (state->config.max_events_per_s > 0.0f)
                            ? state->config.max_events_per_s : 200.0f;
            state->budget += cap * (float) dt * 1e-6f;
            if (state->budget > cap * 0.25f) state->budget = cap * 0.25f;
        }
    }
    else
    {
        state->budget = 1.0f;   /* let the very first line emit */
    }
    state->last_line_us = now_us;

    /* Rebuild derived counts on a >5 % rate move or after a config edit. */
    if (state->lps_derived <= 0.0f
        || fabsf(state->lps - state->lps_derived) > state->lps_derived * 0.05f)
        midi_tap_derive_counts(state);

    /* ── Geometry LUT (dirty-tracked) ─────────────────────────────────────── */
    const MidiTapConfig *cfg = &state->config;
    const float low_hz = (cfg->axis_low_hz > 0.0f) ? cfg->axis_low_hz : 65.406f;
    const int   oct    = (luxstral_num_octaves > 0) ? luxstral_num_octaves : 8;
    if (!state->lut_valid
        || state->lut_px        != px
        || state->lut_octaves   != oct
        || state->lut_transpose != cfg->transpose
        || state->lut_note_lo   != cfg->note_lo
        || state->lut_note_hi   != cfg->note_hi
        || state->lut_low_hz    != low_hz)
        midi_tap_build_lut(state, px, oct);

    if (state->scan_hi < state->scan_lo)
        return;   /* nothing in range (e.g. transpose pushed the axis out) */

    /* ── Background polarity + floor ──────────────────────────────────────── */
    float floor_e = 0.0f;
    const int bg_white = midi_tap_resolve_bg(state, in_r, in_g, in_b, px, &floor_e);
    const int32_t fl = (int32_t) floor_e;

    /* ── Band accumulation — the only O(pixel_count) pass ─────────────────── */
    const int src   = cfg->source;
    const int lo    = state->scan_lo;
    const int hi    = state->scan_hi;
    const float sm  = (cfg->smooth > 0.0f && cfg->smooth <= 1.0f) ? cfg->smooth : 0.5f;
    float emax = 0.0f;

    for (int n = lo; n <= hi; ++n)
    {
        const int i0 = (int) state->band_start_px[n];
        const int i1 = (int) state->band_start_px[n + 1];
        const int w  = i1 - i0;
        if (w <= 0) { state->bands[n].e = 0.0f; continue; }

        int32_t s = 0;
        for (int i = i0; i < i1; ++i)
        {
            const int32_t pm = (src == MIDI_TAP_SRC_R) ? (int32_t) in_r[i]
                             : (src == MIDI_TAP_SRC_G) ? (int32_t) in_g[i]
                             : (src == MIDI_TAP_SRC_B) ? (int32_t) in_b[i]
                             : ((int32_t) in_r[i] + in_g[i] + in_b[i]) / 3;
            const int32_t m = (bg_white ? (255 - pm) : pm) - fl;
            if (m > 0) s += m;
        }
        /* Normalised by the band's OWN width: the axis edges are HALF bands. */
        const float dens = (float) s / (float) w;

        MidiTapBand *b = &state->bands[n];
        b->e += (dens - b->e) * sm;
    }

    /* ── Decision variable ────────────────────────────────────────────────── */
    if (cfg->mode == MIDI_TAP_MODE_FUNDAMENTAL)
        midi_tap_build_hps(state, lo, hi);
    else
        for (int n = lo; n <= hi; ++n)
            state->score[n] = state->bands[n].e;

    for (int n = lo; n <= hi; ++n)
        if (state->score[n] > emax) emax = state->score[n];

    /* ── Gates ────────────────────────────────────────────────────────────── */
    const float thr_on  = cfg->thresh;
    const float thr_off = thr_on * ((cfg->hyst > 0.0f) ? cfg->hyst : 0.6f);
    const float rel     = (cfg->rel >= 0.0f) ? cfg->rel : 0.0f;
    /* The relative term is what keeps a DENSE image musical without per-image
     * tuning: on a line whose peak is 200, nothing below rel*200 can fire. */
    const float gate_on = (thr_on > rel * emax) ? thr_on : rel * emax;

    /* ── Top-N selection (bounded insertion, no sort, no alloc) ───────────── */
    const int N = midi_tap_clampi(cfg->max_poly, 1, MIDI_TAP_MAX_POLY);
    int   top_n[MIDI_TAP_MAX_POLY];
    float top_e[MIDI_TAP_MAX_POLY];
    int   ntop = 0;
    memset(state->is_top, 0, sizeof(state->is_top));

    for (int n = lo; n <= hi; ++n)
    {
        const float e = state->score[n];
        if (e < gate_on) continue;

        if (cfg->peak_only)
        {
            /* Asymmetric comparison so a flat plateau elects exactly ONE band:
             * a 3-semitone-wide blob must emit one note, not a cluster. */
            const float l = (n > lo) ? state->score[n - 1] : -1.0f;
            const float r = (n < hi) ? state->score[n + 1] : -1.0f;
            if (!(e >= l && e > r)) continue;
        }

        if (ntop < N)
        {
            top_n[ntop] = n; top_e[ntop] = e; ++ntop;
        }
        else
        {
            int mi = 0;
            for (int k = 1; k < N; ++k) if (top_e[k] < top_e[mi]) mi = k;
            if (e > top_e[mi]) { top_n[mi] = n; top_e[mi] = e; }
        }
    }
    for (int k = 0; k < ntop; ++k)
        state->is_top[top_n[k]] = 1;

    /* ── Note machine ─────────────────────────────────────────────────────── */
    const float span = (cfg->vel_span > 1.0f) ? cfg->vel_span : 1.0f;
    /* DENSE collapses the transcription delays: attack would postpone every
     * envelope step by a full arm window and min_on would glue the staircase
     * shut. release / max_on keep their meaning (tail gate, runaway ceiling). */
    const int dense     = cfg->dense;
    const int atk_lines = dense ? 1 : state->attack_lines;
    const int mon_lines = dense ? 0 : state->min_on_lines;
    const int rdelta    = (cfg->retrig_delta > 0) ? cfg->retrig_delta : 1;
    int nev = 0;

    for (int n = lo; n <= hi; ++n)
    {
        MidiTapBand *b = &state->bands[n];
        const int   sel = state->is_top[n];
        const float sc  = state->score[n];

        if (b->held)
        {
            const int hold_ok = sel && (sc >= thr_off);
            b->off_lines = hold_ok ? 0 : (uint16_t) (b->off_lines + 1);
            if (b->on_lines   < 0xFFFFu) b->on_lines++;
            if (b->trig_lines < 0xFFFFu) b->trig_lines++;

            const int expired = (state->max_on_lines > 0
                                 && b->on_lines >= (uint16_t) state->max_on_lines);
            /* min_on_lines is a SUSTAIN FLOOR: it delays the OFF, it never
             * retracts the ON (see midi_tap.h). max_on_ms always wins, so a
             * frozen image cannot hold a note forever.
             * NOTE-OFFS ARE NEVER RATE-LIMITED — starving a release is how you
             * ship stuck notes. */
            if ((b->off_lines >= (uint16_t) state->release_lines
                 && b->on_lines >= (uint16_t) mon_lines)
                || expired)
            {
                midi_tap_push(state, now_us, 0x80, (uint8_t) n, 0);
                b->held = 0; b->cand_lines = 0; b->off_lines = 0; b->on_lines = 0;
                ++nev;
            }
            else if (dense && hold_ok
                     && b->trig_lines >= (uint16_t) state->retrig_lines)
            {
                /* DENSE restrike: the velocity kept tracking the band and has
                 * moved by rdelta — re-strike so the file carries the envelope.
                 * Off+on share one stamp (one line = simultaneous by
                 * construction). NOT budgeted: a restrike opens no new voice
                 * and retrig_lines bounds its per-band rate. */
                const uint8_t v  = midi_tap_velocity(cfg, sc, thr_on, span);
                const int     dv = (int) v - (int) b->vel;
                if (dv >= rdelta || -dv >= rdelta)
                {
                    midi_tap_push(state, now_us, 0x80, (uint8_t) n, 0);
                    midi_tap_push(state, now_us, 0x90, (uint8_t) n, v);
                    b->vel = v; b->trig_lines = 0;
                    ++nev;
                }
            }
        }
        else
        {
            b->cand_lines = sel ? (uint16_t) (b->cand_lines + 1) : 0;
            if (b->cand_lines >= (uint16_t) atk_lines)
            {
                if (state->budget < 1.0f)
                {
                    /* Suppressed by the limiter. `held` MUST stay 0 or we would
                     * emit an orphan note-off later. */
                    atomic_fetch_add_explicit(&state->limited, 1ull, memory_order_relaxed);
                }
                else
                {
                    b->vel = midi_tap_velocity(cfg, sc, thr_on, span);

                    midi_tap_push(state, now_us, 0x90, (uint8_t) n, b->vel);
                    b->held = 1; b->on_lines = 0; b->off_lines = 0; b->trig_lines = 0;
                    state->budget -= 1.0f;
                    ++nev;
                }
            }
        }
    }

    if (nev)
    {
        uint32_t held = 0;
        for (int n = lo; n <= hi; ++n) held += state->bands[n].held ? 1u : 0u;
        atomic_store_explicit(&state->voice_count, held, memory_order_relaxed);
        atomic_fetch_add_explicit(&state->active_ticks, 1u, memory_order_relaxed);
    }
}
