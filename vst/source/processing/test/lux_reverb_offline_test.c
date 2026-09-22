/* Deterministic regression tests, run from the repository root:
 * cc -O2 -std=c11 -Wall -Wextra -Werror -Ivst/source/processing \
 *   vst/source/processing/test/lux_reverb_offline_test.c \
 *   vst/source/processing/lux_reverb.c -lm -o /tmp/lux_reverb_test
 * /tmp/lux_reverb_test
 */
#include "lux_reverb.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static LuxReverbState state;
static uint8_t input[3][LUX_REVERB_MAX_PIXELS];
static const uint8_t *output[3];
static uint64_t now_us;
static int failures;
#define CHECK(c, message) do { if (!(c)) { ++failures; printf("FAIL: %s\n", message); } } while (0)

static void start(int white, float diffusion)
{
    lux_reverb_init(&state);
    state.config.enabled = 1;
    state.config.background_mode = white ? LUX_REVERB_BG_WHITE : LUX_REVERB_BG_BLACK;
    state.config.mix = 1.0f;
    state.config.diffusion = diffusion;
    state.config.damping = 0.0f;   /* flat — the damping tests set their own */
    now_us = 1000000;
}

/* Energy lost by an undamped pixel after `seconds` at the default 3 s decay
 * (linear fade: 255 LSB over decay_s). */
static float faded(float seconds) { return 255.0f * seconds / 3.0f; }

static void background(int white, int noisy, int frame, int px)
{
    /* Deliberately tinted paper/channel offsets, plus +/-4 LSB texture. */
    const int base[3] = { 12, 28, 50 };
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < px; ++i)
        {
            const int e = base[c] + (noisy ? (i * 7 + frame * 5 + c) % 9 - 4 : 0);
            input[c][i] = (uint8_t)(white ? 255 - e : e);
        }
}

static void push(int px, uint64_t elapsed)
{
    now_us += elapsed;
    lux_reverb_process_frame_at(&state, input[0], input[1], input[2], px, 8,
        now_us, &output[0], &output[1], &output[2]);
}

static int unchanged(int px)
{
    for (int c = 0; c < 3; ++c)
        if (memcmp(input[c], output[c], (size_t)px)) return 0;
    return 1;
}

static void test_background(void)
{
    int altered = 0;
    for (int white = 0; white <= 1; ++white)
        for (int noisy = 0; noisy <= 1; ++noisy)
        {
            start(white, 1.0f);
            for (int frame = 0; frame < 2000; ++frame)
            {
                background(white, noisy, frame, 256);
                push(256, 1000);
                altered += !unchanged(256);
            }
            CHECK(!lux_reverb_tail_alive(&state), "background must not sustain a tail");
            CHECK(state.active_ticks == 0, "background must not flash the activity LED");
        }
    printf("Background: %d altered lines out of 8000\n", altered);
    CHECK(altered == 0, "tinted/noisy backgrounds must remain byte-identical");

    start(1, 1.0f);
    background(1, 0, 0, 256);
    push(256, 1000);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) input[c][i] -= 30;
    push(256, 1000);
    CHECK(!lux_reverb_tail_alive(&state), "paper brightness step must not excite broadband tail");
}

