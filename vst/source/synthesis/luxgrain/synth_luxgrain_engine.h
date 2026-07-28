/*
 * synth_luxgrain_engine.h
 *
 * LuxGrain — stochastic granular engine driven by the image-line stream.
 *
 * Paradigm shift vs the other engines: a pixel's brightness is not an
 * amplitude, it is an EMISSION DENSITY. The conditioned line coming from a
 * "→ LUXGRAIN" OUT module is folded into log-frequency BANDS (constant
 * pixel-per-semitone axis, pixel 0 = axis_low_hz — same coupling as
 * LuxStral/LuxHarmo), and each band drives an independent Poisson process:
 * the brighter the band, the more grains per second it fires (Xenakis
 * "screens" transposed to the CIS stream).
 *
 * Each grain is a windowed sinusoid:
 *   frequency  = band energy centroid (± intra-band spread ± jitter)
 *   duration   = long on smooth material, short on textured material
 *                (intra-band contrast → duration law)
 *   envelope   = Hann / Tukey / expodec / rexpodec
 *   pan        = per-grain constant-power random, scaled by stereo width
 *
 * SPREAD: the engine keeps a ring of the last N pushed lines. Every grain
 * draws its source cell from a random depth within the spread window, so a
 * wide spread keeps a whole slice of recent image alive — the texture
 * survives a stopped scroll instead of collapsing to the last column.
 *
 * Determinism: one xorshift stream, re-mixed with frame_seq at every line
 * latch — same feed sequence + same seed = bit-identical output.
 *
 * RT-safety: Pure C, allocation-free, bounded O(bands + active grains).
 *            Line pushes and config updates are staged under seqlocks and
 *            latched at BLOCK START only (house pattern — a push must never
 *            step the output mid-block). No JUCE deps, no mutex, no logging.
 *
 * Author: zhonx
 * Created: 2026-07-19
 */

#ifndef SYNTH_LUXGRAIN_ENGINE_H
#define SYNTH_LUXGRAIN_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity matches the other line consumers (>6912 for 400 DPI CIS). */
#define LUXGRAIN_MAX_PIXELS       8192
/* Log-frequency bands the line is folded into (config clamps to this). */
#define LUXGRAIN_MAX_BANDS        192
/* History ring depth in lines (~2 s at the feed's ~250 Hz push rate). */
#define LUXGRAIN_MAX_SPREAD       512
/* Simultaneous grain pool — overflow steals the grain closest to its end. */
#define LUXGRAIN_MAX_GRAINS       768
/* Upper bound on a single process() call. */
#define LUXGRAIN_MAX_BUFFER_SIZE  4096

/* Grain envelope shapes. */
#define LUXGRAIN_ENV_HANN      0   /* symmetric, silky                     */
#define LUXGRAIN_ENV_TUKEY     1   /* flat sustain, cosine edges           */
#define LUXGRAIN_ENV_EXPODEC   2   /* percussive: instant attack, exp tail */
#define LUXGRAIN_ENV_REXPODEC  3   /* reversed: exp swell, quick release   */
#define LUXGRAIN_NUM_ENVS      4

/* Grain material — what a grain plays. The scheduler is material-agnostic:
 * the image stays the sole PILOT (when / pitch / density / duration). */
#define LUXGRAIN_MAT_SINE      0   /* windowed sinusoid (internal LUT)     */
#define LUXGRAIN_MAT_SAMPLE    1   /* user WAV, transposed by root_hz      */

/* Sample material capacity per bank (10 s at 48 kHz). Two banks: a reload
 * fills the inactive one and publishes atomically, so grains still reading
 * the old bank finish their (≤ dur_max) tails untouched. Only a THIRD load
 * inside one tail window could collide — user loads are seconds apart. */
#define LUXGRAIN_SAMPLE_MAX    480000

/* ============================================================================
 * LuxGrainConfig — parameters synced from APVTS (staged, latched per block).
 * ========================================================================== */
