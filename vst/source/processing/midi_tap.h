/*
 * midi_tap.h — pass-through RT image PROBE that extracts MIDI notes.
 *
 * A MIDI TAP insert reads the RGB scanline AS IT FLOWS at its position in a
 * chain (IMAGE_CHAIN_INSERT_MIDITAP), converts it into note on/off events, and
 * forwards the image downstream UNCHANGED. The pixel axis IS the pitch axis
 * (pps = pixel_count / (octaves*12), pixel 0 at axis_low_hz — the same geometry
 * as LuxHarmo/LuxPitch), so one semitone band is ~36 px at the 3456 px default.
 *
 * Extraction (per line, O(pixel_count), allocation-free, bounded):
 *   1. polarity + background floor — AUTO learn-then-lock; floor = EMA of the
 *      per-line 10th-PERCENTILE energy (canonical grey-bands fix, see
 *      lux_drive.c: a mean-based floor over-estimates on dense lines and
 *      gates out real material; a gated sampled-minimum froze and seeded at
 *      0 on dense passages, counting the paper pedestal as material).
 *   2. per-semitone energy accumulation, normalised by the band's OWN pixel
 *      count — the axis edges are HALF bands and must stay comparable.
 *   3. absolute gate + gate relative to the line's own peak, hysteresis on
 *      release, optional band-space ridge test (one note per bright blob).
 *   4. top-N selection (N = max_poly) — bounded insertion, no sort, no alloc.
 *   5. per-band note machine: attack_ms to arm, min_on_ms sustain floor,
 *      release_ms + hysteresis to release, velocity from band energy.
 *   6. DENSE ("black MIDI") option: while a note is held its velocity keeps
 *      tracking the band energy, and the note is RESTRUCK (off+on, one line
 *      stamp) once it has moved by retrig_delta and retrig_ms has elapsed —
 *      the .mid then carries each partial's amplitude ENVELOPE, which is what
 *      keeps a voice intelligible on re-render (a latched velocity flattens
 *      it). Steady partials stay single held notes; attack/min_on collapse to
 *      0 while dense (they contradict per-frame fidelity); restrikes bypass
 *      the note-on budget (retrig_ms bounds their per-band rate).
 *
 * Threading:
 *   PRODUCER (single) : the chain's producer thread (udpThread / media feeder /
 *                       chain_player_execute_owned), midi_tap_process_line().
 *                       RT-safe: two bounded passes + one release store per
 *                       event. No alloc/lock/syscall/logging.
 *                       Owns write_index (release) AND generation.
 *                       On overrun it advances (newest wins), never blocks.
 *   CONSUMERS (N)     : the ring is a BROADCAST WINDOW, not an SPSC queue. The
 *                       producer never reads a consumer cursor, so any number
 *                       of sinks may drain it independently, each owning its
 *                       own cursor OUTSIDE this struct. midi_tap_ring_get()
 *                       copies then RE-VALIDATES (returns 0 → discard).
 *                       What is given up versus a real queue is back-pressure:
 *                       a stalled sink loses the OLDEST events instead of
 *                       blocking the producer. That is the intended trade.
 *   CONTROL           : `config` is written by the message thread (plain
 *                       stores) and read by the producer, like LuxHarmo.
 *
 * Timestamps are ABSOLUTE CLOCK_MONOTONIC microseconds, stamped ONCE PER LINE
 * (every note of one scanline is simultaneous by construction, so a per-event
 * stamp would be both slower and less correct). The master REC epoch
 * (g_midi_tap_transport.t0_us) is applied BY THE SINKS, never here — the RT
 * path is transport-blind, so arming REC can never glitch the extractor.
 *
 * The default axis (low 65.41 Hz, 8 octaves) spans MIDI 36..132: bands above
 * 127 have no MIDI number and are simply NOT SCANNED (truncated, never
 * octave-folded — folding would print a phantom bass line). cfg.transpose = -5
 * shifts the scan so the whole axis lands inside 31..127.
 *
 * Memory: per instance ~= 68 KB (4096-slot event ring + 129-entry band LUT +
 *         128 band states); pool (CHAIN_MAX_CHAINS = 8) ~= 545 KB BSS.
 *
 * RT-safety: pure C, allocation-free, lock-free, bounded O(pixel_count).
 *            No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 */
#ifndef MIDI_TAP_H
#define MIDI_TAP_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-instance pool size */

/* Atomic discipline mirrors video_scroll.h's VS_ATOMIC / lux_pitch.h's
 * LP_ATOMIC: on the C++ side the fields are plain `volatile T` (atomic for
 * int/uint32/uint64 on all targets, written as plain stores) and <atomic> is
 * pulled in only for std::memory_order names; on the C side they are real
 * _Atomic T with <stdatomic.h>. This lets the C++ sinks read the ring while
 * midi_tap.c (C11) uses atomic_*_explicit. */
