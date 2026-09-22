#include "VideoBlit.h"
#include "VideoBilinear.h"
#include "VideoParallel.h"

#include <cmath>
#include <cstring>

namespace videoblit
{
namespace
{
    // Smallest row count worth handing to another thread (dispatch overhead is
    // ~µs; a chunk should be worth tens of those).
    constexpr int kMinRows = 24;

    struct Surface
    {
        uint8_t* data   { nullptr };
        int      width  { 0 };
        int      height { 0 };
        int      stride { 0 };
        bool ok() const noexcept { return data != nullptr && width > 0 && height > 0; }
    };

    // A 4-byte-per-pixel view of an image, or an invalid Surface when the image
    // is not packed ARGB (the caller then falls back to juce::Graphics).
    template <typename BitmapDataT>
    Surface surfaceOf(BitmapDataT& bd) noexcept
    {
        if (bd.pixelStride != 4) return {};
        return { bd.data, bd.width, bd.height, bd.lineStride };
    }

    uint32_t packed(juce::Colour c) noexcept
    {
        juce::PixelARGB px;
        px.setARGB(255, c.getRed(), c.getGreen(), c.getBlue());
        uint32_t out = 0;
        std::memcpy(&out, &px, sizeof(out));
        return out;
    }

    void fillRow(uint8_t* row, int n, uint32_t value) noexcept
    {
        auto* p = reinterpret_cast<uint32_t*>(row);
        for (int i = 0; i < n; ++i) p[i] = value;
    }

