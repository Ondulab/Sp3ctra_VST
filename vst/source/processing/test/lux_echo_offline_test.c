/*
 * lux_echo_offline_test.c
 *
 * Offline harness for the LuxEcho heap ring — no JUCE, no RT, no build-system
 * dependency (this file is NOT in CMakeLists; compile it directly):
 *
 *   cc -O2 -std=c11 -I.. -o /tmp/lux_echo_test \
 *      test/lux_echo_offline_test.c ../lux_echo.c
 *   /tmp/lux_echo_test
 *
 * Pins what the ring handoff must keep true:
 *   • a repeat lands exactly `delay` lines later, for delays far past the
 *     legacy 255 (5 000 and 30 000 lines);
 *   • growing the delay hands over a bigger ring and retires the old one,
 *     which the message thread frees (pool accounting returns to zero);
 *   • a line wider than the stride is stored truncated and reported, the
 *     next sync widens the ring;
 *   • the pool budget refuses a growth and the delay clamps to the ring;
 *   • switching the module off trades a big ring for the minimum one.
 */
#include "../lux_echo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* A dark-on-white line: one black stroke at pixel `at` on paper 230. */
static void make_line(uint8_t *r, uint8_t *g, uint8_t *b, int px, int at)
{
    memset(r, 230, (size_t) px); memset(g, 230, (size_t) px); memset(b, 230, (size_t) px);
    if (at >= 0 && at < px) { r[at] = g[at] = b[at] = 0; r[at+1] = g[at+1] = b[at+1] = 0; }
}

/* Push one line, return the output's darkest value at pixel `probe`. */
static int push(LuxEchoState *st, int px, int stroke_at, int probe)
{
    static uint8_t r[LUX_ECHO_MAX_PIXELS], g[LUX_ECHO_MAX_PIXELS], b[LUX_ECHO_MAX_PIXELS];
    const uint8_t *orr, *og, *ob;
    make_line(r, g, b, px, stroke_at);
    lux_echo_process_frame(st, r, g, b, px, 8, &orr, &og, &ob);
    return orr[probe];
}

/* Feed `n` blank lines, return the first line index (0-based) where the
 * probe pixel prints a repeat (darker than paper - 20), or -1. */
static int wait_repeat(LuxEchoState *st, int px, int probe, int n)
{
    for (int i = 0; i < n; ++i)
        if (push(st, px, -1, probe) < 230 - 20) return i;
    return -1;
}

