/*
 * luxstral_wavetable.c
 *
 * User-sample timbre wavetable — retained sample, scannable extraction point,
 * per-position normalization, lock-free round-robin publish.
 * See luxstral_wavetable.h for the design contract.
 *
 * Author: zhonx
 */

#include "luxstral_wavetable.h"
#include "logger.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WT_PI 3.14159265358979323846

/* Analysis window for pitch detection */
#define WT_ANALYSIS_WIN   4096
/* Uniform grid one extracted cycle is resampled to before projection */
#define WT_CYCLE_GRID     4096
/* Cycles averaged around the extraction point (noise / vibrato smoothing) */
#define WT_AVG_CYCLES     8
/* Pitch search range */
#define WT_F0_MIN_HZ      27.5f
#define WT_F0_MAX_HZ      2000.0f
/* NSDF peak below this = no usable periodicity */
#define WT_MIN_CONFIDENCE 0.30f
/* Extraction window below this RMS = silence, keep the previous timbre */
#define WT_SILENCE_RMS    1e-4

/*──────────────────────────────────────────────────────────────────────────────
 * State — publish slots (RT-read) + retained source sample (message thread)
 *──────────────────────────────────────────────────────────────────────────────*/
static luxstral_wavetable_t g_slots[LUXSTRAL_WT_SLOTS];
static int g_build_slot = 0; /* next slot to build into (message thread only) */
static luxstral_wavetable_t *_Atomic g_active = NULL;

static float *g_sample     = NULL; /* retained mono source (message thread)   */
static int    g_sample_len = 0;
static float  g_sample_fs  = 0.0f;
static double g_period     = 0.0;  /* fixed while scanning (samples)          */
static float  g_root_hz    = 0.0f;
static float  g_confidence = 0.0f;
static float  g_position   = 0.5f; /* scan position 0..1                      */
static char   g_name[LUXSTRAL_WT_NAME_MAX] = {0};
static float  g_overview[LUXSTRAL_WT_OVERVIEW_PAIRS * 2];

/*──────────────────────────────────────────────────────────────────────────────
 * Analysis helpers (all non-RT)
 *──────────────────────────────────────────────────────────────────────────────*/

/* Start of the strongest win-sample window (RMS) — pitch detection anchor,
 * and the fallback extraction point when the scan position sits on silence. */
static int find_loudest_window(const float *x, int n, int win)
{
    if (n <= win)
        return 0;
    const int hop = win / 2;
    double best_e = -1.0;
    int    best_i = 0;
    for (int i = 0; i + win <= n; i += hop) {
        double e = 0.0;
        for (int j = 0; j < win; j++)
            e += (double)x[i + j] * (double)x[i + j];
        if (e > best_e) {
            best_e = e;
            best_i = i;
        }
    }
    return best_i;
}

/* McLeod-style NSDF over one window: nsdf[tau] = 2·Σx[i]x[i+tau] /
 * Σ(x[i]²+x[i+tau]²). Picks the first local max ≥ 0.85 × the global max
 * (avoids octave-down errors), parabolic sub-sample refinement.
 * Returns the period in samples, or 0 on failure.                            */
