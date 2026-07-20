/*
 * luxgrain_vst_adapter.h
 *
 * VST integration layer for the LuxGrain granular engine. LuxGrain follows
 * the INLINE pattern (like LuxWave/LuxSynth today): the engine renders
 * directly in processBlock — no dedicated thread, no double-buffer. This
 * adapter only owns the global engine instance and its output scratch.
 *
 * Threading contract (all lock-free, engine-internal seqlocks):
 *   • audioProcessingThread → luxgrain_feed_tick → engine_stage_line
 *   • JUCE audio thread     → engine_set_config + engine_process (latches)
 */

#ifndef LUXGRAIN_VST_ADAPTER_H
#define LUXGRAIN_VST_ADAPTER_H

#include "synth_luxgrain_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern LuxGrainEngine g_luxgrain_engine;

/* processBlock render scratch (audio thread only). */
extern float g_luxgrain_out_l[LUXGRAIN_MAX_BUFFER_SIZE];
extern float g_luxgrain_out_r[LUXGRAIN_MAX_BUFFER_SIZE];

#ifdef __cplusplus
}
#endif

#endif /* LUXGRAIN_VST_ADAPTER_H */
