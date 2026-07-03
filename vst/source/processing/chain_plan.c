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
    atomic_store_explicit(&s_plan_seq, seq + 1, memory_order_relaxed); /* odd: write in progress */
    /* Full release FENCE (not just a release store): the odd marker must be
     * visible BEFORE the plan writes below — a release store only orders the
     * accesses PRECEDING it, so plan writes could otherwise be hoisted above
     * the odd store on weakly-ordered CPUs and a reader could validate a
     * half-written plan against two equal even sequences. */
    atomic_thread_fence(memory_order_release);
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
    /* Bounded retry: the writer (message thread) can be preempted mid-publish
     * while a HIGH priority reader spins — never let an RT thread spin
     * unbounded on it. After the cap, return the last copy as-is: worst case
     * one possibly-mixed plan for one frame (indices stay bounded — same
     * degradation as the pre-seqlock double buffer), never a stall. */
    for (int tries = 0; tries < 1000; ++tries)
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
    *out = s_plan;                              /* degraded fallback (see above) */
}