static float detect_period_nsdf(const float *x, int n, float fs,
                                float *confidence_out)
{
    int tau_min = (int)(fs / WT_F0_MAX_HZ);
    int tau_max = (int)(fs / WT_F0_MIN_HZ);
    if (tau_min < 2)
        tau_min = 2;
    if (tau_max > n / 2)
        tau_max = n / 2;
    if (tau_max <= tau_min + 2)
        return 0.0f;

    const int ntau = tau_max - tau_min + 1;
    float *nsdf = (float *)malloc((size_t)ntau * sizeof(float));
    if (nsdf == NULL)
        return 0.0f;

    for (int tau = tau_min; tau <= tau_max; tau++) {
        double acf = 0.0, m = 0.0;
        const int lim = n - tau;
        for (int i = 0; i < lim; i++) {
            const double a = x[i], b = x[i + tau];
            acf += a * b;
            m   += a * a + b * b;
        }
        nsdf[tau - tau_min] = (m > 1e-12) ? (float)(2.0 * acf / m) : 0.0f;
    }

    float global_max = 0.0f;
    for (int i = 0; i < ntau; i++)
        if (nsdf[i] > global_max)
            global_max = nsdf[i];

    float best_tau = 0.0f, best_val = 0.0f;
    if (global_max >= WT_MIN_CONFIDENCE) {
        const float thresh = 0.85f * global_max;
        for (int i = 1; i < ntau - 1; i++) {
            if (nsdf[i] >= thresh && nsdf[i] >= nsdf[i - 1] &&
                nsdf[i] >= nsdf[i + 1]) {
                /* Parabolic refinement around the discrete peak */
                const float y0 = nsdf[i - 1], y1 = nsdf[i], y2 = nsdf[i + 1];
                const float den = y0 - 2.0f * y1 + y2;
                float delta = 0.0f;
                if (fabsf(den) > 1e-12f) {
                    delta = 0.5f * (y0 - y2) / den;
                    if (delta > 0.5f)
                        delta = 0.5f;
                    if (delta < -0.5f)
                        delta = -0.5f;
                }
                best_tau = (float)(i + tau_min) + delta;
                best_val = y1;
                break; /* first acceptable peak = highest fundamental candidate */
            }
        }
    }

    free(nsdf);
    if (confidence_out != NULL)
        *confidence_out = best_val;
    return best_tau;
}

/* Advance to the first rising zero-crossing within one period of `start`, so
 * consecutive extractions align on the same waveform feature and scanning
 * doesn't jitter the phase of the published cycle.                           */
static int align_to_rising_zero(const float *x, int n, int start, int period)
{
    const int lim = start + period;
    for (int i = start + 1; i < lim && i < n - 1; i++)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
            return i;
    return start;
}

/* Resample [t0, t0+period) onto the uniform WT_CYCLE_GRID via linear
 * interpolation, accumulating into grid[] (caller averages).                 */
static void accumulate_cycle(const float *x, int n, double t0, double period,
                             double *grid)
{
    for (int g = 0; g < WT_CYCLE_GRID; g++) {
        const double t  = t0 + period * (double)g / (double)WT_CYCLE_GRID;
        const int    i0 = (int)t;
        if (i0 < 0 || i0 + 1 >= n)
            return; /* partial cycle at the very end — skip the remainder */
        const double frac = t - (double)i0;
        grid[g] += (double)x[i0] + frac * ((double)x[i0 + 1] - (double)x[i0]);
    }
}

/* Project the averaged cycle onto harmonics 1..num_harm and RMS-normalize the
 * coefficient set to the sine table's RMS (1/√2) — the per-position loudness
 * contract. Phasor-recurrence trig (no libm in the loop): the whole
 * projection runs in ~2 ms, fast enough for live scanning.
 * Returns 0, or -1 if the cycle is silent.                                   */
static int project_harmonics(const double *grid, int num_harm, float *re_out,
                             float *im_out)
{
    /* Remove DC first — harmonic 0 is deliberately dropped */
    double mean = 0.0;
    for (int g = 0; g < WT_CYCLE_GRID; g++)
        mean += grid[g];
    mean /= (double)WT_CYCLE_GRID;

    double rms_sq = 0.0;
    for (int k = 1; k <= num_harm; k++) {
        const double w  = 2.0 * WT_PI * (double)k / (double)WT_CYCLE_GRID;
        const double cs = cos(w), sn = sin(w);
        double cg = 1.0, sg = 0.0; /* cos(w·g), sin(w·g), g = 0 */
        double a = 0.0, b = 0.0;
        for (int g = 0; g < WT_CYCLE_GRID; g++) {
            const double v = grid[g] - mean;
            a += v * cg;
            b += v * sg;
            const double cn = cg * cs - sg * sn;
            sg = sg * cs + cg * sn;
            cg = cn;
        }
        a *= 2.0 / (double)WT_CYCLE_GRID;
        b *= 2.0 / (double)WT_CYCLE_GRID;
        re_out[k - 1] = (float)a;
        im_out[k - 1] = (float)b;
        rms_sq += 0.5 * (a * a + b * b);
    }

    if (rms_sq < WT_SILENCE_RMS * WT_SILENCE_RMS)
        return -1;

    const float scale = (float)(0.70710678118 / sqrt(rms_sq));
    for (int k = 0; k < num_harm; k++) {
        re_out[k] *= scale;
        im_out[k] *= scale;
    }
    return 0;
}