typedef struct {
    int   enabled;

    /* Cloud statistics */
    float density_hz;       /* grains/s per band at full brightness (0.1..50) */
    float density_shape;    /* exponent on band value → emission rate (0.25..4) */
    float spread_lines;     /* history window grains draw from, 1..512 lines  */

    /* Grain morphology */
    float dur_min_ms;       /* shortest grain (2..100 ms)                     */
    float dur_max_ms;       /* longest grain (20..2000 ms)                    */
    float contrast_amount;  /* 0..1 — how much texture shortens grains        */
    int   env_shape;        /* LUXGRAIN_ENV_*                                 */
    float pitch_jitter_st;  /* per-grain random detune, semitones (0..2)      */
    float stereo_width;     /* 0..1 per-grain random pan span                 */
    float amp_follow;       /* 0 = flat grain amp, 1 = amp tracks band value  */
    float color_pan;        /* 0..1 weight of the cell's colour pan (R=L/B=R) */
    float edge_amount;      /* 0..1 luminance-rise → emission burst gain      */

    /* Material */
    int   material;         /* LUXGRAIN_MAT_* (SAMPLE falls back to SINE when
                             * no sample is published)                        */
    float scrub;            /* 0..1 read position in the sample material      */

    /* Master */
    float master_volume;
    uint32_t seed;          /* base RNG seed (frame_seq is mixed on top)      */

    /* Axis geometry — from g_sp3ctra_config (pixel 0 = axis_low_hz). */
    float axis_low_hz;      /* <= 0 falls back to C2 65.406 Hz                */
    int   num_octaves;      /* full-line span in octaves                      */
    int   num_bands;        /* line folding resolution (16..LUXGRAIN_MAX_BANDS) */
} LuxGrainConfig;

/* ============================================================================
 * Internal cells and voices (in-header for static allocation, house style).
 * ========================================================================== */

/* One log-frequency band of one pushed line — the statistics a grain needs,
 * so the full pixel history never has to be stored. */
typedef struct {
    float value;      /* mean energy of the band's pixels [0,1]              */
    float contrast;   /* intra-band texture (2×std, clamped to [0,1])        */
    float centroid;   /* energy-weighted pixel index (absolute, fractional)  */
    float spread_px;  /* energy-weighted std of the pixel index              */
    float pan;        /* colour temperature, red = −1 (L) … blue = +1 (R);
                       * 0 when the fold received no RGB                     */
    float edge;       /* luminance RISE vs the previous latched line [0,1]
                       * (consumer-computed at latch — contours = attacks)   */
} LuxGrainBandCell;

typedef struct {
    uint32_t delay;        /* samples until onset (scheduled within a block) */
    uint32_t remaining;    /* body samples left                              */
    uint32_t body_len;     /* total body samples                             */
    float    phase;        /* sine phase [0,1)                               */
    float    phase_inc;    /* per-sample phase increment                     */
    float    env_pos;      /* normalized envelope position [0,1]             */
    float    env_inc;      /* per-sample envelope increment                  */
    float    exp_state;    /* expodec/rexpodec multiplicative state          */
    float    exp_coef;     /* per-sample multiplier                          */
    float    amp_l, amp_r; /* constant-power pan gains × grain amplitude     */
    /* SAMPLE material (snapshot at spawn — survives a bank republish) */
    const float *smp;      /* bank data, NULL = sine                         */
    float    smp_pos;      /* fractional read position                       */
    float    smp_inc;      /* per-sample read increment (transposition)      */
    int      smp_len;
    uint8_t  env_shape;
    uint8_t  active;
} LuxGrainVoice;

/* ============================================================================
 * Engine state (preallocated, single global instance owned by the adapter).
 * ========================================================================== */
