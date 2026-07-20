/*
 * luxgrain_feed.h
 *
 * LuxGrain engine feed — pulls the staged "→ LUXGRAIN" send mix and folds
 * it into the granular engine's band-cell staging (the engine latches at
 * its own block start). Called from audioProcessingThread, right next to
 * luxsynth_feed_tick.
 *
 * Contracts (same doctrine as luxsynth_feed):
 *   • torn mix (-1)  → HOLD: the engine keeps its ring untouched;
 *   • no send (0)    → debounced (~100 ms) history WIPE — a transient
 *     inactive flicker must not kill the cloud (micro-coupure lesson);
 *   • unchanged generation → no re-stage (the ring holds, zero cost).
 */
#ifndef LUXGRAIN_FEED_H
#define LUXGRAIN_FEED_H

#include "chain_plan.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void luxgrain_feed_tick(const ChainPlan* plan);

/* Dropout diagnostics (message-thread drain). */
uint64_t luxgrain_feed_silence_pushes(void);
uint64_t luxgrain_feed_line_pushes(void);

#ifdef __cplusplus
}
#endif

#endif /* LUXGRAIN_FEED_H */
