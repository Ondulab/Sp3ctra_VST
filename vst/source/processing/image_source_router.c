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
 * image_source_select — Route to the selected source
 * ============================================================================ */
ImageFrameRGB image_source_select(
    ImageSourceType      source,
    const ImageFrameRGB *sampler,
    const ImageFrameRGB *live,
    const ImageFrameRGB *mix)
{
    const ImageFrameRGB *selected = NULL;
    ImageFrameRGB        empty    = {NULL, NULL, NULL, 0};

    switch (source)
    {
        case IMAGE_SOURCE_SAMPLER:
            selected = sampler;
            break;
        case IMAGE_SOURCE_LIVE:
            selected = live;
            break;
        case IMAGE_SOURCE_MIX:
            selected = mix;
            break;
        default:
            return empty;
    }

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
