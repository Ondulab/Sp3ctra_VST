#include "pipeline_metrics.h"
/* Separate cache lines: unrelated producers never share a counter's cache line.
 * Multiple publishers are allowed during chain ownership handoffs. */
typedef struct { _Alignas(128) uint64_t count; } Counter;
static Counter counters[PIPE_METRIC_COUNT];
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "metrics require lock-free counters");
void pipeline_metric_hit(unsigned metric)
{
    if (metric < PIPE_METRIC_COUNT)
        __atomic_fetch_add(&counters[metric].count, 1, __ATOMIC_RELAXED);
}
void pipeline_metrics_read(uint64_t out[PIPE_METRIC_COUNT])
{
    for (unsigned i = 0; i < PIPE_METRIC_COUNT; ++i)
        out[i] = __atomic_load_n(&counters[i].count, __ATOMIC_RELAXED);
}
