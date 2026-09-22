/* One message-thread publisher, multiple bounded, nonblocking readers. */
#include "chain_plan.h"
#include <string.h>
#include <stdatomic.h>
#include <sched.h>

static ChainPlan s_plans[3];
/* -1 reserves a slot for the writer; nonnegative values count pinned readers. */
static atomic_int s_readers[3];
static atomic_int s_current = -1;
static _Thread_local ChainPlan s_last_valid;

void chain_plan_publish(const ChainPlan* plan)
{
    if (!plan) return;
    for (;;) {
        const int current = atomic_load_explicit(&s_current, memory_order_acquire);
        for (int i = 0; i < 3; ++i) {
            if (i == current) continue;
            int expected = 0;
            if (!atomic_compare_exchange_strong_explicit(&s_readers[i], &expected, -1,
                    memory_order_acquire, memory_order_relaxed)) continue;
            s_plans[i] = *plan;
            atomic_store_explicit(&s_readers[i], 0, memory_order_release);
            atomic_store_explicit(&s_current, i, memory_order_release);
            return;
        }
        // Only the non-audio publisher may wait for a slot to be released.
        sched_yield();
    }
}

void chain_plan_get(ChainPlan* out)
{
    if (!out) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int i = atomic_load_explicit(&s_current, memory_order_acquire);
        if (i < 0) break;
        int readers = atomic_load_explicit(&s_readers[i], memory_order_relaxed);
        if (readers < 0) continue;
        if (!atomic_compare_exchange_strong_explicit(&s_readers[i], &readers, readers + 1,
                memory_order_acquire, memory_order_relaxed)) continue;
        *out = s_plans[i];
        atomic_fetch_sub_explicit(&s_readers[i], 1, memory_order_release);
        s_last_valid = *out;
        return;
    }
    // A coherent previous plan (initially empty), never an unprotected copy.
    *out = s_last_valid;
}
