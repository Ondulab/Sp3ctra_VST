/*
 * chain_plan.c — see chain_plan.h.
 *
 * Seqlock: one producer (message thread, chain_plan_publish) bumps the
 * sequence to ODD, writes the plan, bumps back to EVEN; consumers (the UDP
 * thread AND audioProcessingThread — two independent readers) retry the copy
 * until they observe the same even sequence on both sides. Publishes are rare
 * (user edits only) and the plan is tiny, so reader retries are practically
 * nonexistent — but unlike the previous double buffer, a copy can never be
 * torn by two publishes landing during one read.
 *
 * Author: zhonx
 */
#include "chain_plan.h"

#include <string.h>
#include <stdatomic.h>

static ChainPlan       s_plan;                 /* guarded by s_plan_seq */
static _Atomic uint32_t s_plan_seq   = 0;      /* even = stable, odd = writing */
static _Atomic int      s_plan_valid = 0;

void chain_plan_publish(const ChainPlan* plan)
{
    const uint32_t seq = atomic_load_explicit(&s_plan_seq, memory_order_relaxed);
    atomic_store_explicit(&s_plan_seq, seq + 1, memory_order_release); /* odd: write in progress */
    s_plan = *plan;
    atomic_store_explicit(&s_plan_seq, seq + 2, memory_order_release); /* even: stable */
    atomic_store_explicit(&s_plan_valid, 1, memory_order_release);
}

void chain_plan_get(ChainPlan* out)
{
    if (! atomic_load_explicit(&s_plan_valid, memory_order_acquire))
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    for (;;)
    {
        const uint32_t before = atomic_load_explicit(&s_plan_seq, memory_order_acquire);
        if (before & 1u)
            continue;                           /* writer mid-publish — retry */
        *out = s_plan;
        atomic_thread_fence(memory_order_acquire);
        const uint32_t after = atomic_load_explicit(&s_plan_seq, memory_order_relaxed);
        if (before == after)
            return;                             /* consistent snapshot */
    }
}
