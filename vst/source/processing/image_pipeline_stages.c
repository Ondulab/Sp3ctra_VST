/*
 * image_pipeline_stages.c
 *
 * Implementation of atomic, stateless processing stages for the image pipeline.
 * Each function extracted from image_preprocessor.c into a reusable, testable unit.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#include "image_pipeline_stages.h"
#include "../synthesis/luxstral/synth_luxstral_stereo.h"
#include "../synthesis/luxstral/synth_luxstral_math.h"
#include <math.h>
#include <string.h>
#include <stddef.h>

/* ============================================================================
 * img_stage_rgb_to_grayscale — ITU-R BT.601 luminance conversion
 * ============================================================================ */
void img_stage_rgb_to_grayscale(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    float         *grayscale_out)
{
    int i;
    if (raw_r == NULL || raw_g == NULL || raw_b == NULL ||
        grayscale_out == NULL || pixel_count <= 0)
        return;

    for (i = 0; i < pixel_count; i++)
    {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;

        /* Preventive clamping for floating-point rounding */
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        grayscale_out[i] = normalized;
    }
}

/* ============================================================================
 * img_stage_invert — In-place inversion: pixel = 1.0 - pixel
 * ============================================================================ */
void img_stage_invert(float *pixels, int count)
{
    int i;
    if (pixels == NULL || count <= 0)
        return;

    for (i = 0; i < count; i++)
        pixels[i] = 1.0f - pixels[i];
}

/* ============================================================================
 * img_stage_remove_dc — Subtract per-line mean (AC extraction)
 * ============================================================================ */
void img_stage_remove_dc(float *pixels, int count)
{
    int i;
    float sum, mean;

    if (pixels == NULL || count <= 0)
        return;

    /* Compute mean */
    sum = 0.0f;
    for (i = 0; i < count; i++)
        sum += pixels[i];
    mean = sum / (float)count;

    /* Subtract mean and clamp to [0, 1] */
    for (i = 0; i < count; i++)
    {
        pixels[i] -= mean;
        if (pixels[i] < 0.0f) pixels[i] = 0.0f;
        if (pixels[i] > 1.0f) pixels[i] = 1.0f;
    }
}

/* ============================================================================
 * img_stage_apply_gamma — Non-linear gamma correction (photo convention)
 *
 * Uses the photo/display convention: output = pow(input, 1/gamma).
 *   gamma > 1  →  brightens midtones (boost)
 *   gamma < 1  →  darkens  midtones (compression)
 *   gamma = 1  →  linear   (bypass)
 * ============================================================================ */
void img_stage_apply_gamma(float *pixels, int count, float gamma)
{
    int i;

    if (pixels == NULL || count <= 0)
        return;

    /* Gamma = 0.0 means bypass */
    if (gamma == 0.0f || gamma == 1.0f)
        return;

    const float exponent = 1.0f / gamma;

    for (i = 0; i < count; i++)
    {
        float val = pixels[i];

        /* Clamp input to valid range for powf */
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;

        float result = powf(val, exponent);

        /* NaN/Inf protection (portable, no isnan/isinf) */
        if (result != result || result * 0.0f != 0.0f)
            result = val;

        pixels[i] = result;
    }
}

/* ============================================================================
 * img_stage_calculate_contrast — Variance-based contrast measurement
 *
 * Extracted from image_preprocessor.c::calculate_contrast().
 * Must be called on RAW grayscale (before inversion/gamma) for accuracy.
 * ============================================================================ */
float img_stage_calculate_contrast(
    const float *grayscale,
    int          pixel_count,
    float        contrast_min,
    float        contrast_adjustment_power,
    float        contrast_stride)
{
    size_t sample_stride, valid_samples;
    int i;
    float sum, sum_sq, mean, raw_variance, variance, val;
    float max_possible_variance, contrast_ratio, adjusted_contrast, result;

    if (grayscale == NULL || pixel_count <= 0)
        return 1.0f; /* Default = maximum volume */

    sample_stride = (size_t)contrast_stride;
    if (sample_stride == 0)
        sample_stride = 1;

    sum = 0.0f;
    sum_sq = 0.0f;
    valid_samples = 0;

    for (i = 0; i < pixel_count; i += (int)sample_stride)
    {
        val = grayscale[i];
        /* NaN/Inf check (portable) */
        if (val != val || val * 0.0f != 0.0f)
            continue;

        sum += val;
        sum_sq += val * val;
        valid_samples++;
    }

    if (valid_samples == 0)
        return 1.0f;

    mean = sum / (float)valid_samples;
    raw_variance = (sum_sq / (float)valid_samples) - (mean * mean);
    variance = (raw_variance > 0.0f) ? raw_variance : 0.0f;

    /* VOLUME_AMP_RESOLUTION = 1.0 (normalized float range) */
    max_possible_variance = (1.0f * 1.0f) / 4.0f;

    if (max_possible_variance <= 0.0f)
        return 1.0f;

    contrast_ratio = sqrtf(variance) / sqrtf(max_possible_variance);

    /* NaN/Inf protection */
    if (contrast_ratio != contrast_ratio || contrast_ratio * 0.0f != 0.0f)
        return 1.0f;

    adjusted_contrast = powf(contrast_ratio, contrast_adjustment_power);

    /* Linear mapping from contrast_min to 1.0 */
    result = contrast_min + (1.0f - contrast_min) * adjusted_contrast;

    if (result > 1.0f) result = 1.0f;
    if (result < contrast_min) result = contrast_min;

    return result;
}

