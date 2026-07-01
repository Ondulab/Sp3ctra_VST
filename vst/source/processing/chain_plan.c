/*
 * chain_plan.c — see chain_plan.h.
 *
 * Lock-free double buffer: one producer (message thread, chain_plan_publish)
 * writes the back buffer then flips an atomic index; one consumer (synth thread,
 * chain_plan_get) reads the front index and copies the whole plan. Publishes are
 * rare (user edits only), the plan is tiny, so a torn read would require two
 * flips during one copy — and even then it would only mis-route image data for a
 * single frame, never crash.
 *
 * Author: zhonx
 */
#include "chain_plan.h"

#include <string.h>
#include <stdatomic.h>

static ChainPlan   s_plan_buf[2];
static _Atomic int s_plan_idx   = 0;   /* index of the front (published) buffer */
static _Atomic int s_plan_valid = 0;

void chain_plan_publish(const ChainPlan* plan)
{
    const int front = atomic_load_explicit(&s_plan_idx, memory_order_relaxed);
    const int back  = front ^ 1;
    s_plan_buf[back] = *plan;
    atomic_store_explicit(&s_plan_idx, back, memory_order_release);
    atomic_store_explicit(&s_plan_valid, 1, memory_order_release);
}

void chain_plan_get(ChainPlan* out)
{
    if (! atomic_load_explicit(&s_plan_valid, memory_order_acquire))
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    const int front = atomic_load_explicit(&s_plan_idx, memory_order_acquire);
    *out = s_plan_buf[front];
}
