/* Regression: block-clock video masks must affect frozen source images,
 * independently of conditioning-bank indices and base send gains. */
#include "processing/synth_staging.h"
#include "utils/pipeline_metrics.h"
#include "processing/image_chain.h"
#include "config/config_loader.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
sp3ctra_config_t g_sp3ctra_config;
static ChainPlan plan;
static PreprocessedImageData pp;
static uint64_t mask(unsigned a, unsigned b) {
    return (UINT64_MAX & ~(UINT64_C(255)<<8) & ~(UINT64_C(255)<<24))
        | ((uint64_t)a<<8) | ((uint64_t)b<<24);
}
static void near(float got, float expected) { assert(fabsf(got-expected)<1e-6f); }
static void check(float expected, float wave_expected, uint32_t gen[3]) {
    float out[4]; int n, stereo;
    assert(synth_staging_mix_luxstral(&plan,out,4,NULL,NULL,&stereo,NULL,NULL,NULL,0,&n,&gen[0])>0);
    near(out[0],expected);
    assert(synth_staging_mix_luxsynth(&plan,out,NULL,NULL,NULL,4,&n,&gen[1])>0);
    near(out[0],expected);assert(n==4);
    assert(synth_staging_mix_luxgrain(&plan,out,NULL,NULL,NULL,4,&n,&gen[2])>0);
    near(out[0],expected);assert(n==4);
    assert(synth_staging_mix_luxwave(&plan,out,4,&n)>0);
    near(out[0],wave_expected);assert(n==4);
}
int main(void) {
    plan.num_chains=4;plan.num_ls_sends=2;
    for(int k=0;k<2;++k) {
        const int c=k?3:1, bank=k?2:5;
        plan.ls_send[k].chain_idx=c;
        plan.chain[c].present=1;plan.chain[c].num_inserts=3;
        plan.chain[c].insert_id[0]=IMAGE_CHAIN_INSERT_OUT_LUXSYNTH;
        plan.chain[c].insert_id[1]=IMAGE_CHAIN_INSERT_OUT_LUXWAVE;
        plan.chain[c].insert_id[2]=IMAGE_CHAIN_INSERT_OUT_LUXGRAIN;
        lux_out_params_t* banks[]={g_sp3ctra_config.luxstral_out,g_sp3ctra_config.luxsynth_out,g_sp3ctra_config.luxwave_out,g_sp3ctra_config.luxgrain_out};
        for(int e=0;e<4;++e){banks[e][bank].enabled=1;banks[e][bank].intensity=0.5f;}
        uint8_t rgb[4]={32,64,128,255};
        float line[4];for(int i=0;i<4;++i)line[i]=pp.additive.notes[i]=k?0.8f:0.4f;
        synth_staging_stage_luxstral(c,bank,&pp,4,0,NULL,NULL,NULL,4);
        synth_staging_stage_luxsynth(c,bank,line,rgb,rgb,rgb,4);
        synth_staging_stage_luxgrain(c,bank,line,NULL,NULL,NULL,4);
        synth_staging_stage_luxwave(c,bank,line,4);
    }
    uint32_t prev[3],gen[3];
    synth_staging_set_video_weights(UINT64_MAX);check(0.6f,0.6f,prev);
    synth_staging_set_video_weights(UINT64_MAX & ~UINT64_C(255));
    check(0.6f,0.6f,gen);
    for(int e=0;e<3;++e)assert(gen[e]==prev[e]); // unrelated chain: no extra FFT
    for(int i=0;i<1600;++i){
        synth_staging_set_video_weights(i%2?mask(255,0):mask(0,255));
        check(i%2?0.2f:0.4f,i%2?0.45f:0.65f,gen);
        for(int e=0;e<3;++e){assert(gen[e]!=prev[e]);prev[e]=gen[e];}
    }
    synth_staging_set_video_weights(mask(128,255));
    check(0.4f+0.2f*128.f/255.f,0.65f-0.05f*128.f/255.f,gen);
    // A full base-gain refresh must not erase or bake in the video mask.
    g_sp3ctra_config.luxstral_out[5].intensity=0.5f;
    synth_staging_set_video_weights(UINT64_MAX);check(0.6f,0.6f,gen);
    g_sp3ctra_config.luxstral_out[5].enabled=0;
    g_sp3ctra_config.luxsynth_out[5].enabled=0;
    g_sp3ctra_config.luxwave_out[5].enabled=0;
    g_sp3ctra_config.luxgrain_out[5].enabled=0;
    check(0.4f,0.65f,gen);
    synth_staging_set_video_weights(mask(0,0));
    float out[4]; int n;
    // LuxWave must publish the silent wavetable (return 0 would hold old audio).
    assert(synth_staging_mix_luxwave(&plan,out,4,&n)>0);
    for(int i=0;i<4;++i)near(out[i],0.5f);
    uint64_t events[PIPE_METRIC_COUNT]; pipeline_metrics_read(events);
    for(int e=0;e<4;++e)for(int c=0;c<8;++c)
        assert(events[PIPE_SEND+e*8+c] == (uint64_t)(c==1 || c==3));
    puts("Video audio staging: 1600 frozen-source switches across four engines passed");
}
