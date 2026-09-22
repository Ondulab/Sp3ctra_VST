#ifndef SP3CTRA_PIPELINE_METRICS_H
#define SP3CTRA_PIPELINE_METRICS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Shared-core, cumulative event counts. No timer, allocation, reset, or logging
 * on producers. Each UI reader computes its own deltas against elapsed time.
 * CHAIN counts only walks reaching the end, not each ownership span.
 * SEND counts conditioned lines published by chain to engine staging.
 * FEED counts successful non-empty mix publications toward synthesis, including
 * repeated data for LuxStral/LuxWave; Synth/Grain skip unchanged generations. */
enum {
    PIPE_RX = 0,
    PIPE_CHAIN = 1,                  /* + chain, 8 entries */
    PIPE_SEND = PIPE_CHAIN + 8,      /* + engine * 8 + chain, 32 entries */
    PIPE_FEED = PIPE_SEND + 32,      /* + engine, 4 entries */
    PIPE_METRIC_COUNT = PIPE_FEED + 4
};
void pipeline_metric_hit(unsigned metric);
void pipeline_metrics_read(uint64_t out[PIPE_METRIC_COUNT]);
#ifdef __cplusplus
}
#endif
#endif
