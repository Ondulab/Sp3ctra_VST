/*
 * image_source_router.c
 *
 * Implementation of source routing for the dual-path image pipeline.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#include "image_source_router.h"
#include <stddef.h>

/* ============================================================================
 * image_source_select — Route to the selected channel
 *
 * Since the "Modulated / Live" refactor the router only has two real cases.
 * The legacy `mix` argument is kept for ABI compatibility with callers that
 * still pass three frames; it is unused by the new channel model (modulated
 * processing is now baked upstream into the "sampler" frame: that frame is
 * already Live ► LuxSampler ► LuxPitch ► LuxMask when the relevant inserts
 * are active).
 *
 * Mapping:
 *   IMAGE_SOURCE_MODULATED (0) → `sampler` (the modulated chain output)
 *   IMAGE_SOURCE_LIVE      (1) → `live`    (raw UDP feed)
 * ============================================================================ */
ImageFrameRGB image_source_select(
    ImageSourceType      source,
    const ImageFrameRGB *sampler,
    const ImageFrameRGB *live,
    const ImageFrameRGB *mix)
{
    const ImageFrameRGB *selected = NULL;
    ImageFrameRGB        empty    = {NULL, NULL, NULL, 0};

    (void)mix; /* Deprecated argument — silently ignored */

    if (source == IMAGE_SOURCE_LIVE)
        selected = live;
    else /* IMAGE_SOURCE_MODULATED (and all deprecated aliases) */
        selected = sampler;

    /* Return the selected source if valid, otherwise return empty frame */
    if (selected != NULL && image_frame_is_valid(selected))
        return *selected;

    return empty;
}

/* ============================================================================
 * image_frame_is_valid — Check if a frame descriptor contains usable data
 * ============================================================================ */
int image_frame_is_valid(const ImageFrameRGB *frame)
{
    if (frame == NULL)
        return 0;
    if (frame->r == NULL || frame->g == NULL || frame->b == NULL)
        return 0;
    if (frame->pixel_count <= 0)
        return 0;
    return 1;
}