static void test_decay_and_diffusion(void)
{
    for (int white = 0; white <= 1; ++white)
        for (int diffuse = 0; diffuse <= 1; ++diffuse)
        {
            start(white, (float)diffuse);
            background(white, 0, 0, 256);
            push(256, 1000);
            input[0][128] = white ? 0 : 255;
            push(256, 1000);
            CHECK(output[0][128] == input[0][128], "dry note attack must be preserved");
            const float peak = state.tail_peak;
            const float raw0 = state.tail_r[128];   /* stored envelope, pre-diffusion */
            float previous = peak;
            background(white, 0, 0, 256);
            int outside = 0;
            for (int f = 0; f < 1000; ++f)
            {
                push(256, 1000);
                CHECK(state.tail_peak <= previous, "tail must decay monotonically");
                previous = state.tail_peak;
                for (int i = 0; i < 256; ++i)
                    if (i < 127 || i > 129) outside += output[0][i] != input[0][i];
                CHECK(memcmp(input[1], output[1], 256) == 0 &&
                      memcmp(input[2], output[2], 256) == 0, "note must not leak into other colours");
            }
            printf("Decay (white=%d, diffusion=%d): %.2f of %.0f left, outside support: %d\n",
                white, diffuse, state.tail_r[128], raw0, outside);
            CHECK(fabsf(state.tail_r[128] - (raw0 - faded(1.0f))) < 0.01f,
                  "3 s decay must have lost 85 LSB (a third of full scale) at 1 s");
            CHECK(outside == 0, "diffusion must not keep spreading into new pitches");
            CHECK(lux_reverb_tail_alive(&state), "note must still ring after 1 s");
            for (int f = 0; f < 1000; ++f) push(256, 1000);
            CHECK(lux_reverb_tail_alive(&state), "note must still ring after 2 s");
            for (int f = 0; f < 2000; ++f) push(256, 1000);
            CHECK(state.tail_peak == 0.0f && !lux_reverb_tail_alive(&state), "tail must die completely at 3 s");
            CHECK(unchanged(256), "extinguished tail must return exact background");
        }
}

static float render_rate(int interval)
{
    start(1, 0.8f);
    background(1, 0, 0, 256);
    push(256, 1000);
    input[0][128] = 0;
    push(256, 1000);
    background(1, 0, 0, 256);
    for (int t = 0; t < 1000000; t += interval) push(256, (uint64_t)interval);
    return state.tail_peak;
}

static void test_timing(void)
{
    const float a = render_rate(1000), b = render_rate(10000), c = render_rate(50000);
    CHECK(fabsf(a - b) < 0.01f && fabsf(a - c) < 0.01f, "tail must be invariant at 1000, 100 and 20 lines/s");
    const float before = state.tail_peak;
    for (int i = 0; i < 1000; ++i) push(256, 0);
    CHECK(state.tail_peak == before, "queued lines with zero elapsed time must not age the tail");
    push(256, 10000000);
    CHECK(!lux_reverb_tail_alive(&state) && unchanged(256), "long stall must not resurrect old tail");
    start(1, 0.0f);
    now_us = 0;
    background(1, 0, 0, 256);
    input[0][128] = 0;
    push(256, 0);
    const float zero_peak = state.tail_peak;
    background(1, 0, 0, 256);
    push(256, 1000000);
    CHECK(fabsf(state.tail_peak - (zero_peak - faded(1.0f))) < 0.01f, "offline clock may start at zero");
    const float saved_peak = state.tail_peak;
    now_us -= 500000;
    push(256, 0);
    now_us += 500000;
    push(256, 0);
    CHECK(state.tail_peak == saved_peak, "backward timestamp must not double-count time on recovery");

}

static void test_knee_and_lifecycle(void)
{
    start(0, 0.0f);
    memset(input, 0, sizeof(input));
    push(256, 1000);
    input[0][128] = 9;
    push(256, 1000);
    CHECK(state.tail_peak < 0.5f, "first LSB above gate must not jump into an audible tail");
    input[0][128] = 200;
    push(256, 1000);
    CHECK(lux_reverb_tail_alive(&state), "strong note must excite the tail");
    state.config.mix = 0.0f;
    push(256, 1000);
    CHECK(output[0] == input[0] && !lux_reverb_tail_alive(&state), "mix zero bypasses and clears state");
    state.config.mix = 1.0f;
    memset(input, 0, sizeof(input));
    push(256, 1000);
    CHECK(unchanged(256), "re-enable must not revive old tail");
    input[0][128] = 200;
    push(256, 1000);
    state.config.enabled = 0;
    push(256, 1000);
    CHECK(output[0] == input[0] && !state.tail_active, "disable bypasses and clears state");
    state.config.enabled = 1;
    push(256, 1000);
    memset(input, 0, sizeof(input));
    push(128, 1000);
    CHECK(unchanged(128) && !lux_reverb_tail_alive(&state), "geometry change must clear tail");
    input[0][64] = 255;
    push(128, 1000);
    state.config.background_mode = LUX_REVERB_BG_WHITE;
    memset(input, 255, sizeof(input));
    push(128, 1000);
    CHECK(unchanged(128) && !lux_reverb_tail_alive(&state), "polarity switch must clear tail and floors");
    /* Exercise both boundaries and full capacity under ASan. */
    input[0][0] = input[0][LUX_REVERB_MAX_PIXELS - 1] = 0;
    state.config.diffusion = 1.0f;
    push(LUX_REVERB_MAX_PIXELS, 1000);
    push(1, 1000);
    CHECK(isfinite(state.tail_peak), "boundary processing must stay finite");
}

