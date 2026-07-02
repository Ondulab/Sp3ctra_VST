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
 * Maps: invert_intensity, additive_gamma_value, image_freeze_mode,
 *       image_fade_in_ms, image_live_opacity, additive_contrast_min, etc.
 */
PipelineConfig pipeline_build_config_live(void);

/**
 * @brief Build a PipelineConfig for the SAMPLER stream from g_sp3ctra_config.
 * Maps: invert_intensity, sampler_gamma, sampler_freeze_mode,
 *       sampler_fade_in_ms, image_sampler_opacity, sampler_contrast_min, etc.
 */
PipelineConfig pipeline_build_config_sampler(void);

/**
 * @brief Build a PipelineConfig for the 2nd LuxStral engine (B, M8).
 * Live config with engine B's OWN image/stereo knobs (luxstral_b_* mirror of
 * the luxstralB* APVTS params) and its OWN freeze-envelope state.
 */
PipelineConfig pipeline_build_config_luxstral_b(void);

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
