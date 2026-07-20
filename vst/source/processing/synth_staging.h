/*
 * synth_staging.h
 *
 * Synth-split P3 — per-send staging + N-way mix for the LuxStral engine.
 *
 * Every "→ LUXSTRAL" send (one per chain, up to CHAIN_MAX_CHAINS) is executed
 * by its producer thread (udpThread / feeder tick / FramePlayerThread), which
 * stages the send's CONDITIONED frame here: per-note amplitudes (post
 * inverse-dB law), stereo pan gains and the contrast factor. The audio thread
 * then pulls the intensity-weighted MIX of every active send and commits it
 * as the single engine feed (db->preprocessed_data) — one writer per staging
 * slot, one mixer per engine, no cross-thread contention.
 *
 * Slots are indexed by MODEL CHAIN (0..CHAIN_MAX_CHAINS-1), which is stable
 * across plan republishes; the send's conditioning bank (bank_slot) is stored
 * at publish time. A seqlock per slot keeps producers wait-free; the mixer
 * retries the rare torn read.
 *
 * Mix laws (single send at intensity 1.0 = bit-exact parity):
 *   notes[i]    = Σ_k  w_k · notes_k[i]                w_k = intensity_k
 *   pan gains   = Σ_k  w_k·notes_k[i]·gain_k[i] / Σ_k w_k·notes_k[i]
 *                 (the louder contributor drives the note's pan; centre when
 *                  the note is silent everywhere)
 *   contrast    = Σ_k  w_k·contrast_k / Σ_k w_k
 * Disabled sends (bank .enabled == 0) and inactive slots contribute nothing.
 */
#ifndef SYNTH_STAGING_H
#define SYNTH_STAGING_H

#include "image_preprocessor.h"
#include "chain_plan.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init (idempotent) — zeroes every slot. */

/* Producer (single writer per chain slot): stage one conditioned send frame.
 * Copies additive.notes (num_notes), stereo gains when stereo_valid, and the
 * contrast factor from `pp`. Marks the slot active. */
void synth_staging_stage_luxstral(int chain_idx, int bank_slot,
                                  const PreprocessedImageData* pp,
                                  int num_notes, int stereo_valid);

/* Producer: the send currently produces NO signal (chain no-signal contract)
 * — the slot contributes silence to the mix until staged again. */
void synth_staging_set_inactive(int chain_idx);

/* Consumer (audio thread): mix every active send listed in `plan->ls_send`.
 * Fills notes_out[max_notes] (also usable as the display grayscale — with
 * pixels_per_note == 1 they are the same axis), left/right gain arrays and
 * the blended contrast factor. Weights come from
 * g_sp3ctra_config.luxstral_out[bank_slot] (intensity × enabled).
 * Returns the number of sends actually mixed; 0 → the caller must commit
 * silence; -1 → a slot was torn by a concurrent staging (writer mid-copy on
 * every retry) — the caller must HOLD its previous commit untouched, the
 * outputs are undefined. stereo_valid_out is 1 when at least one mixed send
 * carried pan gains (silent-note fallback = constant-power centre). */
int synth_staging_mix_luxstral(const ChainPlan* plan,
                               float* notes_out, int max_notes,
                               float* left_out, float* right_out,
                               float* contrast_out, int* stereo_valid_out);

/* ── M4 — LuxSynth sends (conditioned LINE + raw RGB at the OUT position) ──
 * Producers stage the send's conditioned grayscale line (luxsynth_condition_
 * line, WITHOUT intensity) plus the raw RGB stream at the OUT marker (colour
 * data for harmonicity). The engine-feed consumer (luxsynth_feed_tick, audio
 * thread) pulls the intensity-weighted mix and runs ONE FFT. */
void synth_staging_stage_luxsynth(int chain_idx, int bank_slot,
                                  const float* line,
                                  const uint8_t* r, const uint8_t* g,
                                  const uint8_t* b, int nb_pixels);

void synth_staging_luxsynth_set_inactive(int chain_idx);

/* Consumer: mix every staged LuxSynth send of a chain whose plan recipe
 * carries an OUT_LUXSYNTH marker. line_out = clamp01(Σ w·line_k), RGB =
 * w-weighted average (harmonicity/colour). Returns the number of sends mixed
 * (0 → silence, -1 → torn slot: HOLD the engine's spectrum, outputs
 * undefined). generation_out (may be NULL) receives a counter that changes
 * whenever any contributing slot was restaged — cheap dirty check. */
int synth_staging_mix_luxsynth(const ChainPlan* plan,
                               float* line_out,
                               uint8_t* r_out, uint8_t* g_out, uint8_t* b_out,
                               int max_pixels, int* nb_pixels_out,
                               uint32_t* generation_out);

/* ── M5 — LuxWave sends (conditioned wavetable LINE at the OUT position) ───
 * Producers stage the send's conditioned line (luxwave_condition_line,
 * WITHOUT intensity). The consumer (pipeline_luxwave_feed_tick) pulls the
 * BIPOLAR mix around the wavetable midpoint:
 *   mixed[i] = clamp01(0.5 + Σ w_k·(line_k[i] − 0.5))     w_k = intensity_k
 * (one send at intensity 1 = bit-exact parity; intensity 0 = flat 0.5 =
 * true wavetable silence). Plan-gated on the OUT_LUXWAVE marker. */
void synth_staging_stage_luxwave(int chain_idx, int bank_slot,
                                 const float* line, int nb_pixels);

void synth_staging_luxwave_set_inactive(int chain_idx);

/* Same return contract as the other mixers: N mixed, 0 = no send (the
 * wavetable holds anyway), -1 = torn slot (hold, outputs undefined). */
int synth_staging_mix_luxwave(const ChainPlan* plan,
                              float* line_out, int max_pixels,
                              int* nb_pixels_out);

/* ── LuxGrain sends (conditioned LINE + raw RGB at the OUT position) ───────
 * Producers stage the send's conditioned line (luxgrain_condition_line,
 * WITHOUT intensity) plus the raw RGB stream at the OUT marker (colour →
 * per-cell grain pan). The consumer (luxgrain_feed_tick) pulls the unipolar
 * mix clamp01(Σ w·line_k) + weight-averaged RGB and folds them into the
 * granular engine's band cells. Plan-gated on the OUT_LUXGRAIN marker.
 * generation_out mirrors the LuxSynth contract (dirty check). */
void synth_staging_stage_luxgrain(int chain_idx, int bank_slot,
                                  const float* line,
                                  const uint8_t* r, const uint8_t* g,
                                  const uint8_t* b, int nb_pixels);

void synth_staging_luxgrain_set_inactive(int chain_idx);

/* Same return contract as the other mixers: N mixed, 0 = no send,
 * -1 = torn slot (hold, outputs undefined). r/g/b_out may be NULL. */
int synth_staging_mix_luxgrain(const ChainPlan* plan,
                               float* line_out,
                               uint8_t* r_out, uint8_t* g_out, uint8_t* b_out,
                               int max_pixels, int* nb_pixels_out,
                               uint32_t* generation_out);

/* Diagnostic: total mixer ticks that HELD on a torn slot (all three mixers).
 * Monotonic, process-lifetime. Message-thread drain (PluginProcessor timer)
 * — a steadily climbing value under device streaming confirms staging
 * contention (was audible as LuxSynth micro-dropouts before the hold). */
uint64_t synth_staging_contention_holds(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_STAGING_H */