/* Damping: the treble edge fades faster than the bass by the law's ratio,
 * the bass keeps the Decay setting, and the shape is the shared
 * lux_reverb_damp_rate (the editor draws exactly this). */
static float ring_for(int type, float amount, int pixel, int interval_us)
{
    start(1, 0.0f);
    state.config.damp_type = type;
    state.config.damping   = amount;
    background(1, 0, 0, 256);
    push(256, 1000);
    input[0][pixel] = 0;                 /* full-scale note */
    push(256, 1000);
    background(1, 0, 0, 256);
    for (int t = 0; t < 1000000; t += interval_us) push(256, (uint64_t)interval_us);
    return state.tail_r[pixel];          /* energy left after 1 s */
}

static void test_damping(void)
{
    const float flat = ring_for(LUX_REVERB_DAMP_LINEAR, 0.0f, 255, 1000);
    CHECK(fabsf(flat - ring_for(LUX_REVERB_DAMP_AIR, 0.0f, 255, 1000)) < 0.01f
          && fabsf(flat - ring_for(LUX_REVERB_DAMP_LINEAR, 0.0f, 0, 1000)) < 0.01f,
          "damping 0 must fade every pixel alike whatever the law");

    for (int type = LUX_REVERB_DAMP_LINEAR; type <= LUX_REVERB_DAMP_AIR; ++type)
    {
        /* 50 %: the treble edge fades 8× faster → its 243 LSB note loses
         * 8 × 85 = 680 LSB in 1 s = gone; at 25 % (2.83×) it loses 240. */
        const float bass   = ring_for(type, 0.5f, 0, 1000);
        const float treble = ring_for(type, 0.5f, 255, 1000);
        const float mid    = ring_for(type, 0.5f, 128, 1000);
        const float top25  = ring_for(type, 0.25f, 255, 1000);
        printf("Damping law %d @50%%: bass %.1f, mid %.1f, treble %.1f; @25%% treble %.1f\n",
               type, bass, mid, treble, top25);
        CHECK(fabsf(bass - flat) < 0.5f, "bass edge must keep the full decay");
        CHECK(treble == 0.0f, "treble edge must be gone after 1 s at 50 %");
        CHECK(fabsf(top25 - (243.0f - faded(1.0f) * powf(2.0f, 1.5f))) < 0.5f,
              "treble edge at 25 % must fade 2^1.5 times faster");
        if (type == LUX_REVERB_DAMP_LINEAR)
            CHECK(fabsf(mid - (243.0f - faded(1.0f) * powf(2.0f, 3.0f * 128.0f / 255.0f))) < 0.5f,
                  "LINEAR: pixel 128 (u = 128/255) fades 2^(3u) times faster at 50 %");
        else
            CHECK(fabsf(mid - bass) < 3.0f,
                  "AIR: the axis centre (4 octaves under the top) keeps ~the full decay");
        CHECK(fabsf(ring_for(type, 0.25f, 200, 1000) - ring_for(type, 0.25f, 200, 20000)) < 0.01f,
              "damped fade must be invariant to the line rate");
    }
    CHECK(fabsf(state.damp_key_span - 8.0f) < 0.001f, "LUT must record the axis span for the editor");
}

int main(void)
{
    test_background();
    test_decay_and_diffusion();
    test_timing();
    test_knee_and_lifecycle();
    test_damping();
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