#ifdef __cplusplus
  #include <atomic>
  #define MT_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define MT_ATOMIC(T) _Atomic T
#endif

/* Capacity matches LuxPitch/LuxMask/LuxEq/LuxHarmo (>6912 for 400 DPI CIS). */
#define MIDI_TAP_MAX_PIXELS   8192
#define MIDI_TAP_NUM_NOTES    128
#define MIDI_TAP_MAX_POLY     16
/* Power of two. Worst case 1000 lines/s x (8 offs + 8 ons) = 16 000 events/s;
 * 4096 slots = ~256 ms of headroom for a sink polling every 10 ms. */
#define MIDI_TAP_RING_SLOTS   4096
#define MIDI_TAP_RING_MASK    (MIDI_TAP_RING_SLOTS - 1)

/* Background mode — which pole is the "material" (mirrors LUX_HARMO_BG_*). */
#define MIDI_TAP_BG_BLACK  0    /* bright material on black background */
#define MIDI_TAP_BG_WHITE  1    /* dark material on white background   */
#define MIDI_TAP_BG_AUTO   2    /* detect from the stream (default)    */

/* Intensity source — which channel drives the band energy. */
#define MIDI_TAP_SRC_LUMA  0
#define MIDI_TAP_SRC_R     1
#define MIDI_TAP_SRC_G     2
#define MIDI_TAP_SRC_B     3

/* What the note decision is taken ON.
 *
 * BANDS       — the per-semitone energy itself. Every bright band may become a
 *               note. Right for printed scores, drawn strokes, sparse material.
 *
 * FUNDAMENTAL — a Harmonic Product Spectrum computed on the band array, so the
 *               decision variable is "how much does a HARMONIC STACK start
 *               here" instead of "how bright is this band". This is what a
 *               VOICE (or any harmonic instrument) needs: its loudest partial
 *               is almost never its fundamental, so BANDS mode transcribes the
 *               harmonic stack — octaves and fifths — instead of the melody.
 *
 *               The pixel axis is LOG-frequency with a constant pixels-per-
 *               semitone density, so harmonic k of f0 always sits exactly
 *               12*log2(k) semitones above it — FIXED offsets, independent of
 *               the note (see MIDI_TAP_HARMONICS). The HPS is therefore just a
 *               few shifted multiplies, not a search. */
#define MIDI_TAP_MODE_BANDS       0
#define MIDI_TAP_MODE_FUNDAMENTAL 1

/* Harmonic offsets in semitones: round(12*log2(k)) for k = 1,2,3,4.
 * k=3 is 19.02 and k=4 is exactly 24 — the rounding error at k=3 is 0.02
 * semitone, far below one band. */
#define MIDI_TAP_HARMONICS 4

/* Velocity shaping. */
#define MIDI_TAP_VEL_LINEAR 0
#define MIDI_TAP_VEL_SOFT   1   /* sqrt — lifts quiet material            */
#define MIDI_TAP_VEL_FIXED  2   /* ignore energy, always cfg.vel_fixed     */

/* Out-of-range policy for notes outside [0,127] after transpose. */
#define MIDI_TAP_RANGE_CLAMP 0
#define MIDI_TAP_RANGE_DROP  1

/* Lines the AUTO polarity learner observes before locking (lux_harmo: 96). */
#define MIDI_TAP_BG_LOCK_LINES 96

/* One extracted event. Exactly 16 B, no padding. */
typedef struct {
    uint64_t t_us;    /* absolute CLOCK_MONOTONIC us, stamped once per line */
    uint8_t  status;  /* 0x90 note-on / 0x80 note-off (the sinks add the channel) */
    uint8_t  note;    /* 0..127 */
    uint8_t  vel;     /* 1..127 on, 0 off */
    uint8_t  flags;   /* reserved (0) */
    uint32_t pad;     /* explicit — keeps sizeof == 16 on every target */
} MidiTapEvent;

/* ============================================================================
 * MidiTapConfig — parameters synced from APVTS (message thread → producer).
 * ============================================================================ */