/* Resynthesize the mip tables from harmonic coefficients: build the richest
 * level incrementally, snapshotting at each power-of-two harmonic boundary
 * (levels share their low harmonics — one accumulation pass builds all 9).
 * Same phasor recurrence as the projection: no libm in the point loop.       */
static void build_mips(luxstral_wavetable_t *wt)
{
    static double acc[LUXSTRAL_WT_TABLE_SIZE]; /* message-thread only */
    memset(acc, 0, sizeof(acc));
    memset(wt->mips, 0, sizeof(wt->mips));

    int level = LUXSTRAL_WT_LEVELS - 1; /* next boundary: 1, 2, 4, … harmonics */
    int boundary = LUXSTRAL_WT_MAX_HARMONICS >> level;

    for (int k = 1; k <= wt->num_harmonics; k++) {
        const double a = (double)wt->harm_re[k - 1];
        const double b = (double)wt->harm_im[k - 1];
        if (a != 0.0 || b != 0.0) {
            const double w  = 2.0 * WT_PI * (double)k / (double)LUXSTRAL_WT_TABLE_SIZE;
            const double cs = cos(w), sn = sin(w);
            double cg = 1.0, sg = 0.0;
            for (int i = 0; i < LUXSTRAL_WT_TABLE_SIZE; i++) {
                acc[i] += a * cg + b * sg;
                const double cn = cg * cs - sg * sn;
                sg = sg * cs + cg * sn;
                cg = cn;
            }
        }
        while (level >= 0 && k == boundary) {
            for (int i = 0; i < LUXSTRAL_WT_TABLE_SIZE; i++)
                wt->mips[level][i] = (float)acc[i];
            level--;
            boundary = (level >= 0) ? (LUXSTRAL_WT_MAX_HARMONICS >> level) : -1;
        }
    }
    /* Fewer harmonics than a level's boundary ⇒ that level is just the full
     * set — fill every remaining (richer) level with the final accumulation. */
    for (; level >= 0; level--)
        for (int i = 0; i < LUXSTRAL_WT_TABLE_SIZE; i++)
            wt->mips[level][i] = (float)acc[i];
}

/* Spectral envelope at `start` — constant-Q magnitude probes (Q≈4) on the
 * fixed log axis, ±2-point smoothing, max-normalized to 1 with a −60 dB
 * floor. Wide probes measure the ENVELOPE (formants), not the harmonic comb.
 * Phasor-recurrence trig, ~1 ms for 96 points.                               */
static void compute_envelope(int start, float *env_out)
{
    double raw[LUXSTRAL_WT_ENV_POINTS];

    for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++) {
        const double f = (double)LUXSTRAL_WT_ENV_FMIN *
                         pow(2.0, (double)LUXSTRAL_WT_ENV_LOG_SPAN *
                                      (double)p /
                                      (double)(LUXSTRAL_WT_ENV_POINTS - 1));
        /* Q≈4 → window = 4 periods of the probe, clamped to [256, 4096]     */
        int win = (int)(4.0 * (double)g_sample_fs / f);
        if (win > 4096) win = 4096;
        if (win < 256)  win = 256;
        int s0 = start;
        if (s0 + win > g_sample_len)
            s0 = g_sample_len - win;
        if (s0 < 0) { s0 = 0; win = g_sample_len; }

        const double w  = 2.0 * WT_PI * f / (double)g_sample_fs;
        const double cs = cos(w), sn = sin(w);
        double cg = 1.0, sg = 0.0, a = 0.0, b = 0.0, wsum = 0.0;
        for (int i = 0; i < win; i++) {
            /* Hann — cheap recurrence-free form via cosine of the SAME
             * phasor would couple the windows; a simple triangular window
             * is plenty for an envelope estimate.                           */
            const double h = 1.0 - fabs(2.0 * (double)i / (double)win - 1.0);
            const double v = (double)g_sample[s0 + i] * h;
            a += v * cg;
            b += v * sg;
            wsum += h;
            const double cn = cg * cs - sg * sn;
            sg = sg * cs + cg * sn;
            cg = cn;
        }
        raw[p] = (wsum > 1.0) ? sqrt(a * a + b * b) / wsum : 0.0;
    }

    /* ±2-point moving average — bridges the harmonic comb at low log-density */
    double sm[LUXSTRAL_WT_ENV_POINTS];
    for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++) {
        double s = 0.0;
        int    c = 0;
        for (int k = p - 2; k <= p + 2; k++)
            if (k >= 0 && k < LUXSTRAL_WT_ENV_POINTS) { s += raw[k]; c++; }
        sm[p] = s / (double)c;
    }

    double mx = 0.0;
    for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++)
        if (sm[p] > mx) mx = sm[p];
    if (mx < 1e-12) {
        for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++)
            env_out[p] = 1.0f; /* degenerate — flat, no filtering */
        return;
    }
    for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++) {
        float v = (float)(sm[p] / mx);
        if (v < LUXSTRAL_WT_ENV_FLOOR)
            v = LUXSTRAL_WT_ENV_FLOOR;
        env_out[p] = v;
    }
}

