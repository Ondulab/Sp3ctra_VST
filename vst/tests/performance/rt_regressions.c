#include "processing/chain_plan.h"
#include "synthesis/luxwave/synth_luxwave_engine.h"
#include "utils/rt_block_metrics.h"
#include "synthesis/luxsynth/synth_luxsynth_engine.h"
#include "synthesis/luxgrain/synth_luxgrain_engine.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

static atomic_int done;
static void* publish_plans(void* unused) {
    (void)unused;
    ChainPlan p;
    for (unsigned n = 1; n <= 10000; ++n) {
        memset(&p, (int)(n % 251 + 1), sizeof(p));
        chain_plan_publish(&p);
    }
    atomic_store(&done, 1);
    return 0;
}
static void* read_plans(void* unused) {
    (void)unused;
    do {
        ChainPlan p;
        chain_plan_get(&p);
        const unsigned char* bytes = (const unsigned char*)&p;
        for (size_t i = 1; i < sizeof(p); ++i) assert(bytes[i] == bytes[0]);
    } while (!atomic_load(&done));
    return 0;
}
static LuxWaveEngine wave;
static void* publish_waves(void* unused) {
    (void)unused;
    float line[LUXWAVE_MAX_PIXELS];
    for (int n = 0; n < 30000; ++n) {
        const float val = (float)(n % 100) / 100.0f;
        for (int i = 0; i < LUXWAVE_MAX_PIXELS; ++i) line[i] = val;
        luxwave_engine_set_image_line(&wave, line, LUXWAVE_MAX_PIXELS);
    }
    atomic_store(&done, 1);
    return 0;
}
static void test_wave(void) {
    luxwave_engine_init(&wave, 48000, 64);
    float line[LUXWAVE_MAX_PIXELS];
    for (int i = 0; i < LUXWAVE_MAX_PIXELS; ++i) line[i] = 0.2f;
    luxwave_engine_set_image_line(&wave, line, LUXWAVE_MAX_PIXELS);
    float l[64], r[64];
    luxwave_engine_process(&wave, 64, l, r);
    float old = wave.wt_buf[wave.wt_read_idx][0];
    for (int i = 0; i < LUXWAVE_MAX_PIXELS; ++i) line[i] = 0.8f;
    luxwave_engine_set_image_line(&wave, line, LUXWAVE_MAX_PIXELS);
    luxwave_engine_process(&wave, 64, l, r);
    luxwave_engine_process(&wave, 64, l, r);
    assert(wave.wt_buf[3][0] == old);
    atomic_store(&done, 0);
    pthread_t writer;
    pthread_create(&writer, 0, publish_waves, 0);
    do {
        luxwave_engine_process(&wave, 17, l, r);
        const int front = wave.wt_read_idx;
        for (int i = 1; i < wave.wt_pixel_count[front]; ++i)
            assert(wave.wt_buf[front][i] == wave.wt_buf[front][0]);
        for (int i = 1; i < wave.wt_pixel_count[3]; ++i)
            assert(wave.wt_buf[3][i] == wave.wt_buf[3][0]);
        for (int i = 0; i < 17; ++i) assert(isfinite(l[i]) && isfinite(r[i]));
    } while (!atomic_load(&done));
    pthread_join(writer, 0);
    l[0] = 123.0f;
    luxwave_engine_process(&wave, 0, l, r);
    luxwave_engine_process(&wave, -1, l, r);
    assert(l[0] == 123.0f);
}
static RTBlockMetrics metrics;
static void test_metrics(void) {
    for (unsigned i = 1; i <= 1000; ++i) rt_block_record(&metrics, i * 1000, 900000);
    rt_block_drain(&metrics);
    assert(metrics.count == 1000 && metrics.deadlines == 100);
    assert(metrics.max_ns == 1000000 && metrics.min_headroom_ns == -100000);
    assert(rt_block_percentile_us(&metrics, 950) == 950);
    assert(rt_block_percentile_us(&metrics, 990) == 990);
    assert(rt_block_percentile_us(&metrics, 999) == 999);
    rt_block_reset_window(&metrics);
    for (unsigned i = 0; i < RT_BLOCK_RING_SIZE + 11; ++i) rt_block_record(&metrics, 50, 100);
    assert(atomic_load(&metrics.dropped) == 11);
    rt_block_drain(&metrics);
    assert(metrics.count == RT_BLOCK_RING_SIZE);
    // Exercise unsigned cursor wrap and overflow bin reporting.
    atomic_store(&metrics.read_pos, UINT32_MAX - 2);
    atomic_store(&metrics.write_pos, UINT32_MAX - 2);
    rt_block_reset_window(&metrics);
    for (unsigned i = 0; i < 6; ++i) rt_block_record(&metrics, 70000000, 2000);
    rt_block_drain(&metrics);
    assert(metrics.count == 6 && metrics.overflows == 6);
}
static RTBlockMetrics concurrent_metrics;
static void* write_metrics(void* unused) {
    (void)unused;
    for (unsigned i = 0; i < 100000; ++i) rt_block_record(&concurrent_metrics, 777000, 1000000);
    atomic_store(&done, 1);
    return NULL;
}
static void test_concurrent_metrics(void) {
    atomic_store(&done, 0);
    pthread_t writer; pthread_create(&writer, NULL, write_metrics, NULL);
    do { rt_block_drain(&concurrent_metrics); } while (!atomic_load(&done));
    pthread_join(writer, NULL); rt_block_drain(&concurrent_metrics);
    assert(concurrent_metrics.count + atomic_load(&concurrent_metrics.dropped) == 100000);
    assert(concurrent_metrics.total_ns == concurrent_metrics.count * 777000);
    assert(concurrent_metrics.max_ns == 777000 && concurrent_metrics.deadlines == 0);
}
static LuxSynthEngine additive;
static LuxGrainEngine grain;
static void* publish_spectral(void* unused) {
    (void)unused;
    float data[LUXGRAIN_MAX_PIXELS];
    for (int n = 0; n < 5000; ++n) {
        const float val = (n % 100) / 100.0f;
        for (int i = 0; i < LUXGRAIN_MAX_PIXELS; ++i) data[i] = val;
        luxsynth_engine_set_spectral_data(&additive, data, data, data, data, data, 16);
        luxgrain_engine_stage_line(&grain, data, NULL, NULL, NULL, LUXGRAIN_MAX_PIXELS, n);
        if (n % 19 == 0) luxgrain_engine_stage_silence(&grain);
    }
    atomic_store(&done, 1);
    return NULL;
}
static void test_spectral(void) {
    luxsynth_engine_init(&additive, 48000, 64);
    LuxSynthConfig cfg = additive.config; cfg.enabled = 1;
    luxsynth_engine_set_config(&additive, &cfg);
    luxsynth_engine_note_on(&additive, 60, 100);
    luxgrain_engine_init(&grain, 48000);
    LuxGrainConfig gc = luxgrain_config_default(); gc.enabled = 1;
    luxgrain_engine_set_config(&grain, &gc);
    atomic_store(&done, 0);
    pthread_t writer; pthread_create(&writer, NULL, publish_spectral, NULL);
    float l[64], r[64]; int iteration = 0;
    do {
        luxsynth_engine_process(&additive, 17, l, r);
        for (int i = 0; i < additive.spectral.num_bins; ++i) {
            assert(additive.spectral.magnitudes[i] == additive.spectral.magnitudes[0]);
            assert(additive.spectral.magnitudes[i] == additive.spectral.left_gains[i]);
        }
        gc.num_bands = (++iteration % 2) ? 16 : LUXGRAIN_MAX_BANDS;
        luxgrain_engine_set_config(&grain, &gc);
        luxgrain_engine_process(&grain, l, r, 17);
        for (int i = 0; i < 17; ++i) assert(isfinite(l[i]) && isfinite(r[i]));
    } while (!atomic_load(&done));
    pthread_join(writer, NULL);
    struct { float samples[4096]; float guard; } guardedL, guardedR;
    guardedL.guard = guardedR.guard = 12345.0f;
    luxsynth_engine_process(&additive, 8192, guardedL.samples, guardedR.samples);
    assert(guardedL.guard == 12345.0f && guardedR.guard == 12345.0f);
    luxwave_engine_process(&wave, 8192, guardedL.samples, guardedR.samples);
    assert(guardedL.guard == 12345.0f && guardedR.guard == 12345.0f);
    luxgrain_engine_process(&grain, guardedL.samples, guardedR.samples, 8192);
    assert(guardedL.guard == 12345.0f && guardedR.guard == 12345.0f);
}
int main(void) {
    pthread_t writer, readers[4];
    for (unsigned i = 0; i < 4; ++i) pthread_create(&readers[i], 0, read_plans, 0);
    pthread_create(&writer, 0, publish_plans, 0);
    pthread_join(writer, 0);
    for (unsigned i = 0; i < 4; ++i) pthread_join(readers[i], 0);
    test_wave(); test_metrics(); test_concurrent_metrics(); test_spectral();
    puts("PASS: coherent ChainPlan snapshots, LuxWave crossfades, LuxSynth/LuxGrain mailboxes, block metrics");
}