int main(void)
{
    lux_echo_init_all();
    LuxEchoState *st = lux_echo_instance(1);
    const int px = 3456;

    printf("1. no ring yet = pass-through, and the width is reported\n");
    st->config.enabled = 1; st->config.mix = 1.0f; st->config.feedback = 0.0f;
    st->config.background_mode = LUX_ECHO_BG_WHITE;
    CHECK(push(st, px, 100, 100) == 0, "stroke passes through unchanged");
    CHECK(lux_echo_capacity(st) == 0, "capacity 0 before any ring");
    CHECK(lux_echo_wants_resync(st) == 1, "width reported → wants a sync");

    printf("2. first sync → ring for delay 5000, repeat exactly 5000 lines later\n");
    st->config.delay_lines = 5000;
    CHECK(lux_echo_ensure_capacity(st, 5000, 1728) == 1, "capacity granted");
    CHECK(lux_echo_wants_resync(st) == 0, "sync done → no more request");
    push(st, px, -1, 0);                       /* adopts */
    CHECK(lux_echo_capacity(st) >= 5000, "capacity %d >= 5000", lux_echo_capacity(st));
    /* Learn the floor for a while, then print a stroke and count. */
    for (int i = 0; i < 200; ++i) push(st, px, -1, 0);
    push(st, px, 100, 100);
    const int hit = wait_repeat(st, px, 100, 6000);
    CHECK(hit == 4999, "repeat after %d blank lines (expected 4999)", hit);
    const size_t bytes_5k = lux_echo_pool_bytes();
    printf("   pool = %.1f MB\n", (double) bytes_5k / 1048576.0);
    CHECK(bytes_5k > (size_t) 5000 * 3 * 3456 && bytes_5k < (size_t) 5400 * 3 * 3456 + 65536,
          "pool sized to the delay at the stream's width (3456), not 8192");

    printf("3. grow to 30000 → new ring adopted, old one retired then freed\n");
    st->config.delay_lines = LUX_ECHO_MAX_DELAY;
    CHECK(lux_echo_ensure_capacity(st, LUX_ECHO_MAX_DELAY, 1728) == 1, "growth granted");
    CHECK(lux_echo_pool_bytes() > bytes_5k * 5, "two rings alive while the handoff is pending");
    push(st, px, -1, 0);                       /* adopts, retires */
    CHECK(lux_echo_capacity(st) >= LUX_ECHO_MAX_DELAY, "capacity %d", lux_echo_capacity(st));
    lux_echo_collect(st);
    const size_t bytes_30k = lux_echo_pool_bytes();
    CHECK(bytes_30k < bytes_5k * 7 && bytes_30k > bytes_5k * 5, "old ring freed (pool %.1f MB)",
          (double) bytes_30k / 1048576.0);
    for (int i = 0; i < 200; ++i) push(st, px, -1, 0);
    push(st, px, 2000, 2000);
    const int hit30 = wait_repeat(st, px, 2000, LUX_ECHO_MAX_DELAY + 10);
    CHECK(hit30 == LUX_ECHO_MAX_DELAY - 1, "repeat after %d lines (expected %d)", hit30, LUX_ECHO_MAX_DELAY - 1);

    printf("4. a wider line is stored truncated, reported, and the next sync widens\n");
    st->config.delay_lines = 10;
    push(st, 8000, 7000, 7000);                /* stroke beyond the 3456 stride */
    CHECK(lux_echo_wants_resync(st) == 1, "8000 px reported");
    int printed = 0;
    for (int i = 0; i < 12; ++i) if (push(st, 8000, -1, 7000) < 210) printed = 1;
    CHECK(printed == 0, "no repeat beyond the stride before the widening");
    CHECK(lux_echo_ensure_capacity(st, 10, 1728) == 1, "widening granted");
    push(st, 8000, -1, 0);                     /* adopt the 8000-wide ring */
    for (int i = 0; i < 200; ++i) push(st, 8000, -1, 0);
    push(st, 8000, 7000, 7000);
    CHECK(wait_repeat(st, 8000, 7000, 20) == 9, "repeat at pixel 7000 once the ring is wide enough");
    lux_echo_collect(st);

    printf("5. off → the big ring is traded for the minimum one\n");
    st->config.enabled = 0;
    CHECK(lux_echo_ensure_capacity(st, 0, 1728) == 1, "shrink granted");
    push(st, px, -1, 0);                       /* disabled, but adopts */
    lux_echo_collect(st);
    printf("   pool after shrink = %.2f MB\n", (double) lux_echo_pool_bytes() / 1048576.0);
    CHECK(lux_echo_pool_bytes() < (size_t) 300 * 3 * 8192 + 65536, "pool back to a minimum ring");
    CHECK(lux_echo_capacity(st) == LUX_ECHO_MIN_SLOTS - 1, "capacity %d", lux_echo_capacity(st));
    CHECK(lux_echo_ensure_capacity(st, 0, 1728) == 1 && lux_echo_pool_bytes() < (size_t) 300 * 3 * 8192 + 65536,
          "a second off sync allocates nothing");

    printf("6. budget: fill other instances at max, the last growth is refused and clamps\n");
    size_t before = lux_echo_pool_bytes();
    int granted = 0;
    for (int i = 2; i < CHAIN_MAX_CHAINS; ++i)
    {
        LuxEchoState *o = lux_echo_instance(i);
        o->config.enabled = 1;
        atomic_store_explicit(&o->seen_px, 8192, memory_order_relaxed);   /* worst-case stride */
        granted += lux_echo_ensure_capacity(o, LUX_ECHO_MAX_DELAY, 8192);
    }
    printf("   %d of %d max-size instances granted, pool = %.0f MB\n", granted, CHAIN_MAX_CHAINS - 2,
           (double) lux_echo_pool_bytes() / 1048576.0);
    CHECK(granted < CHAIN_MAX_CHAINS - 2, "the budget refused at least one");
    CHECK(lux_echo_pool_bytes() <= LUX_ECHO_POOL_BUDGET, "pool under budget");
    /* The refused instance keeps no ring → its delay clamps to 0 capacity. */
    LuxEchoState *last = lux_echo_instance(CHAIN_MAX_CHAINS - 1);
    CHECK(lux_echo_wants_resync(last) == 0, "a refused request does not re-trigger the sync");
    (void) before;

    printf("7. shutdown frees everything\n");
    lux_echo_shutdown_all();
    CHECK(lux_echo_pool_bytes() == 0, "pool = %zu after shutdown", lux_echo_pool_bytes());
    CHECK(lux_echo_capacity(st) == 0, "capacity cleared");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
