/* A/B the old two-barrier scheduler and the new dispatcher with the same real
 * LuxStral kernel, partition data, QoS and reduction order. No device/GUI.
 * This measures an unpaced isolated kernel, not plugin CPU or host xruns. */
#include "synthesis/luxstral/vst_adapters_c.h"
#include "synthesis/luxstral/luxstral_engine.h"
#include "synthesis/luxstral/wave_generation.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <string.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); abort(); } } while (0)
#define BATCHES 3000
static int legacy;
static uint64_t now_ns(clockid_t kind) {
    struct timespec t; clock_gettime(kind,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static void qos(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
}
static void *run(void *arg) {
    synth_thread_worker_t *w=arg;
    LuxStralEngine *e=w->engine;
    qos();
    for (int b=0;b<BATCHES;++b) {
        if (legacy) synth_barrier_wait(e,&e->worker_start_barrier);
        else CHECK(synth_work_dispatch_wait(e->work_dispatch,w->thread_id-1));
        synth_process_worker_range(w);
        if (legacy) synth_barrier_wait(e,&e->worker_end_barrier);
        else synth_work_dispatch_complete(e->work_dispatch);
    }
    return NULL;
}
static uint64_t hash(uint64_t h,const void *ptr,size_t n) {
    const unsigned char *p=ptr;
    while(n--) h=(h^*p++)*1099511628211ULL;
    return h;
}
static int cmp(const void *a,const void *b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;
    return (x>y)-(x<y);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_log_level=LOG_LEVEL_ERROR;
    g_sp3ctra_config.sensor_dpi=400;
    g_sp3ctra_config.pixels_per_note=1;
    g_sp3ctra_config.stereo_mode_enabled=1;
    g_sp3ctra_config.sampling_frequency=96000;
    qos();
    for(int i=0;i<SINE_TABLE_SIZE;++i) g_sine_table[i]=sinf(i*6.2831853f/SINE_TABLE_SIZE);
    const int sizes[]={32,64,128,256},counts[]={1,2,6};
    const int repeats=argc>1?atoi(argv[1]):1;
    CHECK(repeats>=1 && repeats<=10);
    for(int repeat=0;repeat<repeats;++repeat)
    for(int c=0;c<3;++c) for(int s=0;s<4;++s) {
        if(argc>2 && counts[c]!=atoi(argv[2])) continue;
        if(argc>3 && sizes[s]!=atoi(argv[3])) continue;
        uint64_t result[2];
        for(int pass=0;pass<2;++pass) {
            const int mode=pass^(repeat&1); // alternate order to limit time/heat bias
            legacy=(mode==0);
            LuxStralEngine *e=&g_luxstral_engine;
            g_sp3ctra_config.num_workers=counts[c];
            g_sp3ctra_config.audio_buffer_size=sizes[s];
            e->waves=calloc(CIS_MAX_PIXELS_NB,sizeof(struct wave)); CHECK(e->waves);
            for(int i=0;i<CIS_MAX_PIXELS_NB;++i) {
                e->waves[i].phase_inc=0.2f+i*0.01f;
                e->waves[i].physiological_gain=1;
                e->waves[i].alpha_up=0.001f;
            }
            CHECK(synth_init_thread_pool(e)==0);
            if(legacy) CHECK(synth_init_barriers(e,counts[c]+1)==0);
            for(int w=0;w<e->num_workers;++w) {
                synth_thread_worker_t *p=&e->thread_pool[w];
                for(int i=0;i<p->end_note-p->start_note;++i) p->precomputed_volume[i]=0.1f;
            }
            pthread_t threads[MAX_WORKERS];
            int first=legacy?0:1;
            for(int w=first;w<counts[c];++w) CHECK(pthread_create(&threads[w],NULL,run,&e->thread_pool[w])==0);
            uint64_t durations[BATCHES],sum=0,h=1469598103934665603ULL;
            uint64_t cpu=now_ns(CLOCK_PROCESS_CPUTIME_ID);
            for(int b=0;b<BATCHES;++b) {
                uint64_t t=now_ns(CLOCK_MONOTONIC);
                if(legacy) {
                    synth_barrier_wait(e,&e->worker_start_barrier);
                    synth_barrier_wait(e,&e->worker_end_barrier);
                } else {
                    synth_work_dispatch_begin(e->work_dispatch);
                    synth_process_worker_range(&e->thread_pool[0]);
                    synth_work_dispatch_finish(e->work_dispatch);
                }
                durations[b]=now_ns(CLOCK_MONOTONIC)-t;
                sum+=durations[b];
                // Include both rendered channels of every partition each block.
                for(int w=0;w<counts[c];++w) {
                    h=hash(h,e->thread_pool[w].thread_luxstralBuffer_L,sizes[s]*sizeof(float));
                    h=hash(h,e->thread_pool[w].thread_luxstralBuffer_R,sizes[s]*sizeof(float));
                }
            }
            cpu=now_ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
            for(int w=first;w<counts[c];++w) CHECK(pthread_join(threads[w],NULL)==0);
            result[mode]=hash(h,(const void*)e->waves,CIS_MAX_PIXELS_NB*sizeof(struct wave));
            qsort(durations,BATCHES,sizeof(uint64_t),cmp);
            printf("%s workers=%d samples=%d avg=%.2fus P99=%.2fus max=%.2fus cpu=%.3fs hash=%016llx\n",
                legacy?"barriers":"dispatch",counts[c],sizes[s],sum/(BATCHES*1000.0),
                durations[BATCHES*99/100]/1000.0,durations[BATCHES-1]/1000.0,cpu/1e9,(unsigned long long)result[mode]);
            if(legacy) synth_cleanup_barriers(e);
            synth_shutdown_thread_pool();
            free((void*)e->waves); e->waves=NULL;
        }
        CHECK(result[0]==result[1]);
    }
    puts("PASS: identical output/state for both schedulers in every selected configuration");
}