    // Narrow [lo,hi) to the x range satisfying 0 <= v0 + dv*x <= limit.
    // Returns false when the constraint is unsatisfiable anywhere.
    bool clipAxis(double v0, double dv, double limit, double& lo, double& hi) noexcept
    {
        constexpr double kEps = 1.0e-12;
        if (std::abs(dv) < kEps)
            return (v0 >= 0.0 && v0 <= limit);

        double a = (0.0   - v0) / dv;
        double b = (limit - v0) / dv;
        if (a > b) std::swap(a, b);
        lo = std::max(lo, a);
        hi = std::min(hi, b);
        return hi > lo;
    }
}

//==============================================================================
namespace
{
    bool affineToSurface(const Surface& d, const juce::Image& src,
                         const juce::AffineTransform& srcToDst, juce::Colour outside)
    {
    if (! src.isValid() || src.getFormat() != juce::Image::ARGB) return false;
    if (srcToDst.isSingularity()) return false;
    const juce::AffineTransform inv = srcToDst.inverted();

    juce::Image::BitmapData sbd(src, juce::Image::BitmapData::readOnly);
    const Surface s = surfaceOf(sbd);
    if (! d.ok() || ! s.ok()) return false;

    const uint32_t border = packed(outside);

    // Backward map: destination pixel centre → source coordinate. The step in
    // source space per destination x is constant (affine), so each row is a
    // straight 16.16 walk with no per-pixel transform maths.
    const double dux = inv.mat00;
    const double dvx = inv.mat10;

    // The source texel used for the 2x2 tap is clamped to [0, w-2] / [0, h-2],
    // so restricting the walk to [0, w-1] × [0, h-1] makes the last texel
    // interpolate exactly (weight 256) with no per-pixel bounds branch.
    const double maxU = (double) s.width  - 1.0;
    const double maxV = (double) s.height - 1.0;

    videoparallel::parallelChunks(0, d.height, kMinRows,
        [&](int, int y0, int y1)
        {
            for (int y = y0; y < y1; ++y)
            {
                uint8_t* dstRow = d.data + (size_t) y * (size_t) d.stride;

                const double cy = (double) y + 0.5;
                double u0 = inv.mat00 * 0.5 + inv.mat01 * cy + inv.mat02;
                double v0 = inv.mat10 * 0.5 + inv.mat11 * cy + inv.mat12;

                // x range whose sample lands inside the source.
                double lo = 0.0, hi = (double) d.width;
                bool inside = clipAxis(u0, dux, maxU, lo, hi)
                           && clipAxis(v0, dvx, maxV, lo, hi);

                // Valid integer columns are ceil(lo) … floor(hi) INCLUSIVE, so
                // the exclusive loop bound is floor(hi) + 1. Without the +1 the
                // last good column was painted as border — a one-pixel stripe
                // down the trailing edge of the image.
                int xa = inside ? (int) std::ceil (lo - 1.0e-9)     : d.width;
                int xb = inside ? (int) std::floor(hi + 1.0e-9) + 1 : d.width;
                xa = juce::jlimit(0, d.width, xa);
                xb = juce::jlimit(xa, d.width, xb);

                if (xa > 0)          fillRow(dstRow,                    xa,            border);
                if (xb < d.width)    fillRow(dstRow + (size_t) xb * 4,  d.width - xb,  border);
                if (xb <= xa) continue;

                // 16.16 walk across the inside span.
                int uq = (int) std::lround((u0 + dux * (double) xa) * 65536.0);
                int vq = (int) std::lround((v0 + dvx * (double) xa) * 65536.0);
                const int duq = (int) std::lround(dux * 65536.0);
                const int dvq = (int) std::lround(dvx * 65536.0);

                const int lastX = s.width  - 2 < 0 ? 0 : s.width  - 2;
                const int lastY = s.height - 2 < 0 ? 0 : s.height - 2;

                uint8_t* dp = dstRow + (size_t) xa * 4;
                for (int x = xa; x < xb; ++x, dp += 4, uq += duq, vq += dvq)
                {
                    const int ix = juce::jlimit(0, lastX, uq >> 16);
                    const int iy = juce::jlimit(0, lastY, vq >> 16);
                    // 16-bit weights: |delta| <= 255 and w <= 65536, so each
                    // product stays well inside int32 while the interpolation
                    // error drops below one level (8-bit weights cost up to 3).
                    const int wx = juce::jlimit(0, 65536, uq - (ix << 16));
                    const int wy = juce::jlimit(0, 65536, vq - (iy << 16));

                    const uint8_t* p00 = s.data + (size_t) iy * (size_t) s.stride + (size_t) ix * 4;
                    const uint8_t* p10 = p00 + 4;
                    const uint8_t* p01 = p00 + s.stride;
                    const uint8_t* p11 = p01 + 4;

                    bilinearPixel(dp,p00,p10,p01,p11,wx,wy);
                }
            }
        });

    return true;
    }
}

//==============================================================================
bool affineARGB(juce::Image& dest, const juce::Image& src,
                const juce::AffineTransform& srcToDst, juce::Colour outside)
{
    if (! dest.isValid() || dest.getFormat() != juce::Image::ARGB) return false;
    juce::Image::BitmapData dbd(dest, juce::Image::BitmapData::writeOnly);
    return affineToSurface(surfaceOf(dbd), src, srcToDst, outside);
}

//==============================================================================
namespace
{
    bool scaleToSurface(const Surface& d, const juce::Image& src)
    {
        if (! src.isValid() || src.getFormat() != juce::Image::ARGB) return false;
        const int sw = src.getWidth(), sh = src.getHeight();
        if (! d.ok() || sw <= 0 || sh <= 0) return false;
        const int dw = d.width, dh = d.height;

        // A grow (or a mixed grow/shrink) is a plain bilinear resample — reuse
        // the affine path, which already does exactly that.
        if (dw > sw || dh > sh)
            return affineToSurface(d, src,
                                   juce::AffineTransform::scale((float) dw / (float) sw,
                                                                (float) dh / (float) sh),
                                   juce::Colours::black);

        juce::Image::BitmapData sbd(src, juce::Image::BitmapData::readOnly);
        const Surface s = surfaceOf(sbd);
        if (! s.ok()) return false;

        // Shrink: average every source pixel each destination pixel covers — the
        // honest anti-aliased downsample (a bilinear tap here would alias the
        // waterfall's fine lines into moiré).
        videoparallel::parallelChunks(0, dh, kMinRows,
            [&](int, int y0, int y1)
            {
                for (int y = y0; y < y1; ++y)
                {
                    const int sy0 = (int) ((long long) y       * sh / dh);
                    int       sy1 = (int) ((long long) (y + 1) * sh / dh);
                    if (sy1 <= sy0) sy1 = sy0 + 1;

                    uint8_t* dp = d.data + (size_t) y * (size_t) d.stride;
                    for (int x = 0; x < dw; ++x, dp += 4)
                    {
                        const int sx0 = (int) ((long long) x       * sw / dw);
                        int       sx1 = (int) ((long long) (x + 1) * sw / dw);
                        if (sx1 <= sx0) sx1 = sx0 + 1;

                        int acc[4] = { 0, 0, 0, 0 };
                        int n = 0;
                        for (int sy = sy0; sy < sy1 && sy < s.height; ++sy)
                        {
                            const uint8_t* sp = s.data + (size_t) sy * (size_t) s.stride
                                                       + (size_t) sx0 * 4;
                            for (int sx = sx0; sx < sx1 && sx < s.width; ++sx, sp += 4)
                            {
                                acc[0] += sp[0]; acc[1] += sp[1];
                                acc[2] += sp[2]; acc[3] += sp[3];
                                ++n;
                            }
                        }
                        if (n <= 0) { std::memset(dp, 0, 4); continue; }
                        for (int c = 0; c < 4; ++c)
                            dp[c] = (uint8_t) ((acc[c] + n / 2) / n);
                    }
                }
            });

        return true;
    }
}

bool scaleARGB(juce::Image& dest, const juce::Image& src)
{
    if (! dest.isValid() || dest.getFormat() != juce::Image::ARGB) return false;
    juce::Image::BitmapData dbd(dest, juce::Image::BitmapData::writeOnly);
    return scaleToSurface(surfaceOf(dbd), src);
}

bool scaleARGBToSurface(uint8_t* destData, int destW, int destH, int destStride,
                        const juce::Image& src)
{
    return scaleToSurface(Surface { destData, destW, destH, destStride }, src);
}
}
