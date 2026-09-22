#include "vst_adapters.h"
#include "luxstral_engine.h"
#include "wave_generation.h"
#include "luxstral_wavetable.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
sp3ctra_config_t g_sp3ctra_config;
float g_sine_table[SINE_TABLE_SIZE];
float g_square_table[SINE_TABLE_SIZE];
const luxstral_wavetable_t* luxstral_wavetable_acquire(void) { return NULL; }
void log_info(const char* module, const char* fmt, ...) { (void)module; (void)fmt; }
int is_startup_full_verbose(void) { return 0; }
void log_debug(const char* module, const char* fmt, ...) { (void)module; (void)fmt; }
static LuxStralEngine engine;
static synth_thread_worker_t worker;
#define ALLOC(field,count) worker.field = calloc((count), sizeof(float))
int main(void) {
    enum { notes = 576, capacity = 4096 };
    engine.waves = calloc(notes, sizeof(struct wave));
    worker.engine = &engine; worker.start_note = 0; worker.end_note = notes;
    ALLOC(thread_luxstralBuffer,capacity); ALLOC(thread_sumVolumeBuffer,capacity);
#ifdef BASELINE
    ALLOC(thread_maxVolumeBuffer,capacity);
    ALLOC(precomputed_wave_data,notes*capacity);
#else
    ALLOC(precomputed_wave_data,capacity);
#endif
    ALLOC(thread_luxstralBuffer_L,capacity); ALLOC(thread_luxstralBuffer_R,capacity);
    ALLOC(waveBuffer,capacity); ALLOC(volumeBuffer,capacity);
    ALLOC(temp_waveBuffer_L,capacity); ALLOC(temp_waveBuffer_R,capacity);
    ALLOC(precomputed_volume,notes); ALLOC(precomputed_left_gain,notes);
    ALLOC(precomputed_right_gain,notes); ALLOC(last_left_gain,notes); ALLOC(last_right_gain,notes);
    for (int i = 0; i < SINE_TABLE_SIZE; ++i) {
        g_sine_table[i] = sinf(6.2831853f * i / SINE_TABLE_SIZE);
        g_square_table[i] = tanhf(2.0f * g_sine_table[i]);
    }
    for (int i = 0; i < notes; ++i) {
        engine.waves[i].phase_acc = (float)i;
        engine.waves[i].phase_inc = 0.2f + (float)i * 0.08f;
        engine.waves[i].physiological_gain = 0.8f;
        engine.waves[i].alpha_up = 0.001f;
        engine.waves[i].alpha_down_weighted = 0.0002f;
        worker.precomputed_left_gain[i] = 0.7f;
        worker.precomputed_right_gain[i] = 0.3f;
    }
    const int sizes[] = {1,17,64,256,4096};
    const clock_t start = clock();
    for (int mode = 0; mode < 2; ++mode) for (int b = 0; b < 5; ++b) {
        const int size = sizes[b];
        g_sp3ctra_config.audio_buffer_size = size;
        g_sp3ctra_config.stereo_mode_enabled = mode;
        for (int frame = 0; frame < 12; ++frame) {
            engine.sf_morph = frame % 3 ? 0.0f : 0.5f;
            g_sp3ctra_config.luxstral_phase_mode = frame % 4;
            worker.rng_state = 123456u;
            for (int i = 0; i < notes; ++i) worker.precomputed_volume[i] = (i % 2 == 0) ? 0.0f : (float)((i + frame) % 17) / 17.0f;
            synth_process_worker_range(&worker);
            fwrite(worker.thread_luxstralBuffer, sizeof(float), size, stdout);
            fwrite(worker.thread_luxstralBuffer_L, sizeof(float), size, stdout);
            fwrite(worker.thread_luxstralBuffer_R, sizeof(float), size, stdout);
            fwrite(worker.thread_sumVolumeBuffer, sizeof(float), size, stdout);
            for (int i = 0; i < notes; ++i) {
                const float state[] = {engine.waves[i].phase_acc, engine.waves[i].current_volume};
                fwrite(state, sizeof(float), 2, stdout);
            }
        }
    }
    fprintf(stderr, "worker CPU %.4fs\n", (double)(clock()-start)/CLOCKS_PER_SEC);
}
