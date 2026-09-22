/**
 * @file VideoBlit.h
 * @brief Fast ARGB raster helpers for the VIDEO MIX render thread.
 *
 * Two operations the waterfall path used to hand to juce::Graphics, which
 * routes them through the generic edge-table software rasteriser — a per-pixel
 * scalar path with no threading, and the single most expensive step of the
 * whole video chain at record resolutions:
 *
 *   affineARGB() : the zoom / rotate / scale blit of drawWarp(). Backward
 *                  mapped, bilinear, 16.16 fixed point, parallel per row, and
 *                  it PAINTS THE BORDER ITSELF (pixels sampling outside the
 *                  source get `outside`) so the separate full-surface fillAll()
 *                  the old path needed is gone.
 *   scaleARGB()  : plain rescale, no rotation. Box-average when both axes
 *                  shrink (a proper anti-aliased downsample for the preview
 *                  publish), bilinear otherwise (the recorder's upsample to the
 *                  chosen encode resolution).
 *
 * Both require 4-byte pixels (juce::Image::ARGB) and are byte-order agnostic:
 * interpolation is linear per byte, so the platform's channel layout never
 * enters the maths. They return false when handed anything else, letting the
 * caller fall back to juce::Graphics.
 *
 * Thread contract: call from the mixer's render thread only (they fan out over
 * videoparallel::parallelChunks and BLOCK until the fan-in). Never from the RT
 * audio thread.
 */
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace videoblit
{
    /** Blit `src` into `dest` through `srcToDst` (source pixel space →
     *  destination pixel space, the same convention as
     *  juce::Graphics::drawImageTransformed). Destination pixels whose sample
     *  falls outside the source are filled with `outside`, so `dest` is fully
     *  written and needs no prior fill. Returns false (dest untouched) if
     *  either image is not 4-byte ARGB or the transform is singular. */
    bool affineARGB(juce::Image& dest, const juce::Image& src,
                    const juce::AffineTransform& srcToDst, juce::Colour outside);

    /** Rescale the whole of `src` onto the whole of `dest`. Box-average on a
     *  shrink (anti-aliased), bilinear on a grow. Returns false if either image
     *  is not 4-byte ARGB. */
    bool scaleARGB(juce::Image& dest, const juce::Image& src);

    /** As scaleARGB, but onto a RAW 4-byte destination surface — the recorder's
     *  locked CVPixelBuffer, so the encode-resolution frame is produced in one
     *  pass with no intermediate image. `destStride` is in bytes. */
    bool scaleARGBToSurface(uint8_t* destData, int destW, int destH, int destStride,
                            const juce::Image& src);
}