typedef struct {
    int   enabled;
    int   source;            /* MIDI_TAP_SRC_*                                */
    int   mode;              /* MIDI_TAP_MODE_*                               */
    int   max_poly;          /* 1..MIDI_TAP_MAX_POLY, default 8               */
    float thresh;            /* 0..255 band-density gate, default 12          */
    float hyst;              /* 0.1..0.95 release ratio of thresh, default 0.6 */
    float rel;               /* 0..1 gate relative to the line peak, dflt 0.25 */
    float smooth;            /* 0.05..1 per-band EMA coefficient, default 0.5  */
    int   peak_only;         /* 1 = require a band-space ridge, default 1      */
    float attack_ms;         /* arm delay — THE reject-short-notes knob, 12    */
    float release_ms;        /* consecutive failing lines before note-off, 40  */
    float min_on_ms;         /* SUSTAIN FLOOR: delays the off, never the on, 60 */
    float max_on_ms;         /* forced note-off ceiling; 0 = none, default 8000 */
    float vel_span;          /* density above thresh mapping to velocity 127, 96 */
    int   vel_curve;         /* MIDI_TAP_VEL_*                                 */
    int   vel_fixed;         /* 1..127, used when vel_curve == FIXED           */
    int   note_lo, note_hi;  /* scan limits, default 0 / 127                   */
    int   transpose;         /* -48..48 semitones, applied at LUT BUILD time   */
    int   range_policy;      /* MIDI_TAP_RANGE_*                               */
    int   background_mode;   /* MIDI_TAP_BG_*                                  */
    float axis_low_hz;       /* g_sp3ctra_config.low_frequency; <=0 → 65.406   */
    float max_events_per_s;  /* runaway limiter, default 200 (offs exempt)     */
    /* DENSE ("black MIDI") — see step 6 of the header comment. */
    int   dense;             /* 0 = classic transcription (default)            */
    float retrig_ms;         /* min ms between restrikes of one band, dflt 10  */
    int   retrig_delta;      /* velocity move forcing a restrike, default 5    */
} MidiTapConfig;

/* Per-band note state. 16 B x 128 = 2 KB. */
typedef struct {
    float    e;            /* smoothed band density — THE decision variable */
    uint8_t  held;         /* 1 = note-on emitted, not yet released         */
    uint8_t  vel;          /* velocity latched at note-on                   */
    uint8_t  pad[2];
    uint16_t on_lines;     /* lines since note-on (saturating)              */
    uint16_t off_lines;    /* consecutive lines failing the hold test       */
    uint16_t cand_lines;   /* consecutive lines passing the gate while idle */
    uint16_t trig_lines;   /* lines since the last strike (dense retrig clock) */
} MidiTapBand;

/* ============================================================================
 * MidiTapState — complete runtime state (one pool instance).
 * ============================================================================ */
typedef struct MidiTapState {
    MidiTapConfig config;

    /* ── Event ring (broadcast window — see the header comment) ───────────── */
    MidiTapEvent        ring[MIDI_TAP_RING_SLOTS];
    MT_ATOMIC(uint32_t) write_index;   /* producer→consumers total pushes (release) */
    MT_ATOMIC(uint32_t) generation;    /* producer-only: bumped on discontinuity    */
    MT_ATOMIC(uint32_t) active_ticks;  /* rack-LED heartbeat: lines that emitted    */
    MT_ATOMIC(uint32_t) voice_count;   /* currently held notes (UI meter)           */
    MT_ATOMIC(uint64_t) limited;       /* note-ons suppressed by the rate limiter   */

    /* ── Band geometry LUT (rebuilt only when the dirty key changes) ──────── */
    /* band_start_px[n] = first pixel of emitted note n's Voronoi cell.
     * Monotone non-decreasing, so band n spans [start[n], start[n+1]). */
    int32_t band_start_px[MIDI_TAP_NUM_NOTES + 1];
    int     scan_lo, scan_hi;   /* first/last note with a non-empty band */
    int     lut_valid;
    /* Dirty key. */
    int     lut_px, lut_octaves, lut_transpose, lut_note_lo, lut_note_hi;
    float   lut_low_hz;

    /* ── Background polarity + floor (percentile paper level, see lux_drive) ─ */
    int   auto_bg_white;
    int   auto_locked;
    int   auto_lock_countdown;
    int   auto_max_mean;
    int   auto_min_mean;
    float floor_ema;            /* -1 = unseeded */
    int   last_bg_mode;         /* RESOLVED polarity the floor was learned in */

    /* ── Note machine ─────────────────────────────────────────────────────── */
    MidiTapBand bands[MIDI_TAP_NUM_NOTES];
    uint8_t     is_top[MIDI_TAP_NUM_NOTES];   /* per-line top-N membership */
    /* THE decision variable, in the same 0..255 density units in both modes so
     * thresh/rel keep one meaning: bands[n].e in BANDS mode, the HPS in
     * FUNDAMENTAL mode. Every gate, the ridge test, the top-N and the note
     * machine read THIS, never bands[].e directly. */
    float       score[MIDI_TAP_NUM_NOTES];

    /* ── Line-rate tracking (config is in ms, decisions are in lines) ─────── */
    uint64_t last_line_us;
    float    lps;               /* lines/s EMA, seeded at 250 */
    float    lps_derived;       /* lps the derived counts below were built with */
    int      attack_lines, release_lines, min_on_lines, max_on_lines, retrig_lines;

    /* ── Runaway limiter (token bucket, note-ONs only) ────────────────────── */
    float budget;

    int tap_active;   /* latch: emitted at least once since the last reset */
} MidiTapState;