static void publish(luxstral_wavetable_t *wt)
{
    atomic_store_explicit(&g_active, wt, memory_order_release);
    g_build_slot = (g_build_slot + 1) % LUXSTRAL_WT_SLOTS;
}

static void rebuild_overview(void)
{
    const int pairs = LUXSTRAL_WT_OVERVIEW_PAIRS;
    for (int p = 0; p < pairs; p++) {
        const int i0 = (int)((long long)p * g_sample_len / pairs);
        int i1 = (int)((long long)(p + 1) * g_sample_len / pairs);
        if (i1 <= i0)
            i1 = i0 + 1;
        float mn = 0.0f, mx = 0.0f;
        for (int i = i0; i < i1 && i < g_sample_len; i++) {
            if (g_sample[i] < mn) mn = g_sample[i];
            if (g_sample[i] > mx) mx = g_sample[i];
        }
        g_overview[2 * p]     = mn;
        g_overview[2 * p + 1] = mx;
    }
}

/* Extract one averaged cycle at sample offset `start`, project, build, publish.
 * Returns 0 published, -1 window silent/degenerate (nothing published).      */
static int extract_and_publish_at(int start)
{
    const int win = (int)(g_period * (double)WT_AVG_CYCLES) + 2;
    if (start < 0)
        start = 0;
    if (start > g_sample_len - win - 1)
        start = g_sample_len - win - 1;
    if (start < 0)
        return -1;

    static double grid[WT_CYCLE_GRID]; /* message-thread only */
    memset(grid, 0, sizeof(grid));
    const int t0 =
        align_to_rising_zero(g_sample, g_sample_len, start, (int)g_period);
    int cycles = 0;
    for (int c = 0; c < WT_AVG_CYCLES; c++) {
        const double s = (double)t0 + g_period * (double)c;
        if (s + g_period + 1.0 >= (double)g_sample_len)
            break;
        accumulate_cycle(g_sample, g_sample_len, s, g_period, grid);
        cycles++;
    }
    if (cycles == 0)
        return -1;
    for (int g = 0; g < WT_CYCLE_GRID; g++)
        grid[g] /= (double)cycles;

    /* Harmonics above the source's own Nyquist don't exist — cap there.     */
    luxstral_wavetable_t *wt = &g_slots[g_build_slot];
    int num_harm = (int)(g_period * 0.5);
    if (num_harm > LUXSTRAL_WT_MAX_HARMONICS)
        num_harm = LUXSTRAL_WT_MAX_HARMONICS;
    if (num_harm < 1)
        num_harm = 1;

    memset(wt->harm_re, 0, sizeof(wt->harm_re));
    memset(wt->harm_im, 0, sizeof(wt->harm_im));
    if (project_harmonics(grid, num_harm, wt->harm_re, wt->harm_im) != 0)
        return -1; /* silence at this position — previous timbre holds */

    wt->num_harmonics = num_harm;
    wt->root_hz       = g_root_hz;
    wt->confidence    = g_confidence;
    snprintf(wt->source_name, sizeof(wt->source_name), "%s", g_name);

    compute_envelope(start, wt->env);

    build_mips(wt);
    publish(wt);
    return 0;
}

/*──────────────────────────────────────────────────────────────────────────────
 * Public API
 *──────────────────────────────────────────────────────────────────────────────*/

