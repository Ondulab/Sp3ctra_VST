/*
 * luxsynth_feed.c — see luxsynth_feed.h for the contract.
 */
#include "luxsynth_feed.h"
#include "synth_staging.h"
#include "config/config_loader.h"
#include "config/config_instrument.h"
#include "../synthesis/luxsynth/luxsynth_vst_adapter.h"
#include "../synthesis/luxsynth/kissfft/kiss_fftr.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

#define LX_FEED_MAX_BINS (CIS_MAX_PIXELS_NB / 2 + 1)

/* All state below is touched by the audio thread only. */
static float           s_line[CIS_MAX_PIXELS_NB];
static uint8_t         s_r[CIS_MAX_PIXELS_NB];
static uint8_t         s_g[CIS_MAX_PIXELS_NB];
static uint8_t         s_b[CIS_MAX_PIXELS_NB];
static kiss_fft_scalar s_in[CIS_MAX_PIXELS_NB];
static kiss_fft_cpx    s_out[LX_FEED_MAX_BINS];
static float           s_mags[LX_FEED_MAX_BINS];
static float           s_smoothed[LX_FEED_MAX_BINS];
static float           s_harm[LX_FEED_MAX_BINS];
static int             s_harm_init = 0;
static kiss_fftr_cfg   s_cfg = NULL;
static int             s_cfg_n = 0;
static uint32_t        s_last_gen = 0;
static int             s_have_gen = 0;
static int             s_silenced = 0;
static uint64_t        s_last_push_us = 0;

/* Consecutive no-send ticks before the silence contract fires (~100 ms at
 * the 2-4 ms tick rate) — a shorter flicker holds the spectrum instead. */
#define LX_FEED_SILENCE_DEBOUNCE_TICKS 50
static int             s_zero_ticks = 0;

/* Dropout diagnostics — how often the feed pushed SILENCE vs real spectra.
 * Audio thread bumps, message thread drains (PluginProcessor timer). */
static volatile uint64_t s_diag_silence_pushes = 0;
static volatile uint64_t s_diag_spec_pushes   = 0;

uint64_t luxsynth_feed_silence_pushes(void)
{
    return __atomic_load_n(&s_diag_silence_pushes, __ATOMIC_RELAXED);
}

uint64_t luxsynth_feed_spec_pushes(void)
{
    return __atomic_load_n(&s_diag_spec_pushes, __ATOMIC_RELAXED);
}

static void lx_feed_push_silence(int nDisplay)
{
    if (s_silenced)
        return;
    __atomic_fetch_add(&s_diag_silence_pushes, 1, __ATOMIC_RELAXED);
    memset(s_smoothed, 0, sizeof(s_smoothed));
    luxsynth_engine_set_spectral_data(&g_luxsynth_engine,
                                      s_smoothed + 1, NULL, s_harm + 1,
                                      NULL, NULL, nDisplay);
    s_silenced = 1;
    s_have_gen = 0;   /* force a recompute when signal returns */
}

