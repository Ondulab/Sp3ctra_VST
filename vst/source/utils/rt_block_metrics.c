#include "rt_block_metrics.h"
#include <string.h>
#include <limits.h>
void rt_block_record(RTBlockMetrics* m, uint64_t elapsed, uint64_t budget)
{
    const unsigned wr = atomic_load_explicit(&m->write_pos, memory_order_relaxed);
    const unsigned rd = atomic_load_explicit(&m->read_pos, memory_order_acquire);
    if (wr - rd == RT_BLOCK_RING_SIZE) {
        atomic_fetch_add_explicit(&m->dropped, 1, memory_order_relaxed);
        return;
    }
    m->samples[wr & (RT_BLOCK_RING_SIZE - 1)] = (RTBlockSample){elapsed, budget};
    atomic_store_explicit(&m->write_pos, wr + 1, memory_order_release);
}
void rt_block_drain(RTBlockMetrics* m)
{
    unsigned rd = atomic_load_explicit(&m->read_pos, memory_order_relaxed);
    const unsigned wr = atomic_load_explicit(&m->write_pos, memory_order_acquire);
    while (rd != wr) {
        const RTBlockSample s = m->samples[rd & (RT_BLOCK_RING_SIZE - 1)];
        const uint64_t us = s.elapsed_ns / 1000;
        ++m->histogram[us < RT_BLOCK_HIST_SIZE ? us : RT_BLOCK_HIST_SIZE - 1];
        if (us >= RT_BLOCK_HIST_SIZE) ++m->overflows;
        const int64_t headroom = (int64_t)s.budget_ns - (int64_t)s.elapsed_ns;
        if (m->count == 0 || headroom < m->min_headroom_ns) m->min_headroom_ns = headroom;
        ++m->count;
        m->total_ns += s.elapsed_ns;
        if (s.elapsed_ns > m->max_ns) m->max_ns = s.elapsed_ns;
        if (s.budget_ns > 0 && s.elapsed_ns > s.budget_ns) ++m->deadlines;
        ++rd;
    }
    atomic_store_explicit(&m->read_pos, rd, memory_order_release);
}
uint64_t rt_block_percentile_us(const RTBlockMetrics* m, unsigned permille)
{
    if (!m->count) return 0;
    if (permille > 1000) permille = 1000;
    uint64_t rank = (m->count * permille + 999) / 1000;
    if (!rank) rank = 1;
    uint64_t accumulated = 0;
    for (unsigned i = 0; i < RT_BLOCK_HIST_SIZE; ++i) {
        accumulated += m->histogram[i];
        if (accumulated >= rank) return i;
    }
    return RT_BLOCK_HIST_SIZE - 1;
}
void rt_block_reset_window(RTBlockMetrics* m)
{
    memset(m->histogram, 0, sizeof(m->histogram));
    m->count = m->total_ns = m->max_ns = m->deadlines = m->overflows = 0;
    m->min_headroom_ns = INT64_MAX;
}