/* ============================================================================
 * Master transport — ONE process-wide epoch shared by every instance, so the
 * .mid files of N probes land on a single timeline.
 * ========================================================================== */
typedef struct {
    MT_ATOMIC(uint64_t) t0_us;   /* monotonic us of the last REC arm; 0 = never */
    MT_ATOMIC(uint32_t) run_id;  /* bumped on every arm — sinks detect a new take */
    MT_ATOMIC(int)      armed;   /* 1 while REC runs */
} MidiTapTransport;

extern MidiTapTransport g_midi_tap_transport;

/* Message thread. arm() stores t0 then run_id then armed (release); readers
 * load armed (acquire) first — one release/acquire pair covers all three. */
uint64_t midi_tap_transport_arm(void);
void     midi_tap_transport_disarm(void);   /* t0 kept so sinks can finish draining */
uint64_t midi_tap_transport_t0_us(void);
int      midi_tap_transport_armed(void);
uint32_t midi_tap_transport_run_id(void);

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void          midi_tap_init(MidiTapState *state);   /* atomic_store, NOT atomic_init */
void          midi_tap_reset(MidiTapState *state);  /* bands/floor/AUTO/LUT re-armed */
MidiTapConfig midi_tap_config_default(void);

/* ── Producer (chain producer thread — RT-safe) ────────────────────────────── */
/* Extract one RGB line. The image is NOT modified: this is a probe, the caller
 * forwards its input pointers downstream unchanged. */
void midi_tap_process_line(MidiTapState *state,
                           const uint8_t *in_r, const uint8_t *in_g,
                           const uint8_t *in_b, int pixel_count,
                           int luxstral_num_octaves);

/* Producer-thread note kill: release every held band. Idempotent and cheap
 * (a 128-entry scan), so chain_publish_no_signal may call it every tick while
 * the chain stays de-fed. It calls this INSTEAD of VideoScroll's white-line
 * feed: under BG_BLACK polarity a white line is maximum material EVERYWHERE,
 * i.e. a 128-note chord on every chain teardown. */
void midi_tap_silence(MidiTapState *state);

/* Message-thread note kill, legal ONLY once the producer provably no longer
 * touches this slot (the >= 40 ms deferred pool reset). Pushes the note-offs
 * into the ring so the sinks can drain them BEFORE midi_tap_init() wipes it. */
void midi_tap_panic(MidiTapState *state);

/* ── Consumers (any number; each owns its own cursor) ──────────────────────── */
uint32_t midi_tap_ring_writepos(const MidiTapState *state);

/* Events available since `cursor`, clamped to RING_SLOTS. *out_dropped (may be
 * NULL) receives the number LOST to an overrun — a nonzero value obliges the
 * caller to release its own held[] shadow and re-anchor its cursor. */
uint32_t midi_tap_ring_available(const MidiTapState *state, uint32_t cursor,
                                 uint32_t *out_dropped);

/* Copy the event at ABSOLUTE index `idx`. Returns 1 on success, 0 if `idx` is
 * outside the live window. ON A 0 RETURN *out IS INDETERMINATE AND MUST BE
 * DISCARDED. Re-validates AFTER the copy to reject a tear. */
int      midi_tap_ring_get(const MidiTapState *state, uint32_t idx, MidiTapEvent *out);

/* Producer-owned. A change means DISCONTINUITY: the consumer must re-anchor its
 * cursor to writepos and release its held[] shadow. It never writes it. */
uint32_t midi_tap_generation(const MidiTapState *state);
uint32_t midi_tap_active_ticks(const MidiTapState *state);   /* rack LED */
uint32_t midi_tap_voice_count(const MidiTapState *state);    /* UI meter */
uint64_t midi_tap_limited_count(const MidiTapState *state);  /* limiter stat */

/* ── Per-instance pool (mirrors video_scroll) ──────────────────────────────── */
extern MidiTapState g_midi_tap_proc;
MidiTapState *midi_tap_instance(int idx);   /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void          midi_tap_init_all(void);

/* ── Shared clock (also used by the C++ sinks, so they agree with the stamps) */
uint64_t midi_tap_now_us(void);   /* CLOCK_MONOTONIC microseconds */

#ifdef __cplusplus
}
#endif

#endif /* MIDI_TAP_H */
