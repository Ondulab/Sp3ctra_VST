#include "synthesis/luxstral/vst_adapters_c.h"
#include "synthesis/luxstral/luxstral_engine.h"
#include "synthesis/luxstral/wave_generation.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "synthesis/luxstral/synth_luxstral_runtime.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); abort(); } } while (0)
int main(void) {
    g_log_level = LOG_LEVEL_ERROR;
    const int counts[] = {1,2,6,16};
    g_sp3ctra_config.sensor_dpi = 200;
    g_sp3ctra_config.pixels_per_note = 1;
    g_sp3ctra_config.audio_buffer_size = 32;
    g_sp3ctra_config.stereo_mode_enabled = 1;
    g_sp3ctra_config.sampling_frequency = 96000;
    for (int i = 0; i < SINE_TABLE_SIZE; ++i) g_sine_table[i] = sinf(i*6.2831853f/SINE_TABLE_SIZE);
    for (int cycle = 0; cycle < 3; ++cycle) for (int c = 0; c < 4; ++c) {
        g_sp3ctra_config.num_workers = counts[c];
        LuxStralEngine* e = &g_luxstral_engine;
        e->waves = calloc(1728,sizeof(struct wave)); CHECK(e->waves);
        for (int i = 0; i < 1728; ++i) {
            e->waves[i].phase_inc = 0.2f + i*0.01f;
            e->waves[i].physiological_gain = 1;
            e->waves[i].alpha_up = 0.001f;
        }
        CHECK(synth_prepare_runtime()==0);
        float* saved = e->additiveBuffer;
        const int sizes[] = {1,64,512,4096,32};
        for (int j = 0; j < 5; ++j) {
            g_sp3ctra_config.audio_buffer_size = sizes[j];
            CHECK(synth_prepare_runtime()==0);
            CHECK(e->additiveBuffer == saved && e->audio_buffer_size == sizes[j]);
            e->stereoBuffer_R[sizes[j]-1] = 0;
            e->processed_grayScale[CIS_MAX_PIXELS_NB-1] = 0;
        }
        g_sp3ctra_config.audio_buffer_size = 4097;
        CHECK(synth_prepare_runtime()!=0);
        g_sp3ctra_config.audio_buffer_size = 32;
        CHECK(e->started_auxiliaries==counts[c]-1);
        for (int w = 0; w < e->num_workers; ++w) {
            synth_thread_worker_t* p = &e->thread_pool[w];
            for (int i = 0; i < p->end_note-p->start_note; ++i) p->precomputed_volume[i]=0.1f;
        }
        for (int batch=0; batch<100; ++batch) {
            synth_work_dispatch_begin(e->work_dispatch);
            synth_process_worker_range(&e->thread_pool[0]);
            synth_work_dispatch_finish(e->work_dispatch);
            for (int i = 0; i < 1728; ++i) {
                CHECK(isfinite(e->waves[i].phase_acc));
                CHECK(e->waves[i].current_volume > 0);
            }
        }
        synth_shutdown_thread_pool();
        CHECK(!e->thread_pool && !e->worker_threads && !e->work_dispatch);
        free((void*)e->waves); e->waves=NULL;
        synth_runtime_free_buffers();
        synth_luxstral_cleanup();
        CHECK(!e->additiveBuffer && !e->grayScale_live && !e->processed_grayScale);

    }
    puts("PASS: real LuxStral worker pool: 1/2/6/16 partitions, render, repeated shutdown/restart");
}
