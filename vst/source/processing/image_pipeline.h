/*
 * image_pipeline.h
 *
 * Pipeline orchestrator for the dual-path image processing system.
 * This is the single entry point replacing image_preprocess_frame() and
 * image_preprocess_lux_sampler().
 *
 * Paths:
 *   Path A (LuxStral):          route → gray → contrast → invert → ac_remove → gamma
 *                                → average → pan → blob → freeze/fade envelope
 *   Path B (LuxSynth+LuxWave):  route → gray_luxsynth → fft → color_fft → copy_rgb
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#ifndef IMAGE_PIPELINE_H
#define IMAGE_PIPELINE_H

#include "image_pipeline_types.h"
#include "image_preprocessor.h"
#include "chain_plan.h"        /* ChainPlan (M5 — LuxWave feed tick) */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/** @brief Initialize the pipeline (delegates to FFT lazy-init flag reset). */
void pipeline_init(void);

/** @brief Cleanup pipeline resources. */
void pipeline_cleanup(void);

/* ============================================================================
 * Main entry point
 * ============================================================================ */

/**
 * @brief Process one frame through the dual-path pipeline.
 *
 * Replaces image_preprocess_frame() and image_preprocess_lux_sampler().
 * Includes freeze/opacity/fade envelope for the LuxStral path.
 *
 * @param raw_r, raw_g, raw_b  Input RGB channels [0-255]
 * @param config                Pipeline configuration (routing, toggles, envelope)
 * @param out                   Output (backward-compatible PreprocessedImageData)
 * @return                      0 on success, -1 on error
 */
int pipeline_process_frame(
    const uint8_t         *raw_r,
    const uint8_t         *raw_g,
    const uint8_t         *raw_b,
    const PipelineConfig  *config,
    PreprocessedImageData *out
);

/* ============================================================================
 * Convenience builders — create PipelineConfig from g_sp3ctra_config
 * ============================================================================ */

/**
 * @brief Build a PipelineConfig for the LIVE stream from g_sp3ctra_config.
 * Maps: luxstral_out[0]/luxsynth_out[0] conditioning banks, image_freeze_mode,
 *       image_fade_in_ms, image_live_opacity, stereo, etc.
 */
PipelineConfig pipeline_build_config_live(void);

/**
 * @brief Build a PipelineConfig for the SAMPLER stream from g_sp3ctra_config.
 * Maps: sampler_gamma, sampler_freeze_mode, sampler_fade_in_ms,
 *       image_sampler_opacity, sampler_contrast_min, etc.
 */
PipelineConfig pipeline_build_config_sampler(void);

/**
 * @brief M5 — LuxWave per-send conditioning: grayscale + Negative / DC
 * Blocking / Gamma from the luxwave_out bank `bank_slot`, WITHOUT intensity
 * (the bipolar mix applies it as the send weight). line_out = nb_pixels floats.
 */
void luxwave_condition_line(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int bank_slot,
    float *line_out,
    int nb_pixels);

/**
 * @brief M5 — audio-thread LuxWave wavetable feed: pull the bipolar mix of
 * the staged "→ LUXWAVE" sends, apply the Chain-2 transport envelope
 * (ENVELOPE_LUXWAVE) and push the wavetable line. No active send → no push
 * (the wavetable keeps its last content; it only sounds under MIDI).
 */
void pipeline_luxwave_feed_tick(const ChainPlan *plan);

/**
 * @brief Synth-split P3 — config for ONE LuxStral send (N-chain mix).
 * Engine-A shape with the send's conditioning bank (luxstral_out[bank_slot])
 * and a per-send envelope state (chain-indexed). player_fed = 1 when the
 * FramePlayerThread drives the send (its freeze_mode stays authoritative).
 * Per-frame intensity is forced to 1.0 — the audio mixer applies the bank's
 * intensity as the mix weight (synth_staging_mix_luxstral).
 */
PipelineConfig pipeline_build_config_ls_send(int bank_slot, int chain_idx,
                                             int player_fed);

/* ============================================================================
 * Per-path processing (used internally and available for testing)
 * ============================================================================ */

void pipeline_path_luxstral(
    const uint8_t        *raw_r,
    const uint8_t        *raw_g,
    const uint8_t        *raw_b,
    const PipelineConfig *config,
    PreprocessedImageData *out
);

void pipeline_path_luxsynth_luxwave(
    const uint8_t  *raw_r,
    const uint8_t  *raw_g,
    const uint8_t  *raw_b,
    const PipelineConfig *config,
    PreprocessedImageData *out
);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PIPELINE_H */
