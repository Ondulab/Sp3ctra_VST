/*
 * internal_source.c
 *
 * M9 — Internal SRC modules (IMAGE / VIDEO / CAMERA) line pool.
 * See internal_source.h for the threading model.
 *
 * Author: zhonx
 */
#include "internal_source.h"
#include "chain_plan.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    uint8_t         r[INTERNAL_SRC_MAX_PIXELS];
    uint8_t         g[INTERNAL_SRC_MAX_PIXELS];
    uint8_t         b[INTERNAL_SRC_MAX_PIXELS];
    int             pixel_count;   /* 0 = nothing published yet */
    int             active;
} InternalSource;

static InternalSource s_sources[INTERNAL_SRC_COUNT] = {
    { PTHREAD_MUTEX_INITIALIZER, {0}, {0}, {0}, 0, 0 },
    { PTHREAD_MUTEX_INITIALIZER, {0}, {0}, {0}, 0, 0 },
    { PTHREAD_MUTEX_INITIALIZER, {0}, {0}, {0}, 0, 0 },
};

/* Monotonic ms of the last completed live UDP line (0 = never). 64-bit atomic
 * via __atomic builtins — written by udpThread, read by the feeder thread. */
static int64_t s_last_live_ms = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void internal_source_set_active(int kind, int active)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT)
        return;
    pthread_mutex_lock(&s_sources[kind].mutex);
    s_sources[kind].active = active ? 1 : 0;
    pthread_mutex_unlock(&s_sources[kind].mutex);
}

int internal_source_is_active(int kind)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT)
        return 0;
    pthread_mutex_lock(&s_sources[kind].mutex);
    int a = s_sources[kind].active;
    pthread_mutex_unlock(&s_sources[kind].mutex);
    return a;
}

int internal_source_any_active(void)
{
    for (int k = 0; k < INTERNAL_SRC_COUNT; ++k)
        if (internal_source_is_active(k))
            return 1;
    return 0;
}

void internal_source_publish(int kind,
                             const uint8_t *r, const uint8_t *g, const uint8_t *b,
                             int n)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT || !r || !g || !b || n <= 0)
        return;
    if (n > INTERNAL_SRC_MAX_PIXELS)
        n = INTERNAL_SRC_MAX_PIXELS;

    InternalSource *s = &s_sources[kind];
    pthread_mutex_lock(&s->mutex);
    memcpy(s->r, r, (size_t) n);
    memcpy(s->g, g, (size_t) n);
    memcpy(s->b, b, (size_t) n);
    s->pixel_count = n;
    pthread_mutex_unlock(&s->mutex);
}

int internal_source_copy(int kind, uint8_t *r, uint8_t *g, uint8_t *b,
                         int max_pixels)
{
    if (kind < 0 || kind >= INTERNAL_SRC_COUNT || !r || !g || !b || max_pixels <= 0)
        return 0;
    if (max_pixels > INTERNAL_SRC_MAX_PIXELS)
        max_pixels = INTERNAL_SRC_MAX_PIXELS;

    InternalSource *s = &s_sources[kind];
    pthread_mutex_lock(&s->mutex);
    const int n = s->pixel_count;
    if (!s->active || n <= 0)
    {
        pthread_mutex_unlock(&s->mutex);
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
    pthread_mutex_unlock(&s->mutex);
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