/* ============================================================================
 * img_stage_grayscale_luxstral — Per-note averaging for additive synthesis
 * ============================================================================ */
void img_stage_grayscale_luxstral(
    const float *grayscale,
    int          pixel_count,
    int          pixels_per_note,
    int          max_notes,
    float       *notes_out,
    int         *num_notes_out)
{
    int num_notes, note, pix;

    if (grayscale == NULL || notes_out == NULL || num_notes_out == NULL)
        return;
    if (pixel_count <= 0 || pixels_per_note <= 0 || max_notes <= 0)
        return;

    num_notes = pixel_count / pixels_per_note;
    if (num_notes > max_notes)
        num_notes = max_notes;

    for (note = 0; note < num_notes; note++)
    {
        float sum = 0.0f;
        int valid_pixels = 0;

        for (pix = 0; pix < pixels_per_note; pix++)
        {
            int idx = note * pixels_per_note + pix;
            if (idx < pixel_count)
            {
                float val = grayscale[idx];
                /* NaN/Inf protection */
                if (val == val && val * 0.0f == 0.0f)
                {
                    sum += val;
                    valid_pixels++;
                }
            }
        }

        notes_out[note] = (valid_pixels > 0)
                          ? (sum / (float)valid_pixels)
                          : 0.0f;

        /* Final NaN check */
        if (notes_out[note] != notes_out[note])
            notes_out[note] = 0.0f;
    }

    /* Bug correction: note 0 = 0 (legacy behaviour) */
    if (num_notes > 0)
        notes_out[0] = 0.0f;

    *num_notes_out = num_notes;
}

/* ============================================================================
 * img_stage_compute_pan_luxstral — Stereo pan from color temperature
 * ============================================================================ */
void img_stage_compute_pan_luxstral(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    int            pixels_per_note,
    int            max_notes,
    float         *pan_out,
    float         *left_gains_out,
    float         *right_gains_out)
{
    int num_notes, note, pix;

    if (raw_r == NULL || raw_g == NULL || raw_b == NULL)
        return;
    if (pan_out == NULL || left_gains_out == NULL || right_gains_out == NULL)
        return;
    if (pixel_count <= 0 || pixels_per_note <= 0 || max_notes <= 0)
        return;

    num_notes = pixel_count / pixels_per_note;
    if (num_notes > max_notes)
        num_notes = max_notes;

    for (note = 0; note < num_notes; note++)
    {
        uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
        uint32_t count = 0;

        for (pix = 0; pix < pixels_per_note; pix++)
        {
            int idx = note * pixels_per_note + pix;
            if (idx < pixel_count)
            {
                r_sum += raw_r[idx];
                g_sum += raw_g[idx];
                b_sum += raw_b[idx];
                count++;
            }
        }

        if (count > 0)
        {
            uint8_t r_avg = (uint8_t)(r_sum / count);
            uint8_t g_avg = (uint8_t)(g_sum / count);
            uint8_t b_avg = (uint8_t)(b_sum / count);

            float temperature = calculate_color_temperature(r_avg, g_avg, b_avg);
            pan_out[note] = temperature;
            calculate_pan_gains(temperature,
                                &left_gains_out[note],
                                &right_gains_out[note]);
        }
        else
        {
            pan_out[note] = 0.0f;
            left_gains_out[note] = 0.707f;
            right_gains_out[note] = 0.707f;
        }
    }
}

/* ============================================================================
 * img_stage_grayscale_luxsynth — Linear grayscale for FFT path
 * ============================================================================ */
void img_stage_grayscale_luxsynth(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int            pixel_count,
    int            do_inversion,
    float         *grayscale_out)
{
    int i;

    if (raw_r == NULL || raw_g == NULL || raw_b == NULL ||
        grayscale_out == NULL || pixel_count <= 0)
        return;

    for (i = 0; i < pixel_count; i++)
    {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;

        if (do_inversion)
            normalized = 1.0f - normalized;

        grayscale_out[i] = normalized;
    }
}

/* ============================================================================
 * img_stage_copy_rgb_raw — Direct RGB copy for LuxWave
 * ============================================================================ */
void img_stage_copy_rgb_raw(
    const uint8_t *rgb_r,
    const uint8_t *rgb_g,
    const uint8_t *rgb_b,
    int            pixel_count,
    uint8_t       *r_out,
    uint8_t       *g_out,
    uint8_t       *b_out)
{
    if (rgb_r == NULL || rgb_g == NULL || rgb_b == NULL ||
        r_out == NULL || g_out == NULL || b_out == NULL ||
        pixel_count <= 0)
        return;

    memcpy(r_out, rgb_r, (size_t)pixel_count);
    memcpy(g_out, rgb_g, (size_t)pixel_count);
    memcpy(b_out, rgb_b, (size_t)pixel_count);
}

/* ============================================================================
 * img_stage_blob_detect — StrokeForge blob detection wrapper
 * ============================================================================ */
void img_stage_blob_detect(
    const float          *notes,
    int                   num_notes,
    float                 contrast_factor,
    StrokeForgeFrameData *out)
{
    if (notes == NULL || out == NULL || num_notes <= 0)
        return;

    strokeforge_analyze_frame(notes, num_notes, contrast_factor, out);
}
