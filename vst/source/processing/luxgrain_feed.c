/*
 * luxgrain_feed.c — see luxgrain_feed.h for the contract.
 */
#include "luxgrain_feed.h"
#include "synth_staging.h"
#include "config/config_loader.h"
#include "config/config_instrument.h"
#include "../audio/buffers/audio_image_buffers.h"
#include "../synthesis/luxgrain/luxgrain_vst_adapter.h"

#include <string.h>

extern AudioImageBuffers *g_audioImageBuffers;

/* All state below is touched by the audio-processing thread only. */
static float    s_line[CIS_MAX_PIXELS_NB];
static uint8_t  s_r[CIS_MAX_PIXELS_NB];
static uint8_t  s_g[CIS_MAX_PIXELS_NB];
static uint8_t  s_b[CIS_MAX_PIXELS_NB];
static uint32_t s_last_gen = 0;
static int      s_have_gen = 0;
static int      s_silenced = 0;

/* Consecutive no-send ticks before the no-signal wipe fires (~100 ms at the
 * 2-4 ms tick rate) — a shorter flicker holds the ring instead (the same
 * debounce that fixed the LuxSynth micro-coupures). */
#define LG_FEED_SILENCE_DEBOUNCE_TICKS 50
static int s_zero_ticks = 0;

static volatile uint64_t s_diag_silence_pushes = 0;
static volatile uint64_t s_diag_line_pushes    = 0;

uint64_t luxgrain_feed_silence_pushes(void)
{
    return __atomic_load_n(&s_diag_silence_pushes, __ATOMIC_RELAXED);
}

uint64_t luxgrain_feed_line_pushes(void)
{
    return __atomic_load_n(&s_diag_line_pushes, __ATOMIC_RELAXED);
}

void luxgrain_feed_tick(const ChainPlan* plan)
{
    if (plan == NULL || !g_luxgrain_engine.initialized)
        return;

    const int N = get_cis_pixels_nb();
    if (N <= 0 || N > CIS_MAX_PIXELS_NB)
        return;

    int      nbp = 0;
    uint32_t gen = 0;
    const int mixed = synth_staging_mix_luxgrain(plan, s_line, s_r, s_g, s_b,
                                                 N, &nbp, &gen);
    if (mixed < 0)
        return;   /* torn slot — HOLD the ring, retry next tick */

    if (mixed == 0)
    {
        if (s_zero_ticks < LG_FEED_SILENCE_DEBOUNCE_TICKS)
        {
            if (++s_zero_ticks == LG_FEED_SILENCE_DEBOUNCE_TICKS
                && !s_silenced)
            {
                __atomic_fetch_add(&s_diag_silence_pushes, 1,
                                   __ATOMIC_RELAXED);
                luxgrain_engine_stage_silence(&g_luxgrain_engine);
                /* Head-panel tap → WHITE (engine unfed, no-signal contract). */
                audio_image_buffers_publish_engine_input(
                    g_audioImageBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXGRAIN,
                    NULL, NULL, NULL, N);
                s_silenced = 1;
                s_have_gen = 0;   /* force a re-stage when signal returns */
            }
        }
        return;
    }
    s_zero_ticks = 0;

    if (s_have_gen && gen == s_last_gen && !s_silenced)
        return;   /* nothing restaged — the ring holds */
    s_last_gen = gen;
    s_have_gen = 1;
    s_silenced = 0;

    __atomic_fetch_add(&s_diag_line_pushes, 1, __ATOMIC_RELAXED);
    luxgrain_engine_stage_line(&g_luxgrain_engine, s_line, s_r, s_g, s_b,
                               N, gen);
    /* Head-panel tap: the exact RGB mix the engine folds this cycle (the
     * GRAIN_GRAY / GRAIN_COLOR panels condition it for display themselves,
     * like the other engine taps). Gen-gated → ~line rate, not tick rate. */
    audio_image_buffers_publish_engine_input(
        g_audioImageBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXGRAIN,
        s_r, s_g, s_b, N);
}