void luxsynth_feed_tick(const ChainPlan* plan)
{
    if (plan == NULL || !luxsynth_are_buffers_ready())
        return;

    const int N = get_cis_pixels_nb();
    if (N <= 0 || N > CIS_MAX_PIXELS_NB)
        return;

    if (!s_harm_init)
    {
        for (int k = 0; k < LX_FEED_MAX_BINS; ++k) s_harm[k] = 0.5f;
        s_harm_init = 1;
    }

    /* Bins choice: 0=32, 1=64, 2=128, 3=256 harmonics (== the UI FFT view). */
    static const int kBinsChoices[4] = { 32, 64, 128, 256 };
    int choice = g_sp3ctra_config.lx_fft_bins_choice;
    if (choice < 0) choice = 0;
    if (choice > 3) choice = 3;
    const int nBins    = N / 2 + 1;
    int       nDisplay = kBinsChoices[choice];
    if (nDisplay > nBins - 1) nDisplay = nBins - 1;
    if (nDisplay <= 0)
        return;

    int      nbp = 0;
    uint32_t gen = 0;
    const int mixed = synth_staging_mix_luxsynth(plan, s_line,
                                                 s_r, s_g, s_b,
                                                 N, &nbp, &gen);
    if (mixed < 0)
        return;   /* torn slot (producer mid-staging) — HOLD the engine's
                   * spectrum; pushing silence here was the audible LuxSynth
                   * micro-dropout whenever this tick collided with device
                   * line-rate staging (s_silenced/s_have_gen untouched). */
    if (mixed == 0)
    {
        /* No active "→ LUXSYNTH" send THIS tick. A transient inactive — a
         * player stop that restages within a few ms, a no-signal flicker —
         * must NOT wipe the spectrum: that instant wipe WAS the audible
         * "micro coupure" (field logs: clicks == silencePushes, 1:1). Only
         * a PERSISTENT no-send state is a real removal/stop → then the
         * no-send contract applies (~100 ms at the 2-4 ms tick rate). */
        if (s_zero_ticks < LX_FEED_SILENCE_DEBOUNCE_TICKS)
        {
            if (++s_zero_ticks == LX_FEED_SILENCE_DEBOUNCE_TICKS)
                lx_feed_push_silence(nDisplay);
        }
        return;
    }
    s_zero_ticks = 0;

    /* (P4 — 2026-07-14: the global Chain-2 transport gate is GONE — each
     * "→ LUXSYNTH" send is gated at staging time by ITS chain's transport:
     * HOLD = the producer stops re-staging (the mix holds), STOP = the slot
     * ramps silent then deactivates. The mix below is transport-correct.) */
    if (s_have_gen && gen == s_last_gen && !s_silenced)
        return;   /* nothing restaged — the engine keeps its spectrum */
    s_last_gen = gen;
    s_have_gen = 1;
    s_silenced = 0;

    /* KissFFT config — (re)allocated on size change only (init-time). */
    if (s_cfg_n != N)
    {
        if (s_cfg) kiss_fft_free(s_cfg);
        s_cfg   = kiss_fftr_alloc(N, 0, NULL, NULL);
        s_cfg_n = N;
        memset(s_smoothed, 0, sizeof(s_smoothed));
        for (int k = 0; k < LX_FEED_MAX_BINS; ++k) s_harm[k] = 0.5f;
    }
    if (s_cfg == NULL)
        return;

    /* Hann window over the mixed conditioned line. */
    const float k2pi = 2.0f * (float) M_PI / (float) (N > 1 ? N - 1 : 1);
    for (int i = 0; i < N; ++i)
    {
        const float hann = 0.5f * (1.0f - cosf(k2pi * (float) i));
        s_in[i] = s_line[i] * hann;
    }
    kiss_fftr(s_cfg, s_in, s_out);

    /* Magnitudes — suppress DC, peak-normalise the displayed bins. */
    s_mags[0] = 0.0f;
    float maxMag = 1e-12f;
    for (int k = 1; k <= nDisplay; ++k)
    {
        const float re  = s_out[k].r;
        const float im  = s_out[k].i;
        const float mag = sqrtf(re * re + im * im);
        s_mags[k] = mag;
        if (mag > maxMag) maxMag = mag;
    }
    const float invMax = 1.0f / maxMag;
    for (int k = 1; k <= nDisplay; ++k)
        s_mags[k] *= invMax;

    /* Temporal smoothing — the alphas were tuned for the historical 30 fps UI
     * push; correct them for this thread's actual update rate so the musical
     * response is unchanged. */
    float sm = g_sp3ctra_config.lx_fft_smoothing;
    if (sm < 0.0f) sm = 0.0f;
    if (sm > 1.0f) sm = 1.0f;
    float aAtk = 0.80f - sm * 0.75f;
    float aRel = 0.50f - sm * 0.48f;
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        const uint64_t now =
            (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
        float dt = (s_last_push_us != 0)
                   ? (float) (now - s_last_push_us) * 1.0e-6f
                   : (1.0f / 30.0f);
        if (dt <= 0.0f || dt > 1.0f) dt = 1.0f / 30.0f;
        s_last_push_us = now;
        const float steps = dt * 30.0f;
        aAtk = 1.0f - powf(1.0f - aAtk, steps);
        aRel = 1.0f - powf(1.0f - aRel, steps);
    }
    for (int k = 0; k <= nDisplay; ++k)
    {
        const float cur  = s_mags[k];
        const float prev = s_smoothed[k];
        const float a    = (cur >= prev) ? aAtk : aRel;
        s_smoothed[k] = a * cur + (1.0f - a) * prev;
    }

    /* Per-bin harmonicity from the mixed RGB colour temperature (same
     * algorithm as the UI FFT view: global R-B bias removed, amplified,
     * lightly smoothed). */
    {
        float globalR = 0.0f, globalB = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            globalR += (float) s_r[i];
            globalB += (float) s_b[i];
        }
        const float globalBias = (globalR - globalB) / ((float) N * 255.0f);
        const float kHarmGain  = 4.0f;
        int regionW = N / (nDisplay > 0 ? nDisplay : 1);
        if (regionW < 1) regionW = 1;
        for (int k = 1; k <= nDisplay; ++k)
        {
            int posStart = (k - 1) * regionW;
            if (posStart > N - 1) posStart = N - 1;
            int posEnd = k * regionW;
            if (posEnd <= posStart) posEnd = posStart + 1;
            if (posEnd > N) posEnd = N;
            float sumR = 0.0f, sumB = 0.0f;
            for (int i = posStart; i < posEnd; ++i)
            {
                sumR += (float) s_r[i];
                sumB += (float) s_b[i];
            }
            const float n       = (float) (posEnd - posStart);
            float tempRaw = (sumR - sumB) / (n * 255.0f) - globalBias;
            float tempAmp = tempRaw * kHarmGain;
            if (tempAmp < -1.0f) tempAmp = -1.0f;
            if (tempAmp >  1.0f) tempAmp =  1.0f;
            const float newH = (tempAmp + 1.0f) * 0.5f;
            s_harm[k] = 0.40f * newH + 0.60f * s_harm[k];
        }
    }

    __atomic_fetch_add(&s_diag_spec_pushes, 1, __ATOMIC_RELAXED);
    luxsynth_engine_set_spectral_data(&g_luxsynth_engine,
                                      s_smoothed + 1,   /* skip DC */
                                      NULL,
                                      s_harm + 1,
                                      NULL, NULL,
                                      nDisplay);
}
