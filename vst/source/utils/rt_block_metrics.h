#ifndef RT_BLOCK_METRICS_H
#define RT_BLOCK_METRICS_H
#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RT_BLOCK_RING_SIZE 8192u
#define RT_BLOCK_HIST_SIZE 65536u
/* Single audio writer / single diagnostics reader. No allocation or retry. */
typedef struct { uint64_t elapsed_ns, budget_ns; } RTBlockSample;
typedef struct {
    RTBlockSample samples[RT_BLOCK_RING_SIZE];
    atomic_uint write_pos, read_pos;
    atomic_uint_fast64_t dropped;
    /* The following state belongs exclusively to the diagnostics reader. */
    uint64_t histogram[RT_BLOCK_HIST_SIZE];
    uint64_t count, total_ns, max_ns, deadlines, overflows;
    int64_t min_headroom_ns;
} RTBlockMetrics;
void rt_block_record(RTBlockMetrics*, uint64_t elapsed_ns, uint64_t budget_ns);
void rt_block_drain(RTBlockMetrics*);
uint64_t rt_block_percentile_us(const RTBlockMetrics*, unsigned permille);
void rt_block_reset_window(RTBlockMetrics*);
#ifdef __cplusplus
}
#endif
#endif