int luxstral_wavetable_load(const float *mono, int num_samples,
                            float sample_rate, float root_hz_override,
                            const char *source_name)
{
    if (mono == NULL || num_samples < 512 || sample_rate < 8000.0f) {
        log_error("WAVETABLE", "Load rejected: %d samples @ %.0f Hz",
                  num_samples, (double)sample_rate);
        return -1;
    }

    /* 1 — fundamental at the loudest window (stays fixed while scanning)    */
    const int win_start =
        find_loudest_window(mono, num_samples, WT_ANALYSIS_WIN);
    const int win_len = (num_samples - win_start < WT_ANALYSIS_WIN)
                            ? num_samples - win_start
                            : WT_ANALYSIS_WIN;

    float  confidence = 1.0f;
    double period;
    if (root_hz_override > 0.0f) {
        period = (double)sample_rate / (double)root_hz_override;
    } else {
        const float p = detect_period_nsdf(mono + win_start, win_len,
                                           sample_rate, &confidence);
        if (p <= 0.0f) {
            log_warning("WAVETABLE",
                        "No periodicity found (confidence %.2f) — load aborted",
                        (double)confidence);
            return -1;
        }
        period = (double)p;
    }

    /* 2 — retain the source (replaces any previous one)                      */
    float *copy = (float *)malloc((size_t)num_samples * sizeof(float));
    if (copy == NULL)
        return -1;
    memcpy(copy, mono, (size_t)num_samples * sizeof(float));
    free(g_sample);
    g_sample     = copy;
    g_sample_len = num_samples;
    g_sample_fs  = sample_rate;
    g_period     = period;
    g_root_hz    = (float)((double)sample_rate / period);
    g_confidence = confidence;
    snprintf(g_name, sizeof(g_name), "%s",
             (source_name != NULL) ? source_name : "(unnamed)");
    rebuild_overview();

    /* 3 — extract at the current scan position; a silent spot falls back to
     * the loudest window so a fresh load always makes sound.                */
    const int win = (int)(g_period * (double)WT_AVG_CYCLES) + 2;
    int start = (int)(g_position * (double)(g_sample_len - win));
    if (extract_and_publish_at(start) != 0) {
        if (extract_and_publish_at(win_start) != 0) {
            log_warning("WAVETABLE", "Sample is silent everywhere tried — "
                                     "load aborted");
            free(g_sample);
            g_sample = NULL;
            g_sample_len = 0;
            return -1;
        }
        log_info("WAVETABLE", "Scan position silent — extracted at loudest "
                              "window instead");
    }

    log_info("WAVETABLE",
             "Timbre loaded: '%s' — root %.1f Hz (confidence %.2f), "
             "%.1f s retained, scan position %.2f",
             g_name, (double)g_root_hz, (double)confidence,
             (double)num_samples / (double)sample_rate, (double)g_position);
    return 0;
}

int luxstral_wavetable_set_position(float pos01)
{
    if (pos01 < 0.0f) pos01 = 0.0f;
    if (pos01 > 1.0f) pos01 = 1.0f;
    g_position = pos01;
    if (g_sample == NULL || g_period <= 0.0)
        return -1;
    const int win = (int)(g_period * (double)WT_AVG_CYCLES) + 2;
    const int start = (int)((double)pos01 * (double)(g_sample_len - win));
    const int rc = extract_and_publish_at(start);
    if (rc == 0)
        log_info_every_ms(2000, "WAVETABLE",
                          "Scan %.2f (%.2f s) — timbre re-extracted",
                          (double)pos01,
                          (double)start / (double)g_sample_fs);
    return rc;
}

float luxstral_wavetable_get_duration_s(void)
{
    return (g_sample != NULL && g_sample_fs > 0.0f)
               ? (float)g_sample_len / g_sample_fs
               : 0.0f;
}

int luxstral_wavetable_get_env(float *env96)
{
    const luxstral_wavetable_t *wt =
        atomic_load_explicit(&g_active, memory_order_acquire);
    if (wt == NULL || env96 == NULL)
        return 0;
    memcpy(env96, wt->env, sizeof(wt->env));
    return 1;
}

float luxstral_wavetable_get_position(void)
{
    return g_position;
}

