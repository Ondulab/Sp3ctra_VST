/*
 * luxsynth_feed.h
 *
 * Synth-split M4 (+ D2) — core-side LuxSynth engine feed.
 *
 * Replaces the UI-thread spectral bridge (CisVisualizerComponent::
 * computeFftMagnitudes → luxsynth_engine_set_spectral_data): the engine now
 * hears the staged "→ LUXSYNTH" send mix even with the editor closed.
 *
 * Flow (audio thread, once per render iteration):
 *   synth_staging_mix_luxsynth  (Σ intensity·conditioned line, weighted RGB)
 *   → Hann window → ONE real FFT → peak-normalise → temporal smoothing
 *   (lx_fft_smoothing, dt-corrected to the historical 30 fps tuning)
 *   → per-bin harmonicity from the mixed RGB colour temperature
 *   → luxsynth_engine_set_spectral_data.
 *
 * Gates: Chain-2 transport (image/raw freeze: PLAY compute, HOLD keep last,
 * STOP silence) and the no-send contract (0 active sends → silence). A
 * generation counter skips the FFT when nothing was restaged.
 */
#ifndef LUXSYNTH_FEED_H
#define LUXSYNTH_FEED_H

#include "chain_plan.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio thread (audioProcessingThread): pull the send mix and refresh the
 * LuxSynth engine's spectral data. Cheap no-op when nothing changed. */
void luxsynth_feed_tick(const ChainPlan* plan);

/* Dropout diagnostics (monotonic, process lifetime; message-thread drain):
 * how many times the feed pushed SILENCE (no-send contract) vs real spectra. */
uint64_t luxsynth_feed_silence_pushes(void);
uint64_t luxsynth_feed_spec_pushes(void);

#ifdef __cplusplus
}
#endif

#endif /* LUXSYNTH_FEED_H */
