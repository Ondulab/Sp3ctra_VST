/*
 * image_source_router.h
 *
 * Source routing module for the dual-path image pipeline.
 * Selects which input source (Sampler, Live, or Mix) feeds each path.
 *
 * RT-safety: All functions are pure, stateless, and allocation-free.
 * No JUCE dependencies — pure C interface.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#ifndef IMAGE_SOURCE_ROUTER_H
#define IMAGE_SOURCE_ROUTER_H

#include "image_pipeline_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Select the source frame for a given path based on routing configuration.
 *
 * Returns the ImageFrameRGB descriptor corresponding to the selected source.
 * If the selected source is unavailable (NULL pointers or zero pixel_count),
 * returns a zeroed ImageFrameRGB (all NULL pointers, pixel_count = 0).
 *
 * @param source    Which source to select (S, L, or M)
 * @param sampler   Sampler (S) frame — from CapturedFrame playback (may be NULL)
 * @param live      Live (L) frame — from AudioImageBuffers read pointer (may be NULL)
 * @param mix       Mix (M) frame — pre-blended S+L from image_blend_darken (may be NULL)
 * @return          The selected source as an ImageFrameRGB descriptor
 */
ImageFrameRGB image_source_select(
    ImageSourceType      source,
    const ImageFrameRGB *sampler,
    const ImageFrameRGB *live,
    const ImageFrameRGB *mix
);

/**
 * @brief Check if an ImageFrameRGB descriptor contains valid data.
 *
 * @param frame  Frame to check
 * @return       1 if frame has non-NULL RGB pointers and pixel_count > 0, else 0
 */
int image_frame_is_valid(const ImageFrameRGB *frame);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_SOURCE_ROUTER_H */