int luxstral_wavetable_has_sample(void)
{
    return g_sample != NULL;
}

int luxstral_wavetable_get_overview(float *minmax)
{
    if (g_sample == NULL || minmax == NULL)
        return 0;
    memcpy(minmax, g_overview, sizeof(g_overview));
    return 1;
}

int luxstral_wavetable_load_from_harmonics(const float *harm_re,
                                           const float *harm_im,
                                           int num_harmonics, float root_hz,
                                           const float *env96,
                                           const char *source_name)
{
    if (harm_re == NULL || harm_im == NULL || num_harmonics < 1 ||
        num_harmonics > LUXSTRAL_WT_MAX_HARMONICS)
        return -1;

    /* Static fallback: no retained source, scanning unavailable.            */
    free(g_sample);
    g_sample     = NULL;
    g_sample_len = 0;

    luxstral_wavetable_t *wt = &g_slots[g_build_slot];
    memset(wt->harm_re, 0, sizeof(wt->harm_re));
    memset(wt->harm_im, 0, sizeof(wt->harm_im));
    memcpy(wt->harm_re, harm_re, (size_t)num_harmonics * sizeof(float));
    memcpy(wt->harm_im, harm_im, (size_t)num_harmonics * sizeof(float));
    if (env96 != NULL)
        memcpy(wt->env, env96, sizeof(wt->env));
    else
        for (int p = 0; p < LUXSTRAL_WT_ENV_POINTS; p++)
            wt->env[p] = 1.0f;   /* flat — no formant filtering */
    wt->num_harmonics = num_harmonics;
    wt->root_hz       = root_hz;
    wt->confidence    = 1.0f;
    snprintf(wt->source_name, sizeof(wt->source_name), "%s",
             (source_name != NULL) ? source_name : "(restored)");
    snprintf(g_name, sizeof(g_name), "%s", wt->source_name);
    g_root_hz    = root_hz;
    g_confidence = 1.0f;

    build_mips(wt);
    publish(wt);

    log_info("WAVETABLE", "Timbre restored from session: '%s' — root %.1f Hz, "
                          "%d harmonics (static, source file not reloaded)",
             wt->source_name, (double)root_hz, num_harmonics);
    return 0;
}

void luxstral_wavetable_clear(void)
{
    free(g_sample);
    g_sample     = NULL;
    g_sample_len = 0;
    if (atomic_load_explicit(&g_active, memory_order_acquire) != NULL) {
        atomic_store_explicit(&g_active, NULL, memory_order_release);
        log_info("WAVETABLE", "Timbre cleared — bank back to sine/square");
    }
}

const luxstral_wavetable_t *luxstral_wavetable_acquire(void)
{
    return atomic_load_explicit(&g_active, memory_order_acquire);
}

int luxstral_wavetable_is_loaded(void)
{
    return atomic_load_explicit(&g_active, memory_order_acquire) != NULL;
}

int luxstral_wavetable_get_info(char *name_out, int name_cap,
                                float *root_hz_out, float *confidence_out)
{
    const luxstral_wavetable_t *wt =
        atomic_load_explicit(&g_active, memory_order_acquire);
    if (wt == NULL)
        return 0;
    if (name_out != NULL && name_cap > 0)
        snprintf(name_out, (size_t)name_cap, "%s", wt->source_name);
    if (root_hz_out != NULL)
        *root_hz_out = wt->root_hz;
    if (confidence_out != NULL)
        *confidence_out = wt->confidence;
    return 1;
}

int luxstral_wavetable_get_harmonics(float *harm_re_out, float *harm_im_out,
                                     int max_harmonics, float *root_hz_out)
{
    const luxstral_wavetable_t *wt =
        atomic_load_explicit(&g_active, memory_order_acquire);
    if (wt == NULL || harm_re_out == NULL || harm_im_out == NULL)
        return 0;
    int n = wt->num_harmonics;
    if (n > max_harmonics)
        n = max_harmonics;
    memcpy(harm_re_out, wt->harm_re, (size_t)n * sizeof(float));
    memcpy(harm_im_out, wt->harm_im, (size_t)n * sizeof(float));
    if (root_hz_out != NULL)
        *root_hz_out = wt->root_hz;
    return n;
}
