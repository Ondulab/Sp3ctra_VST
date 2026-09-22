/* macOS regression: the real LuxStral pool, old asymmetric scheduling versus
 * the matched team policy, including a 128 -> 512 host-buffer change.
 * Run outside a host: it temporarily makes THIS test thread real-time. */
#include "synthesis/luxstral/vst_adapters_c.h"
#include "synthesis/luxstral/luxstral_engine.h"
#include "synthesis/luxstral/wave_generation.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); abort(); } } while (0)
#define BLOCKS 400
static uint64_t ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static void ordinary(void) {
    thread_extended_policy_data_t p={.timeshare=TRUE};
    CHECK(thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_EXTENDED_POLICY,
          (thread_policy_t)&p,THREAD_EXTENDED_POLICY_COUNT)==KERN_SUCCESS);
    thread_precedence_policy_data_t q={.importance=0};
    thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_PRECEDENCE_POLICY,
                      (thread_policy_t)&q,THREAD_PRECEDENCE_POLICY_COUNT);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
}
static thread_time_constraint_policy_data_t read_policy(pthread_t thread) {
    thread_time_constraint_policy_data_t p={0};
    mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def=FALSE;
    CHECK(thread_policy_get(pthread_mach_thread_np(thread),THREAD_TIME_CONSTRAINT_POLICY,
                            (thread_policy_t)&p,&n,&def)==KERN_SUCCESS);
    return p;
}
static void verify_team(LuxStralEngine *e) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t period=(uint64_t)g_sp3ctra_config.audio_buffer_size*1000000000ULL /
                    g_sp3ctra_config.sampling_frequency;
    uint32_t expected=(uint32_t)(period*tb.denom/tb.numer);
    thread_time_constraint_policy_data_t p=read_policy(pthread_self());
    CHECK(p.period==expected && p.computation==(uint32_t)(expected*0.75));
    for(int w=1;w<=e->started_auxiliaries;++w) {
        thread_time_constraint_policy_data_t q=read_policy(e->worker_threads[w]);
        CHECK(q.period==p.period && q.computation==p.computation && q.constraint==p.constraint);
    }
}
static void *replacement_producer(void *arg) {
    ordinary();
    LuxStralEngine *e=arg;
    synth_update_realtime_team_policy(e);
    verify_team(e);
    ordinary();
    return NULL;
}
static uint64_t hash(uint64_t h,const void*ptr,size_t n) {
    const unsigned char*p=ptr;
    while(n--)h=(h^*p++)*1099511628211ULL;
    return h;
}
static int cmp(const void*a,const void*b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);
}
int main(void) {
    setvbuf(stdout,NULL,_IOLBF,0);
    g_log_level=LOG_LEVEL_ERROR;
    ordinary();
    g_sp3ctra_config.sensor_dpi=400;
    g_sp3ctra_config.pixels_per_note=1;
    g_sp3ctra_config.num_workers=6;
    g_sp3ctra_config.sampling_frequency=96000;
    g_sp3ctra_config.audio_buffer_size=128;
    g_sp3ctra_config.stereo_mode_enabled=1;
    LuxStralEngine *e=&g_luxstral_engine;
    e->waves=calloc(CIS_MAX_PIXELS_NB,sizeof(struct wave));CHECK(e->waves);
    for(int i=0;i<SINE_TABLE_SIZE;++i)g_sine_table[i]=sinf(i*6.2831853f/SINE_TABLE_SIZE);
    CHECK(synth_prepare_runtime()==0);
    g_sp3ctra_config.audio_buffer_size=512;
    CHECK(synth_prepare_runtime()==0);
    uint64_t hashes[2];
    for(int mode=0;mode<2;++mode) {
        // Match the previous release's stale 128-frame, 50% auxiliary policy.
        // The producer remains QoS-only in the baseline phase.
        if(mode==0)for(int w=1;w<=e->started_auxiliaries;++w) {
            thread_time_constraint_policy_data_t p=read_policy(e->worker_threads[w]);
            p.computation=p.period/2; p.preemptible=FALSE;
            CHECK(thread_policy_set(pthread_mach_thread_np(e->worker_threads[w]),
                THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&p,
                THREAD_TIME_CONSTRAINT_POLICY_COUNT)==KERN_SUCCESS);
        }
        if(mode) {
            synth_update_realtime_team_policy(e); verify_team(e);
            g_sp3ctra_config.sampling_frequency=48000;
            synth_update_realtime_team_policy(e); verify_team(e);
            g_sp3ctra_config.audio_buffer_size=256;
            synth_update_realtime_team_policy(e); verify_team(e);
            g_sp3ctra_config.sampling_frequency=96000;
            g_sp3ctra_config.audio_buffer_size=512;
            synth_update_realtime_team_policy(e); verify_team(e);
        }
        memset((void*)e->waves,0,CIS_MAX_PIXELS_NB*sizeof(struct wave));
        for(int i=0;i<CIS_MAX_PIXELS_NB;++i) {
            e->waves[i].phase_inc=0.2f+i*0.01f;
            e->waves[i].physiological_gain=1;
            e->waves[i].alpha_up=e->waves[i].alpha_down_weighted=0.001f;
            e->waves[i].current_volume=0.1f;
        }
        for(int w=0;w<e->num_workers;++w)for(int i=0;i<e->thread_pool[w].end_note-e->thread_pool[w].start_note;++i)
            e->thread_pool[w].precomputed_volume[i]=0.1f;
        uint64_t sum[3]={0},max[3]={0},times[BLOCKS],h=1469598103934665603ULL;
        int misses=0;
        const uint64_t budget=512ULL*1000000000ULL/96000;
        for(int b=0;b<BLOCKS;++b) {
            uint64_t a=ns();
            synth_work_dispatch_begin(e->work_dispatch);
            uint64_t o=ns();
            synth_process_worker_range(&e->thread_pool[0]);
            uint64_t j=ns();
            synth_work_dispatch_finish(e->work_dispatch);
            uint64_t z=ns(),d[3]={o-a,j-o,z-j};
            for(int k=0;k<3;++k){sum[k]+=d[k];if(d[k]>max[k])max[k]=d[k];}
            times[b]=z-a; if(times[b]>budget)++misses;
            for(int w=0;w<e->num_workers;++w) {
                h=hash(h,e->thread_pool[w].thread_luxstralBuffer_L,512*sizeof(float));
                h=hash(h,e->thread_pool[w].thread_luxstralBuffer_R,512*sizeof(float));
            }
            // Pacing, without spin or unbounded catch-up after a late iteration.
            uint64_t elapsed=ns()-a;
            if(elapsed<budget){struct timespec t={0,(long)(budget-elapsed)};nanosleep(&t,NULL);}
        }
        qsort(times,BLOCKS,sizeof(uint64_t),cmp);
        printf("%s: launch avg/max %.2f/%.2fus own %.2f/%.2fus join %.2f/%.2fus total avg %.2fus P99 %.2fus deadlines %d/%d\n",
            mode?"matched RT":"legacy QoS/RT",sum[0]/(BLOCKS*1000.),max[0]/1000.,
            sum[1]/(BLOCKS*1000.),max[1]/1000.,sum[2]/(BLOCKS*1000.),max[2]/1000.,
            (sum[0]+sum[1]+sum[2])/(BLOCKS*1000.),times[BLOCKS*99/100]/1000.,misses,BLOCKS);
        hashes[mode]=hash(h,(const void*)e->waves,CIS_MAX_PIXELS_NB*sizeof(struct wave));
    }
    ordinary();
    CHECK(hashes[0]==hashes[1]);
    // A new producer at the same format must not inherit the old thread's cache.
    pthread_t replacement;
    CHECK(pthread_create(&replacement,NULL,replacement_producer,e)==0);
    CHECK(pthread_join(replacement,NULL)==0);
    synth_shutdown_thread_pool();
    synth_luxstral_cleanup();
    free((void*)e->waves);e->waves=NULL;
    puts("PASS: real pool, matching Mach policies after SR/buffer changes, identical audio/state");
}
