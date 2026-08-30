/**
 * @file slp_rx_state.c
 * @brief Seqlock'd HID snapshot + reception statistics (see header).
 */
#include "slp_rx_state.h"
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ── clock ─────────────────────────────────────────────────────────────────── */
uint32_t slp_now_ms(void)
{
#ifdef _WIN32
    return (uint32_t) GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u);
#endif
}

/* ── HID snapshot (seqlock: odd seq = write in progress) ───────────────────── */
static struct
{
    uint32_t        seq;
    uint32_t        generation;
    uint32_t        last_ms;
    slp_hid_sample  sample;
} s_hid;

void slp_hid_publish(const slp_hid_sample *s)
{
    const uint32_t start = __atomic_load_n(&s_hid.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_hid.seq, start + 1u, __ATOMIC_RELEASE);      /* odd: writing */
    memcpy(&s_hid.sample, s, sizeof(*s));
    s_hid.generation++;
    s_hid.last_ms = slp_now_ms();
    __atomic_store_n(&s_hid.seq, start + 2u, __ATOMIC_RELEASE);      /* even: stable */
}

uint32_t slp_hid_read(slp_hid_sample *out)
{
    for (int attempt = 0; attempt < 8; attempt++)
    {
        const uint32_t s1 = __atomic_load_n(&s_hid.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)
            continue;                                              /* writer busy */
        slp_hid_sample tmp;
        memcpy(&tmp, &s_hid.sample, sizeof(tmp));
        const uint32_t gen = s_hid.generation;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t s2 = __atomic_load_n(&s_hid.seq, __ATOMIC_RELAXED);
        if (s1 == s2)
        {
            if (out) *out = tmp;
            return gen;
        }
    }
    return 0;   /* contended beyond reason: report "nothing new" */
}

uint32_t slp_hid_age_ms(void)
{
    const uint32_t last = __atomic_load_n(&s_hid.last_ms, __ATOMIC_RELAXED);
    if (last == 0u)
        return UINT32_MAX;
    return slp_now_ms() - last;
}

/* ── statistics ────────────────────────────────────────────────────────────── */
static slp_rx_stats s_stats;

void slp_rx_stats_writer(slp_rx_stats **out)
{
    *out = &s_stats;
}

void slp_rx_stats_snapshot(slp_rx_stats *out)
{
    /* Display-only: a torn read costs nothing more than a momentarily odd number. */
    memcpy(out, &s_stats, sizeof(*out));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}
