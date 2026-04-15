/*
 * image_blend.c
 *
 * Implementation of darken-blend for the image pipeline.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#include "image_blend.h"
#include <stddef.h>

/* ============================================================================
 * image_blend_darken — Darken-blend two RGB frames with opacities
 *
 * Blend rule per pixel per channel:
 *   scaled_a = a * opacity_a + 255 * (1 - opacity_a)  — fade toward white
 *   scaled_b = b * opacity_b + 255 * (1 - opacity_b)  — fade toward white
 *   result   = min(scaled_a, scaled_b)                 — darken
 *
 * White (255) is the identity element: min(x, 255) = x.
 * An opacity of 0 produces white, which does not affect the other source.
 * ============================================================================ */
int image_blend_darken(
    const ImageFrameRGB *sampler,
    float                sampler_opacity,
    const ImageFrameRGB *live,
    float                live_opacity,
    uint8_t             *out_r,
    uint8_t             *out_g,
    uint8_t             *out_b,
    int                  max_pixels)
{
    int smp_count, live_count, out_count, i;
    float smp_inv, live_inv;

    if (out_r == NULL || out_g == NULL || out_b == NULL || max_pixels <= 0)
        return 0;

    /* Determine pixel counts for each source */
    smp_count  = (sampler != NULL && sampler->r != NULL) ? sampler->pixel_count : 0;
    live_count = (live != NULL && live->r != NULL) ? live->pixel_count : 0;

    /* Output count = max of both sources, clamped to buffer size */
    out_count = (smp_count > live_count) ? smp_count : live_count;
    if (out_count > max_pixels)
        out_count = max_pixels;
    if (out_count <= 0)
        return 0;

    /* Clamp opacities */
    if (sampler_opacity < 0.0f) sampler_opacity = 0.0f;
    if (sampler_opacity > 1.0f) sampler_opacity = 1.0f;
    if (live_opacity < 0.0f) live_opacity = 0.0f;
    if (live_opacity > 1.0f) live_opacity = 1.0f;

    smp_inv  = 1.0f - sampler_opacity;
    live_inv = 1.0f - live_opacity;

    for (i = 0; i < out_count; i++)
    {
        /* Get sampler pixel (white if beyond range or unavailable) */
        int sr, sg, sb;
        if (i < smp_count)
        {
            sr = (int)(sampler->r[i] * sampler_opacity + 255.0f * smp_inv);
            sg = (int)(sampler->g[i] * sampler_opacity + 255.0f * smp_inv);
            sb = (int)(sampler->b[i] * sampler_opacity + 255.0f * smp_inv);
        }
        else
        {
            sr = sg = sb = 255; /* White = identity for darken */
        }

        /* Get live pixel (white if beyond range or unavailable) */
        int lr, lg, lb;
        if (i < live_count)
        {
            lr = (int)(live->r[i] * live_opacity + 255.0f * live_inv);
            lg = (int)(live->g[i] * live_opacity + 255.0f * live_inv);
            lb = (int)(live->b[i] * live_opacity + 255.0f * live_inv);
        }
        else
        {
            lr = lg = lb = 255;
        }

        /* Darken blend: take the darkest pixel per channel */
        out_r[i] = (uint8_t)((sr < lr) ? sr : lr);
        out_g[i] = (uint8_t)((sg < lg) ? sg : lg);
        out_b[i] = (uint8_t)((sb < lb) ? sb : lb);
    }

    return out_count;
}
