#include "synthesis/luxwave/synth_luxwave_engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static LuxWaveEngine engine;
int main(void) {
    const int blocks[] = {1, 17, 64, 256, 4096};
    const float rates[] = {44100, 48000, 96000};
    float image[LUXWAVE_MAX_PIXELS];
    for (int r = 0; r < 3; ++r) for (int b = 0; b < 5; ++b) {
        const int n = blocks[b];
        luxwave_engine_init(&engine, rates[r], n);
        LuxWaveConfig cfg = engine.config;
        cfg.enabled = 1; cfg.attack_curve = 0.4f; cfg.release_curve = -0.3f;
        cfg.lfo_rate_hz = 1.5f; cfg.lfo_depth_semitones = 0.5f;
        luxwave_engine_set_config(&engine, &cfg);
        for (int v = 0; v < 8; ++v) luxwave_engine_note_on(&engine, 48 + v * 3, 90);
        for (int pos = 0, next = 0; pos < 24000; pos += n) {
            if (pos >= next) {
                const int pixels = next % 2 ? 1728 : LUXWAVE_MAX_PIXELS;
                for (int i = 0; i < pixels; ++i)
                    image[i] = 0.5f + 0.4f * sinf((float)i * 6.2831853f / pixels + pos * 0.001f);
                luxwave_engine_set_image_line(&engine, image, pixels);
                next = pos + 4096; // previous crossfade completed before publication
            }
            if (pos >= 16000) luxwave_engine_all_notes_off(&engine);
            luxwave_engine_process(&engine, n, engine.output_left, engine.output_right);
            if (fwrite(engine.output_left, sizeof(float), n, stdout) != (size_t)n) return 1;
            if (fwrite(engine.output_right, sizeof(float), n, stdout) != (size_t)n) return 1;
        }
    }
    return 0;
}
