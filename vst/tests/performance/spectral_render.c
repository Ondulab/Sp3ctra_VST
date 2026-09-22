#include "synthesis/luxsynth/synth_luxsynth_engine.h"
#include "synthesis/luxgrain/synth_luxgrain_engine.h"
#include <math.h>
#include <stdio.h>
static LuxSynthEngine synth;
static LuxGrainEngine grain;
int main(void) {
    float mag[128], pan[128], harm[128], gain[128], line[LUXGRAIN_MAX_PIXELS];
    float l[4096], r[4096];
    const int sizes[] = {1,17,64,256,4096};
    const float rates[] = {44100,48000,96000};
    for (int sr = 0; sr < 3; ++sr) for (int bs = 0; bs < 5; ++bs) {
        const int n = sizes[bs];
        luxsynth_engine_init(&synth,rates[sr],n);
        luxgrain_engine_init(&grain,rates[sr]);
        LuxSynthConfig sc = synth.config; sc.enabled = 1; sc.attack_curve = 0.4f;
        LuxGrainConfig gc = luxgrain_config_default(); gc.enabled = 1; gc.density_hz = 80.0f;
        luxsynth_engine_set_config(&synth,&sc);
        for (int i = 0; i < 8; ++i) luxsynth_engine_note_on(&synth,48+3*i,80);
        for (int f = 0; f < 32; ++f) {
            gc.num_bands = (f < 16) ? 64 : 128;
            luxgrain_engine_set_config(&grain,&gc);
            for (int i = 0; i < 128; ++i) {
                mag[i] = 0.5f + 0.4f*sinf(i*0.1f + f); pan[i] = 0.2f;
                harm[i] = 0.9f; gain[i] = 0.7f;
            }
            for (int i = 0; i < LUXGRAIN_MAX_PIXELS; ++i) line[i] = 0.5f+0.4f*sinf(i*0.01f+f);
            luxsynth_engine_set_spectral_data(&synth,mag,pan,harm,gain,gain,128);
            luxgrain_engine_stage_line(&grain,line,NULL,NULL,NULL,LUXGRAIN_MAX_PIXELS,f);
            if (f == 24) luxgrain_engine_stage_silence(&grain);
            luxsynth_engine_process(&synth,n,l,r);
            fwrite(l,sizeof(float),n,stdout); fwrite(r,sizeof(float),n,stdout);
            luxgrain_engine_process(&grain,l,r,n);
            fwrite(l,sizeof(float),n,stdout); fwrite(r,sizeof(float),n,stdout);
        }
    }
}
