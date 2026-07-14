/*
 * internal_source.c
 *
 * M9 / P5-M2 — Internal SRC modules (IMAGE / VIDEO / CAMERA) line pool,
 * one line PER MODULE INSTANCE: (kind, slot 0..INTERNAL_SRC_SLOTS-1).
 * See internal_source.h for the threading + broadcast model.
 *
 * Author: zhonx
 */
#include "internal_source.h"
#include "chain_plan.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t r[INTERNAL_SRC_MAX_PIXELS];
    uint8_t g[INTERNAL_SRC_MAX_PIXELS];
    uint8_t b[INTERNAL_SRC_MAX_PIXELS];
    int     pixel_count;   /* 0 = nothing published yet */
    int     active;
} InternalSource;

/* One mutex for the whole pool: every caller is Non-RT and the critical
 * sections are a few memcpy of one line — contention is negligible. */
static pthread_mutex_t s_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static InternalSource  s_sources[INTERNAL_SRC_COUNT][INTERNAL_SRC_SLOTS];

/* Monotonic ms of the last completed live UDP line (0 = never). 64-bit atomic
 * via __atomic builtins — written by udpThread, read by the feeder thread. */
static int64_t s_last_live_ms = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void erase_locked(InternalSource *s)
{
    /* Deactivation ERASES the published content (module removed from its
     * chain, media unloaded, source disabled): nothing may resurrect the old
     * line on re-activation — the producer must publish a fresh one. White
     * fill = the blank-paper contract (defensive; pixel_count == 0 already
     * blocks reads). */
    s->pixel_count = 0;
    memset(s->r, 0xFF, sizeof(s->r));
    memset(s->g, 0xFF, sizeof(s->g));
    memset(s->b, 0xFF, sizeof(s->b));
}

void internal_source_set_active(int kind, int slot, int active)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT || slot >= INTERNAL_SRC_SLOTS)
        return;
    pthread_mutex_lock(&s_pool_mutex);
    const int lo = (slot < 0) ? 0 : slot;
    const int hi = (slot < 0) ? INTERNAL_SRC_SLOTS - 1 : slot;
    for (int sl = lo; sl <= hi; ++sl)
    {
        InternalSource *s = &s_sources[kind][sl];
        s->active = active ? 1 : 0;
        if (!active)
            erase_locked(s);
    }
    pthread_mutex_unlock(&s_pool_mutex);
}

int internal_source_is_active(int kind, int slot)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT || slot >= INTERNAL_SRC_SLOTS)
        return 0;
    pthread_mutex_lock(&s_pool_mutex);
    int a = 0;
    if (slot < 0)
    {
        for (int sl = 0; sl < INTERNAL_SRC_SLOTS && !a; ++sl)
            a = s_sources[kind][sl].active;
    }
    else
        a = s_sources[kind][slot].active;
    pthread_mutex_unlock(&s_pool_mutex);
    return a;
}

int internal_source_any_active(void)
{
    for (int k = 0; k < INTERNAL_SRC_COUNT; ++k)
        if (internal_source_is_active(k, -1))
            return 1;
    return 0;
}

void internal_source_publish(int kind, int slot,
                             const uint8_t *r, const uint8_t *g, const uint8_t *b,
                             int n)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT || slot >= INTERNAL_SRC_SLOTS
        || !r || !g || !b || n <= 0)
        return;
    if (n > INTERNAL_SRC_MAX_PIXELS)
        n = INTERNAL_SRC_MAX_PIXELS;

    pthread_mutex_lock(&s_pool_mutex);
    const int lo = (slot < 0) ? 0 : slot;
    const int hi = (slot < 0) ? INTERNAL_SRC_SLOTS - 1 : slot;
    for (int sl = lo; sl <= hi; ++sl)
    {
        InternalSource *s = &s_sources[kind][sl];
        if (slot < 0 && !s->active)
            continue;   /* broadcast targets ACTIVE slots only */
        memcpy(s->r, r, (size_t) n);
        memcpy(s->g, g, (size_t) n);
        memcpy(s->b, b, (size_t) n);
        s->pixel_count = n;
    }
    pthread_mutex_unlock(&s_pool_mutex);
}

int internal_source_copy(int kind, int slot,
                         uint8_t *r, uint8_t *g, uint8_t *b,
                         int max_pixels)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT
        || slot < 0 || slot >= INTERNAL_SRC_SLOTS
        || !r || !g || !b || max_pixels <= 0)
        return 0;
    if (max_pixels > INTERNAL_SRC_MAX_PIXELS)
        max_pixels = INTERNAL_SRC_MAX_PIXELS;

    InternalSource *s = &s_sources[kind][slot];
    pthread_mutex_lock(&s_pool_mutex);
    const int n = s->pixel_count;
    if (!s->active || n <= 0)
    {
        pthread_mutex_unlock(&s_pool_mutex);
        return 0;
    }
    if (n == max_pixels)
    {
        memcpy(r, s->r, (size_t) n);
        memcpy(g, s->g, (size_t) n);
        memcpy(b, s->b, (size_t) n);
    }
    else
    {
        /* Width mismatch (e.g. engine published 3456 while the device runs at
         * 1728 DPI): nearest-neighbour resample to the requested width. */
        for (int i = 0; i < max_pixels; ++i)
        {
            const int j = (int) (((int64_t) i * n) / max_pixels);
            r[i] = s->r[j];
            g[i] = s->g[j];
            b[i] = s->b[j];
        }
    }
    pthread_mutex_unlock(&s_pool_mutex);
    return max_pixels;
}

void internal_source_note_live_line(void)
{
    __atomic_store_n(&s_last_live_ms, now_ms(), __ATOMIC_RELAXED);
}

int internal_source_live_streaming(void)
{
    const int64_t last = __atomic_load_n(&s_last_live_ms, __ATOMIC_RELAXED);
    if (last == 0)
        return 0;
    return (now_ms() - last) < 250;
}

int internal_source_kind_for_chain_src(int chain_src_kind)
{
    switch (chain_src_kind)
    {
        case CHAIN_SRC_IMAGE:  return INTERNAL_SRC_IMAGE;
        case CHAIN_SRC_VIDEO:  return INTERNAL_SRC_VIDEO;
        case CHAIN_SRC_CAMERA: return INTERNAL_SRC_CAMERA;
        default:               return -1;
    }
}