typedef struct {
    /* Config — RENDER copy (audio thread) + staged copy (UI thread). */
    LuxGrainConfig    config;
    LuxGrainConfig    config_pending;
    volatile uint32_t cfg_pending_seq;   /* seqlock: odd = writer inside */
    uint32_t          cfg_applied_seq;

    /* Line staging — producer folds the conditioned line into band cells
     * under the seqlock; process() latches at block start into the ring.
     * (Two pushes inside one block keep only the last — harmless for a
     * stochastic cloud, the ring advances once per latch.) */
    LuxGrainBandCell  pending_cells[LUXGRAIN_MAX_BANDS];
    int               pending_bands;
    int               pending_clear;        /* 1 = no-signal: wipe the ring  */
    int               axis_pixels_pending;  /* line width of the staged fold */
    uint32_t          pending_frame_seq;
    volatile uint32_t line_pending_seq;
    uint32_t          line_applied_seq;

    /* History ring of latched lines (audio thread only). */
    LuxGrainBandCell  ring[LUXGRAIN_MAX_SPREAD][LUXGRAIN_MAX_BANDS];
    int               ring_write;    /* next slot to write                  */
    int               ring_count;    /* valid lines in the ring             */

    /* Per-band fractional Poisson state is folded into the RNG draws; the
     * only per-band scheduler state is the band's spawn RNG stream. */
    uint32_t          rng;           /* xorshift32 state                    */

    /* Grain pool. */
    LuxGrainVoice     grains[LUXGRAIN_MAX_GRAINS];
    int               active_grains;

    /* Axis cache (rebuilt when config geometry changes). */
    float             band_freq[LUXGRAIN_MAX_BANDS];  /* centre Hz          */
    float             px_to_oct;     /* octaves per pixel                   */
    int               axis_pixels;   /* line width the cache was built for  */

    /* SAMPLE material — two banks, atomic publish (message-thread writer).
     * sample_active: -1 = none, else the published bank index. */
    float             sample_data[2][LUXGRAIN_SAMPLE_MAX];
    int               sample_len[2];
    float             sample_root_hz[2];
    float             sample_srate[2];    /* source file sample rate         */
    float             sample_gain[2];     /* RMS-normalisation to sine level */
    volatile int      sample_active;

    float             sample_rate;
    float             inv_sample_rate;
    int               initialized;
} LuxGrainEngine;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/* One-time init (idempotent). Builds the sine LUT, zeroes the state. */
int  luxgrain_engine_init(LuxGrainEngine *engine, float sample_rate);

/* All grains off, ring cleared, RNG re-seeded from config.seed. */
void luxgrain_engine_reset(LuxGrainEngine *engine);

LuxGrainConfig luxgrain_config_default(void);

/* UI/message thread: stage a new config (latched at next block start). */
/* Re-derive sample-rate-dependent state after a host sample-rate change
 * (grain pitch, durations, sample playback ratio). Call from prepareToPlay. */
void luxgrain_engine_set_sample_rate(LuxGrainEngine *engine, float sample_rate);

void luxgrain_engine_set_config(LuxGrainEngine *engine,
                                const LuxGrainConfig *config);

/* Producer thread: stage one CONDITIONED grayscale line [0,1] plus the raw
 * RGB stream at the OUT position (colour → per-cell pan; r/g/b may be NULL
 * → pan 0). The line is folded into band cells here (producer pays the
 * O(pixels) cost, the audio thread only copies cells). frame_seq keeps the
 * cloud deterministic. */
void luxgrain_engine_stage_line(LuxGrainEngine *engine,
                                const float *line,
                                const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels,
                                uint32_t frame_seq);

/* ── SAMPLE material (message thread only) ─────────────────────────────────
 * Publish a mono buffer as grain material. root_hz <= 0 runs the built-in
 * NSDF fundamental detector (loudest window). The buffer is resampled by
 * the grains at read time (smp_inc = f_target/root × src_sr/engine_sr).
 * Returns 0 on success, -1 on failure (too short / no stable pitch). */
int  luxgrain_engine_set_sample(LuxGrainEngine *engine,
                                const float *mono, int num_samples,
                                float sample_rate, float root_hz);
void luxgrain_engine_clear_sample(LuxGrainEngine *engine);
/* 1 + root written when a sample is published; 0 otherwise. */
int  luxgrain_engine_sample_info(const LuxGrainEngine *engine,
                                 float *root_hz_out, float *duration_s_out);

/* No-signal contract (chain STOP / send removed): stage a HISTORY WIPE —
 * the ring empties at the next latch, so no new grains spawn (active tails
 * finish, ≤ dur_max). Distinct from HOLD, where the producer simply stops
 * re-staging and the spread window legitimately keeps playing the past. */
void luxgrain_engine_stage_silence(LuxGrainEngine *engine);

/* Audio thread: render one block (adds nothing outside [0..num_samples)).
 * Latches pending config/line, runs the scheduler, renders the grains. */
void luxgrain_engine_process(LuxGrainEngine *engine,
                             float *out_l, float *out_r, int num_samples);

/* Diagnostics (message-thread reads, monotonic). */
int  luxgrain_engine_active_grains(const LuxGrainEngine *engine);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_LUXGRAIN_ENGINE_H */
