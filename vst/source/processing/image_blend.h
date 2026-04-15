/*
 * image_blend.h
 *
 * Darken-blend module for mixing Sampler and Live image sources.
 * Extracted from FrameSampler::FramePlayerThread for reuse in the pipeline.
 *
 * Blend rule: darken (min per channel) with per-source opacity.
 * White (255) is the identity element for darken blend,
 * so a source with opacity=0 becomes white and does not affect the result.
 *
 * RT-safety: Pure C, stateless, no allocation.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#ifndef IMAGE_BLEND_H
#define IMAGE_BLEND_H

#include "image_pipeline_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Darken-blend two RGB frames with independent opacities.
 *
 * For each pixel:
 *   1. Scale source A toward white:  a_out = a_pixel * a_opacity + 255 * (1 - a_opacity)
 *   2. Scale source B toward white:  b_out = b_pixel * b_opacity + 255 * (1 - b_opacity)
 *   3. Darken blend: result = min(a_out, b_out) per channel
 *
 * If one source has fewer pixels than the other, the shorter source is
 * treated as white (255) beyond its pixel_count (identity for darken).
 *
 * @param sampler         Sampler (S) source frame
 * @param sampler_opacity Sampler opacity [0.0, 1.0] — 0 = white/silent, 1 = full signal
 * @param live            Live (L) source frame
 * @param live_opacity    Live opacity [0.0, 1.0] — 0 = white/silent, 1 = full signal
 * @param out_r           Output red channel buffer (must be pre-allocated, size >= max_pixels)
 * @param out_g           Output green channel buffer
 * @param out_b           Output blue channel buffer
 * @param max_pixels      Maximum number of pixels to write to output buffers
 * @return                Number of pixels actually written to output
 */
int image_blend_darken(
    const ImageFrameRGB *sampler,
    float                sampler_opacity,
    const ImageFrameRGB *live,
    float                live_opacity,
    uint8_t             *out_r,
    uint8_t             *out_g,
    uint8_t             *out_b,
    int                  max_pixels
);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_BLEND_H */
