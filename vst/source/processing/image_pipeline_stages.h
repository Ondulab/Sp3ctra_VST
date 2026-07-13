/*
 * image_pipeline_stages.h
 *
 * Atomic, stateless processing stage functions for the image pipeline.
 * Each function performs one well-defined transformation step.
 *
 * These stages are composed into two parallel paths by image_pipeline.c:
 *   Path A (LuxStral):        invert → ac_remove → gamma → pan_luxstral → gray_luxstral → blob
 *   Path B (LuxSynth+LuxWave): invert → ac_remove → (no gamma) → gray_luxsynth → pan_luxsynth
 *                               → fft_gray → fft_pan → blob → copy_rgb
 *
 * RT-safety: All functions are pure C, allocation-free, and bounded O(N).
 *            No JUCE dependencies. No global state access (except noted).
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#ifndef IMAGE_PIPELINE_STAGES_H
#define IMAGE_PIPELINE_STAGES_H

#include "image_pipeline_types.h"
#include "image_preprocessor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RGB → Grayscale conversion
 * Uses ITU-R BT.601 weights: 0.299R + 0.587G + 0.114B
 * Output range: [0.0, 1.0]
 * ============================================================================ */
void img_stage_rgb_to_grayscale(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    float         *grayscale_out
);

/* ============================================================================
 * Inversion: pixel = 1.0 - pixel
 * Operates in-place on a float [0.0, 1.0] buffer.
 * ============================================================================ */
void img_stage_invert(float *pixels, int count);

/* ============================================================================
 * AC Removal (DC offset subtraction): pixel = pixel - mean(pixels)
 * Removes the per-line average, leaving only AC (variation) content.
 * Result is clamped to [0.0, 1.0].
 * ============================================================================ */
void img_stage_remove_dc(float *pixels, int count);

/* ============================================================================
 * Gamma correction: pixel = powf(pixel, gamma)
 * Input must be in [0.0, 1.0]. Gamma = 0.0 means bypass (no-op).
 * Includes NaN/Inf protection.
 * ============================================================================ */
void img_stage_apply_gamma(float *pixels, int count, float gamma);

/* ============================================================================
 * Inverse-dB decode (SCORE dB decode law): exact inverse of the SCORE
 * encoder's brightness map (score_engine.c: intensity linear in dB over a
 * range_db window, white = silence).
 *
 *   input  x : ink density in [0,1] (grayscale AFTER inversion)
 *   output   : amplitude = 10^((x − 1) · range_db / 20)
 *              x ≤ half a grey quantum → 0 (true silence: the encoder maps
 *              everything at/below its dB floor to pure white)
 *
 * Runs on the LuxStral path after img_stage_apply_gamma — ALWAYS ON (single
 * decode chain, gamma 1.0 = pure dB decode); no other stage is forced.
 * range_db must match the encoder's dynamicRangeDB (default 50).
 * ============================================================================ */
void img_stage_apply_db_decode(float *pixels, int count, float range_db);

/* ============================================================================
 * Contrast calculation — measures pixel variance for volume modulation.
 * Must be called on RAW grayscale (before inversion/gamma) for accuracy.
 * Returns a value in [contrast_min, 1.0].
 *
 * NOTE: This function accesses g_sp3ctra_config for contrast parameters.
 *       Future refactoring may pass these as explicit parameters.
 * ============================================================================ */
float img_stage_calculate_contrast(
    const float *grayscale,
    int          pixel_count,
    float        contrast_min,
    float        contrast_adjustment_power,
    float        contrast_stride
);

/* ============================================================================
 * LuxStral-specific: Per-note averaging
 * Averages `pixels_per_note` consecutive grayscale values into one note value.
 * Sets note[0] = 0.0 (legacy bug correction).
 * ============================================================================ */
void img_stage_grayscale_luxstral(
    const float *grayscale,
    int          pixel_count,
    int          pixels_per_note,
    int          max_notes,
    float       *notes_out,
    int         *num_notes_out
);

/* ============================================================================
 * LuxStral-specific: Stereo pan from color temperature
 * Computes per-note pan position and constant-power L/R gains.
 * ============================================================================ */
void img_stage_compute_pan_luxstral(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    int            pixels_per_note,
    int            max_notes,
    float          temp_amp,      /* per-engine colour-temperature amplification */
    float         *pan_out,
    float         *left_gains_out,
    float         *right_gains_out
);

/* ============================================================================
 * LuxSynth-specific: Linear grayscale (no gamma)
 * Same as rgb_to_grayscale but writes to a separate output for FFT input.
 * Optional inversion applied in the same pass.
 * ============================================================================ */
void img_stage_grayscale_luxsynth(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    int            do_inversion,
    float         *grayscale_out
);

/* ============================================================================
 * StrokeForge blob detection wrapper.
 * Delegates to strokeforge_analyze_frame() from the existing module.
 * ============================================================================ */
void img_stage_blob_detect(
    const float          *notes,
    int                   num_notes,
    float                 contrast_factor,
    StrokeForgeFrameData *out
);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PIPELINE_STAGES_H */
