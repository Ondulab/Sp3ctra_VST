#include "VideoScrollRenderCore.h"
#include "../PluginProcessor.h"   // Sp3ctraAudioProcessor, vsParam(), getAPVTS()

// Per-instance capture ring API. video_scroll.h already wraps its declarations in
// its own extern "C" guard, so a plain include is correct (see file header note).
#include "../processing/video_scroll.h"

#include <cstring>
#include <cmath>

//==============================================================================
// Construction
//==============================================================================
VideoScrollRenderCore::VideoScrollRenderCore(Sp3ctraAudioProcessor& proc, int slot)
    : processor_(proc), slot_(juce::jlimit(0, CHAIN_MAX_CHAINS - 1, slot))
{
    // Preallocate the per-tick capture window (one row per possible ring slot).
    // Matches the original frameRing_ window: at most RING_SLOTS fresh frames are
    // ever drained in a single tick (newest-wins clamp inside the ring API).
    capR_.resize(VIDEO_SCROLL_RING_SLOTS);
    capG_.resize(VIDEO_SCROLL_RING_SLOTS);
    capB_.resize(VIDEO_SCROLL_RING_SLOTS);
    capPx_.assign(VIDEO_SCROLL_RING_SLOTS, 0);

    tmpR_.resize(VIDEO_SCROLL_MAX_PIXELS);
    tmpG_.resize(VIDEO_SCROLL_MAX_PIXELS);
    tmpB_.resize(VIDEO_SCROLL_MAX_PIXELS);

    // Anchor the ring cursor + generation so a freshly-created engine doesn't
    // replay stale frames or spuriously clear on its first tick.
    if (auto* st = video_scroll_instance(slot_))
    {
        cursor_  = video_scroll_ring_writepos(st);
        lastGen_ = video_scroll_generation(st);
        genInit_ = true;
    }
}

VideoScrollRenderCore::~VideoScrollRenderCore() = default;

//==============================================================================
void VideoScrollRenderCore::setSlot(int slot)
{
    const int s = juce::jlimit(0, CHAIN_MAX_CHAINS - 1, slot);
    if (s == slot_) return;
    slot_ = s;

    // Re-anchor on the new ring and treat the switch as a discontinuity.
    if (auto* st = video_scroll_instance(slot_))
    {
        cursor_  = video_scroll_ring_writepos(st);
        lastGen_ = video_scroll_generation(st);
        genInit_ = true;
    }
    heldPx_ = 0;
    clear();
}

//==============================================================================
// APVTS slot-scoped raw param load (null-guarded). Mirrors the legacy globals
// the original read, but scoped to this engine's slot.
//==============================================================================
float VideoScrollRenderCore::param(const char* suffix, float defaultValue) const
{
    if (auto* p = processor_.getAPVTS().getRawParameterValue(vsParam(slot_, suffix)))
        return p->load();
    return defaultValue;
}

//==============================================================================
// Buffer allocation — mirrors VideoDisplayComponent::allocateScrollBuffer(),
// except the previous history is RESCALED into the new buffer instead of being
// discarded: a window open/resize used to blank every waterfall to black (one
// of the reported "flash" glitches).
//==============================================================================
void VideoScrollRenderCore::allocateScrollBuffer(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w != compW_ || h != compH_)
    {
        compW_ = w;
        compH_ = h;
        bufW_  = w;
        // 4× headroom: birth-line panning + extra history for the Compression
        // time-squish to actually have older content to pack into the far field.
        const int newBufH = juce::jmax(2, h * 4);
        // SoftwareImageType → guaranteed packed RGB (pixelStride 3) so the raw
        // BitmapData pointer maths in scrollStep()/buildWarp() is correct
        // (the native macOS backend would store RGB as 4-byte ARGB).
        // White = blank paper: an empty chain streams white, never black.
        juce::Image next(juce::Image::RGB, bufW_, newBufH, true, juce::SoftwareImageType());
        next.clear(next.getBounds(), juce::Colours::white);
        if (history_.isValid() && buffersInit_)
        {
            juce::Graphics g(next);
            g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
            g.drawImage(history_, 0, 0, bufW_, newBufH,
                        0, 0, history_.getWidth(), history_.getHeight());
        }
        bufH_              = newBufH;
        history_           = std::move(next);
        scrollAccumulator_ = 0.f;
        buffersInit_       = true;
        warpDirty_         = true;   // new buffer → force a warp rebuild.
    }
}

void VideoScrollRenderCore::setDisplaySize(int w, int h)
{
    // Horizontal display (Deg90/270): allocate at swapped dims so the frame,
    // once rotated by drawWarp(), natively matches the viewport aspect instead
    // of letterboxing to a sliver.
    if ((juce::jlimit(0, 3, (int) param("mode", 0.f)) & 1) != 0)
        std::swap(w, h);
    allocateScrollBuffer(w, h);
}

//==============================================================================
void VideoScrollRenderCore::clear()
{
    if (history_.isValid())
        history_.clear(history_.getBounds(), juce::Colours::white);   // blank paper
    scrollAccumulator_ = 0.f;
    heldPx_            = 0;      // a cleared waterfall must not resurrect old lines
    warpDirty_         = true;   // history blanked → force a warp rebuild.
}

//==============================================================================
// drainRing — replaces the original CaptureThread + the frameRing_ window that
// scrollStep() snapshotted. Pulls up to RING_SLOTS fresh scanlines from the
// per-instance ring into capR_/capG_/capB_ (oldest first → newest last, matching
// the original base..wr ordering). Skips frames where ring_get returns 0 (the
// slot was overwritten mid-copy — indeterminate, must not be rendered). Advances
// cursor_ to writepos so the ring never backs up. Returns captured count.
//==============================================================================
int VideoScrollRenderCore::drainRing(int* outPixelCount)
{
    if (outPixelCount) *outPixelCount = 0;

    auto* st = video_scroll_instance(slot_);
    if (!st) return 0;

    // Generation change → producer signalled a discontinuity. Blank history and
    // re-anchor the cursor (mirror the original clear-pulse handling in
    // timerCallback(), but driven by the ring's own generation counter).
    const uint32_t gen = video_scroll_generation(st);
    if (!genInit_) { lastGen_ = gen; genInit_ = true; }
    else if (gen != lastGen_)
    {
        lastGen_ = gen;
        clear();
        cursor_ = video_scroll_ring_writepos(st);
        return 0;
    }

    // Snapshot the write position ONCE (like the original read ringWriteIdx_
    // once per scrollStep), so `available`, `base` and the final cursor advance
    // all use a single coherent `wr`. Deriving available from this same `wr`
    // avoids a skew if the producer advances between two separate acquire loads.
    const uint32_t wr = video_scroll_ring_writepos(st);
    uint32_t available = wr - cursor_;          // wrap-safe unsigned subtraction
    if (available == 0)
        return 0;
    if (available > (uint32_t) VIDEO_SCROLL_RING_SLOTS)
        available = (uint32_t) VIDEO_SCROLL_RING_SLOTS;   // overrun clamp: newest window

    const uint32_t base = wr - available;   // absolute index of the oldest in-window frame

    int captured = 0;
    for (uint32_t k = 0; k < available; ++k)
    {
        int px = 0;
        const int ok = video_scroll_ring_get(st, base + k,
                                              tmpR_.data(), tmpG_.data(), tmpB_.data(), &px);
        if (!ok || px <= 0)
            continue;   // 0 return → indeterminate slot; skip it.

        if (px > VIDEO_SCROLL_MAX_PIXELS) px = VIDEO_SCROLL_MAX_PIXELS;

        auto& dr = capR_[captured];
        auto& dg = capG_[captured];
        auto& db = capB_[captured];
        dr.assign(tmpR_.begin(), tmpR_.begin() + px);
        dg.assign(tmpG_.begin(), tmpG_.begin() + px);
        db.assign(tmpB_.begin(), tmpB_.begin() + px);
        capPx_[captured] = px;
        ++captured;

        if (outPixelCount) *outPixelCount = px;   // newest valid pixel count wins
    }

    // Advance to writepos regardless of skips — capture never lags the display.
    cursor_ = wr;
    return captured;
}

//==============================================================================
// tick — drains the ring and advances the scroll (the original timerCallback()
// body, minus the repaint which the mixer owns). Returns true when the history
// buffer was mutated (scrolled/stamped/cleared) — false lets the mixer skip the
// repaint entirely (a paused output must NOT keep invalidating its views: 60 fps
// repaints of a static image occasionally get presented half-painted by the OS,
// which reads as flicker).
//==============================================================================
bool VideoScrollRenderCore::tick(double nowMs, double dtMs)
{
    if (!buffersInit_ || bufW_ <= 0 || bufH_ <= 0)
        return false;

    return scrollStep(nowMs, dtMs);
}

//==============================================================================
// scrollStep — one scroll generation (legacy birth-line model), now IN PLACE.
// Port of VideoDisplayComponent::scrollStep(), with the ring snapshot replaced
// by drainRing() and the ping-pong Graphics blit replaced by direct row moves
// (memcpy) on the single history buffer — same visual result, a fraction of the
// memory traffic. The speed curve, birth line and geometry are unchanged; the
// advance is scaled by dtMs so real time, not tick cadence, drives the scroll.
//==============================================================================
bool VideoScrollRenderCore::scrollStep(double nowMs, double dtMs)
{
    // ── Transport: paused → freeze in place ──────────────────────────────────
    // Keep the current image untouched and perform no scroll/stamp. The capture
    // ring is still drained (cursor re-anchored to writepos) so it never backs up
    // while frozen; resuming picks up the live stream rather than replaying stale
    // frames — exactly the original pause behaviour.
    if (param("paused", 0.f) >= 0.5f)
    {
        if (auto* st = video_scroll_instance(slot_))
            cursor_ = video_scroll_ring_writepos(st);
        scrollAccumulator_ = 0.f;
        return false;
    }

    // The history buffer is kept as a CLEAN linear waterfall (one buffer row per
    // unit of CIS time). The artistic time-squish (Compression) and aging (Fade)
    // are NOT baked in here — they are applied at display time in buildWarp() as
    // functions of the distance from the birth line.

    // ── Speed → signed pixels this tick (absolute, exponential) ──────────────
    //   px/frame = sign(s) * (2^(kSpeedExp*|s|) - 1) at the 60 fps reference.
    //   kSpeedExp is set so |s|=1 → ~16.7 px/frame = ~1000 px/s → one screen
    //   pixel per incoming CIS line = full-fidelity 1000 lps. The advance is
    //   scaled by the REAL elapsed time so a late tick scrolls proportionally
    //   more instead of silently slowing the waterfall down.
    constexpr float kSpeedExp    = 4.14f;
    constexpr double kRefFrameMs = 1000.0 / 60.0;
    const float speedParam = juce::jlimit(-1.f, 1.f, param("speed", 0.f));
    const float mag    = std::pow(2.0f, kSpeedExp * std::abs(speedParam)) - 1.0f;
    const float dtScale = (float) (juce::jlimit(1.0, 100.0, dtMs) / kRefFrameMs);
    const float pxRate = ((speedParam < 0.f) ? -mag : mag) * dtScale;
    const bool  reverse = (pxRate < 0.f);

    scrollAccumulator_ += std::abs(pxRate);
    const int scroll = (int) scrollAccumulator_;
    scrollAccumulator_ -= (float) scroll;

    // Fresh CIS frames captured since the previous tick (drained every tick so
    // capture never lags behind the display).
    int newestPx = 0;
    int captured = drainRing(&newestPx);

    // ── Starvation bridge ─────────────────────────────────────────────────────
    // The producer is bursty (UDP packet granularity) while the scroll advances
    // continuously: a starved tick that still scrolls used to leave a black
    // 2×scroll gap at the birth line (the "bandes noires"). Re-stamp the newest
    // known line for up to kHoldMs; past that the source is genuinely stopped
    // and the honest black gap returns (no-signal contract).
    if (captured > 0)
    {
        const int n = captured - 1;
        heldR_ = capR_[n];
        heldG_ = capG_[n];
        heldB_ = capB_[n];
        heldPx_   = capPx_[n];
        heldAtMs_ = nowMs;
    }
    else if (scroll > 0 && heldPx_ > 0 && (nowMs - heldAtMs_) <= kHoldMs)
    {
        capR_[0] = heldR_;
        capG_[0] = heldG_;
        capB_[0] = heldB_;
        capPx_[0] = heldPx_;
        captured  = 1;
        newestPx  = heldPx_;
    }

    // ── Birth-line position ──────────────────────────────────────────────────
    const float posParam = juce::jlimit(-1.f, 1.f, param("linePos", 1.f));
    const float posNorm  = (posParam + 1.f) * 0.5f;
    const int   birthY   = juce::jlimit(0, bufH_, (int) (posNorm * (float) bufH_));

    // ── Line geometry ────────────────────────────────────────────────────────
    //   coreH : data rows that exactly fill the motion gap (2*scroll px) at a
    //           fixed 1-row-per-time-slice scale (keeps the time-scale constant).
    //   bandH : total stamped height. Thickness DUPLICATES the birth line onto
    //           the rows above/below it (see buildLineImage): a clean fat bar.
    const float thickParam  = juce::jlimit(0.f, 1.f, param("thickness", 0.f));
    const int   thicknessPx = juce::jmax(1, (int) (1.0f + thickParam * (float) (compH_ - 1)));
    const int   coreH = juce::jmax(1, 2 * scroll);
    const int   bandH = juce::jmax(coreH, thicknessPx);

    // Build the fresh content: the newest `captured` frames stamped as DISTINCT
    // rows across the band (time gradient, newest at the birth line) instead of
    // averaged into one line — this is what preserves the 1000 lps detail.
    juce::Image lineImg;
    const bool haveLine = buildLineImage(lineImg, coreH, bandH, captured, newestPx);

    // Nothing changed and not scrolling — keep last frame (the display effects
    // are re-applied from the cached warp each refresh).
    if (scroll == 0 && !haveLine)
        return false;

    {
        juce::Image::BitmapData bmp(history_, juce::Image::BitmapData::readWrite);
        const size_t rowBytes = (size_t) bufW_ * (size_t) bmp.pixelStride;
        auto rowPtr = [&bmp](int y) { return bmp.getLinePointer(y); };

        if (scroll > 0)
        {
            if (!reverse)
            {
                // Zones move AWAY from the birth line. Upper zone [0, birthY)
                // shifts up (ascending copy reads rows not yet overwritten);
                // lower zone [birthY, bufH_) shifts down (descending copy).
                for (int y = 0; y < birthY - scroll; ++y)
                    std::memcpy(rowPtr(y), rowPtr(y + scroll), rowBytes);
                for (int y = bufH_ - 1; y >= birthY + scroll; --y)
                    std::memcpy(rowPtr(y), rowPtr(y - scroll), rowBytes);
                // The vacated 2×scroll band around the birth line is covered by
                // the stamp below (bandH >= 2*scroll). If this tick has nothing
                // to stamp (source stopped past the hold window) blank it — the
                // honest "no stream" WHITE (blank paper, never black).
                if (!haveLine)
                    for (int y = juce::jmax(0, birthY - scroll);
                         y < juce::jmin(bufH_, birthY + scroll); ++y)
                        std::memset(rowPtr(y), 0xFF, rowBytes);
            }
            else
            {
                // Reverse: zones converge TOWARD the birth line; the outer edges
                // are vacated. Upper zone shifts down (descending copy), lower
                // zone shifts up (ascending copy); the stamp band overwrites the
                // 2×scroll rows around the line where the zones meet.
                for (int y = birthY - 1; y >= scroll; --y)
                    std::memcpy(rowPtr(y), rowPtr(y - scroll), rowBytes);
                for (int y = 0; y < juce::jmin(scroll, birthY); ++y)
                    std::memset(rowPtr(y), 0xFF, rowBytes);   // vacated edge → white
                for (int y = birthY; y < bufH_ - scroll; ++y)
                    std::memcpy(rowPtr(y), rowPtr(y + scroll), rowBytes);
                for (int y = juce::jmax(birthY, bufH_ - scroll); y < bufH_; ++y)
                    std::memset(rowPtr(y), 0xFF, rowBytes);   // vacated edge → white
            }
        }

        // Stamp the fresh strip at the birth line (already bandH px tall).
        // lineImg is packed RGB at bufW_ like the history → straight row copies.
        if (haveLine && lineImg.isValid())
        {
            const juce::Image::BitmapData line(lineImg, juce::Image::BitmapData::readOnly);
            const int hPx  = lineImg.getHeight();
            const int yPos = (int) ((float) birthY - (float) hPx * 0.5f);
            const int y0   = juce::jmax(0, yPos);
            const int y1   = juce::jmin(bufH_, yPos + hPx);
            for (int y = y0; y < y1; ++y)
                std::memcpy(rowPtr(y), line.getLinePointer(y - yPos), rowBytes);
        }
    }

    warpDirty_ = true;   // history advanced → buildWarp() must rebuild this tick.
    return true;
}

//==============================================================================
// buildLineImage — turn the `captured` CIS frames drained this tick into the
// birth-line strip. Each frame becomes ONE distinct display row (width bufW_,
// horizontal box-average), and the rows are mapped onto the band as a time
// gradient with the NEWEST frame at the birth line. This preserves the full
// 1000 lps temporal detail instead of collapsing the whole tick into a single
// averaged line duplicated across the band (the old ~16:1 detail loss).
//   `captured`     = number of valid rows in capR_/capG_/capB_ this tick.
//   `captureCount` = newest valid pixel count (the CIS width to box-average).
//==============================================================================
bool VideoScrollRenderCore::buildLineImage(juce::Image& out, int coreH, int bandH,
                                           int captured, int captureCount)
{
    if (captured <= 0 || bufW_ <= 0) return false;
    coreH = juce::jmax(1, coreH);
    bandH = juce::jmax(coreH, bandH);

    // Inversion mode: 0 Off / 1 Negative (255-RGB) / 2 Luminance (invert HSL
    // lightness, keep hue+saturation). Fall back to the legacy "invert" bool
    // (== Negative) when a pre-migration session left invertMode at Off.
    int invMode = (int) param("invertMode", 0.f);
    if (invMode == 0 && param("invert", 0.f) > 0.5f) invMode = 1;
    const bool colorMode = param("colorMode", 0.f) > 0.5f;

    // The reference width is the newest captured line's pixel count.
    const int count = captureCount;
    if (count <= 0) return false;

    out = juce::Image(juce::Image::RGB, bufW_, bandH, false, juce::SoftwareImageType());
    juce::Image::BitmapData bmp(out, juce::Image::BitmapData::writeOnly);
    const int dps = bmp.pixelStride;

    // ── Horizontal box-average map ───────────────────────────────────────────
    // Output column x covers the CIS range [col0[x], col0[x+1]). Averaging the
    // whole range removes the moiré / hatching when the CIS line is much wider
    // than the display. Per-frame prefix sums make each column an O(1) lookup.
    std::vector<int> col0(bufW_ + 1);
    for (int x = 0; x <= bufW_; ++x)
        col0[x] = juce::jlimit(0, count, (int) ((long long) x * count / bufW_));

    // ── Collect the frames drained this tick (oldest → newest) ───────────────
    // Each valid frame becomes ONE distinct display row. We no longer collapse
    // the whole tick into a single averaged line (that discarded ~16:1 of the
    // 1000 lps temporal detail); the rows are mapped onto the band below.
    std::vector<int> valid;
    valid.reserve((size_t) juce::jmax(0, captured));
    for (int f = 0; f < captured; ++f)
        if (capPx_[f] == count) valid.push_back(f);
    const int used = (int) valid.size();

    const size_t rowBytes = (size_t) bufW_ * (size_t) dps;
    if (used == 0)
    {
        for (int r = 0; r < bandH; ++r)
            std::memset(bmp.getLinePointer(r), 0xFF, rowBytes);   // no width match → white
        return true;
    }

    // ── Per-frame horizontal box-average → one float row each (oldest→newest) ─
    // Luma is folded into all three channels when !colorMode so the band-fill
    // pass below is channel-agnostic.
    std::vector<float> rowsR((size_t) used * (size_t) bufW_);
    std::vector<float> rowsG((size_t) used * (size_t) bufW_);
    std::vector<float> rowsB((size_t) used * (size_t) bufW_);
    std::vector<int>   pfR(count + 1), pfG(count + 1), pfB(count + 1);

    for (int u = 0; u < used; ++u)
    {
        const int f = valid[u];
        const uint8_t* fr = capR_[f].data();
        const uint8_t* fg = capG_[f].data();
        const uint8_t* fb = capB_[f].data();
        float* oR = &rowsR[(size_t) u * (size_t) bufW_];
        float* oG = &rowsG[(size_t) u * (size_t) bufW_];
        float* oB = &rowsB[(size_t) u * (size_t) bufW_];

        if (colorMode)
        {
            pfR[0] = pfG[0] = pfB[0] = 0;
            for (int i = 0; i < count; ++i)
            {
                pfR[i + 1] = pfR[i] + fr[i];
                pfG[i + 1] = pfG[i] + fg[i];
                pfB[i + 1] = pfB[i] + fb[i];
            }
            for (int x = 0; x < bufW_; ++x)
            {
                int a = col0[x], b = col0[x + 1];
                if (b <= a) b = juce::jmin(count, a + 1);
                const float inv = 1.f / (float) (b - a);
                oR[x] = (float) (pfR[b] - pfR[a]) * inv;
                oG[x] = (float) (pfG[b] - pfG[a]) * inv;
                oB[x] = (float) (pfB[b] - pfB[a]) * inv;
            }
        }
        else
        {
            // Derive luma (the ring stores r/g/b only): 0.299/0.587/0.114 via the
            // 77/150/29 fixed-point form used in captureCurrentFrame().
            pfR[0] = 0;
            for (int i = 0; i < count; ++i)
            {
                const int gy = ((int) fr[i] * 77 + (int) fg[i] * 150 + (int) fb[i] * 29) >> 8;
                pfR[i + 1] = pfR[i] + gy;
            }
            for (int x = 0; x < bufW_; ++x)
            {
                int a = col0[x], b = col0[x + 1];
                if (b <= a) b = juce::jmin(count, a + 1);
                const float gy = (float) (pfR[b] - pfR[a]) / (float) (b - a);
                oR[x] = oG[x] = oB[x] = gy;
            }
        }
    }

    // ── Map the `used` rows onto the band as a time gradient ──────────────────
    // The birth line (band centre) is the NEWEST row; rows age away from it on
    // both sides — mirroring how scrollStep() pushes both zones away from the
    // birth line. The gradient spans the `coreH` motion rows; a taller band
    // (Thickness) keeps a clean bar by repeating the newest row past the core.
    // `span` box-averages a group of frames per row when the tick delivered more
    // lines than the core can show (honest decimation, never a total collapse).
    const float center   = (float) (bandH - 1) * 0.5f;
    const float coreHalf = juce::jmax(1.0f, (float) coreH * 0.5f);
    const float span     = juce::jmax(1.0f, (float) used / coreHalf);

    int prevLo = -1, prevHi = -1;
    for (int r = 0; r < bandH; ++r)
    {
        const float d    = std::abs((float) r - center);
        const float ageN = juce::jmin(1.0f, d / coreHalf);         // 0 birth → 1 core edge
        const float fpos = (float) (used - 1) * (1.0f - ageN);     // newest at the birth line
        int lo = (int) std::floor(fpos - span * 0.5f);
        int hi = (int) std::ceil (fpos + span * 0.5f);
        lo = juce::jlimit(0, used - 1, lo);
        hi = juce::jlimit(0, used,     hi);
        if (hi <= lo) hi = juce::jmin(used, lo + 1);

        auto* dst = bmp.getLinePointer(r);
        if (r > 0 && lo == prevLo && hi == prevHi)
        {
            std::memcpy(dst, bmp.getLinePointer(r - 1), rowBytes);   // same group → reuse
            continue;
        }
        prevLo = lo; prevHi = hi;

        const float invN = 1.f / (float) (hi - lo);
        for (int x = 0; x < bufW_; ++x)
        {
            float sr = 0.f, sg = 0.f, sb = 0.f;
            for (int u = lo; u < hi; ++u)
            {
                const size_t idx = (size_t) u * (size_t) bufW_ + (size_t) x;
                sr += rowsR[idx]; sg += rowsG[idx]; sb += rowsB[idx];
            }
            int rr = (int) (sr * invN + 0.5f);
            int gv = (int) (sg * invN + 0.5f);
            int bb = (int) (sb * invN + 0.5f);
            if (invMode == 1)          // Negative — flip each channel
            {
                rr = 255 - rr; gv = 255 - gv; bb = 255 - bb;
            }
            else if (invMode == 2)     // Luminance only — invert HSL lightness
            {
                // Inverting L in HSL while keeping hue+saturation is exactly a
                // uniform per-channel shift by (1 - (max+min)) [proof: chroma
                // C = (1-|2L-1|)·S is unchanged by L→1-L, so only the L-C/2
                // offset moves, by the same amount on every channel]. In 0..255:
                // delta = 255 - max - min. No clipping possible (new range stays
                // in [1-max, 1-min]), but the write below jlimits anyway.
                const int mx = juce::jmax(rr, gv, bb);
                const int mn = juce::jmin(rr, gv, bb);
                const int delta = 255 - mx - mn;
                rr += delta; gv += delta; bb += delta;
            }
            auto* dp = reinterpret_cast<juce::PixelRGB*>(dst + x * dps);
            dp->setARGB(255,
                        (juce::uint8) juce::jlimit(0, 255, rr),
                        (juce::uint8) juce::jlimit(0, 255, gv),
                        (juce::uint8) juce::jlimit(0, 255, bb));
        }
    }

    return true;
}

//==============================================================================
// buildWarp — the expensive per-pixel pass: warp (compression time-squish) + age
// (fade) the clean linear history into warpBuf_. Independent of the destination
// size, so it is computed ONCE per tick and shared by every view (column + window).
// Returns true when warpBuf_ was actually rebuilt (false on a cache hit), so the
// mixer knows whether its views need a repaint at all.
//==============================================================================
bool VideoScrollRenderCore::buildWarp()
{
    // No history yet (size 0 / slot has no frames) → nothing to warp.
    if (!buffersInit_ || bufW_ <= 0 || bufH_ <= 0 || compH_ <= 0) { warpReady_ = false; return false; }

    const juce::Image& shown = history_;
    if (!shown.isValid()) { warpReady_ = false; return false; }

    // ── Cache: reuse the previous warpBuf_ when nothing that shapes it changed ──
    // (frozen history + identical linePos/compress/fade + same render size). This
    // is what makes a paused output free. zoom/mode are applied later in drawWarp,
    // so they are deliberately NOT part of this signature.
    const float pLinePos  = param("linePos",  1.f);
    const float pCompress = param("compress", 1.f);
    const float pFade     = param("fade",     0.f);
    const float pGamma    = param("gamma",    1.f);
    if (warpReady_ && !warpDirty_
        && pLinePos == wsLinePos_ && pCompress == wsCompress_ && pFade == wsFade_
        && pGamma == wsGamma_
        && bufW_ == wsBufW_ && compH_ == wsCompH_)
        return false;
    wsLinePos_ = pLinePos; wsCompress_ = pCompress; wsFade_ = pFade; wsGamma_ = pGamma;
    wsBufW_ = bufW_; wsCompH_ = compH_; warpDirty_ = false;

    // ── Birth line (the "source" the effects radiate from) ───────────────────
    const float posParam = juce::jlimit(-1.f, 1.f, param("linePos", 1.f));
    const float posNorm  = (posParam + 1.f) * 0.5f;
    const int   birthBuf    = juce::jlimit(0, bufH_, (int) (posNorm * (float) bufH_));
    const float birthScreen = posNorm * (float) compH_;
    const float upSpan   = juce::jmax(1.0f, birthScreen);                 // px above
    const float downSpan = juce::jmax(1.0f, (float) compH_ - birthScreen);// px below

    // ── Distance-driven display effects (applied on a clean linear buffer) ───
    //   Compression → non-linear deceleration / time-squish.
    //   Fade        → progressive aging (dim + desaturate + horizontal blur).
    // The legacy "videoScrollMaxDuration" (1..64) is now the per-instance
    // "compress" param (same range/normalisation).
    const float comp01 = juce::jlimit(0.f, 1.f, (param("compress", 1.f) - 1.0f) / 63.0f);
    const float fade01 = juce::jlimit(0.f, 1.f, param("fade", 0.f));

    // Gamma gain LUT (photo convention pow(x, 1/gamma): >1 brightens midtones,
    // identity at 1 — endpoints 0/255 are fixed, so the paper-white background
    // stays white). Applied per channel as the last step of the per-pixel pass.
    const float gammaVal = juce::jlimit(0.01f, 10.0f, pGamma);
    uint8_t gammaLut[256];
    for (int i = 0; i < 256; ++i)
        gammaLut[i] = (uint8_t) juce::jlimit(0, 255, (int) std::lround(
            255.0 * std::pow((double) i / 255.0, 1.0 / (double) gammaVal)));

    if (warpBuf_.getWidth() != bufW_ || warpBuf_.getHeight() != compH_)
        warpBuf_ = juce::Image(juce::Image::RGB, juce::jmax(1, bufW_),
                               juce::jmax(1, compH_), false, juce::SoftwareImageType());

    constexpr float kCompMax = 2.5f;   // squish strength (gentle, non-linear)
    constexpr float kBlurMax = 8.0f;   // far-edge fade blur radius (px) at fade=1
    const float cComp = comp01 * kCompMax;

    // Aging factor a∈[0,1]: 0 at the source, 1 at the nearest viewport edge.
    auto agingAt = [&](float y) -> float
    {
        const float d = y - birthScreen;
        return juce::jmin(1.0f, std::abs(d) / ((d < 0.f) ? upSpan : downSpan));
    };

    // Screen→buffer edge map: signed offset that grows quadratically with the
    // screen distance from the source (G'(0)=1 → full speed at the line).
    auto bufEdge = [&](int y) -> int
    {
        const float d  = (float) y - birthScreen;
        const float ad = std::abs(d);
        const float sp = (d < 0.f) ? upSpan : downSpan;
        const float gg = ad + cComp * ad * ad / sp;     // buffer px from birth
        return birthBuf + (int) std::lround(d < 0.f ? -gg : gg);
    };

    // Precompute the compH_+1 buffer edges (one linear walk, not per pixel).
    if ((int) warpEdge_.size() != compH_ + 1) warpEdge_.resize(compH_ + 1);
    for (int y = 0; y <= compH_; ++y)
        warpEdge_[y] = bufEdge(y);

    if ((int) accR_.size() != bufW_)
    { accR_.assign(bufW_, 0); accG_.assign(bufW_, 0); accB_.assign(bufW_, 0); }

    {
        const juce::Image::BitmapData srcBmp(shown,    juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData       dstBmp(warpBuf_, juce::Image::BitmapData::writeOnly);
        const int dps = dstBmp.pixelStride;
        const int sps = srcBmp.pixelStride;   // 3 (packed) or 4 (native ARGB) — must respect

        for (int y = 0; y < compH_; ++y)
        {
            int lo = warpEdge_[y];
            int hi = warpEdge_[y + 1];
            if (hi < lo) { const int t = lo; lo = hi; hi = t; }
            lo = juce::jlimit(0, bufH_, lo);
            hi = juce::jlimit(0, bufH_, hi);
            if (hi <= lo) hi = juce::jmin(bufH_, lo + 1);
            const int cnt = juce::jmax(1, hi - lo);

            // Box-average every buffer row this screen row covers (proper
            // anti-aliased downsample — contiguous spans, each row read once).
            int* aR = accR_.data();
            int* aG = accG_.data();
            int* aB = accB_.data();
            for (int by = lo; by < hi; ++by)
            {
                const uint8_t* srcLine = srcBmp.getLinePointer(by);
                if (by == lo)
                    for (int x = 0; x < bufW_; ++x)
                    {
                        const auto* p = reinterpret_cast<const juce::PixelRGB*>(srcLine + x * sps);
                        aR[x] = p->getRed(); aG[x] = p->getGreen(); aB[x] = p->getBlue();
                    }
                else
                    for (int x = 0; x < bufW_; ++x)
                    {
                        const auto* p = reinterpret_cast<const juce::PixelRGB*>(srcLine + x * sps);
                        aR[x] += p->getRed(); aG[x] += p->getGreen(); aB[x] += p->getBlue();
                    }
            }

            const float a   = agingAt((float) y + 0.5f);
            const float dim = 1.0f - fade01 * a;
            const float sat = fade01 * a;
            const float k   = dim / (float) cnt;   // fold averaging + dim

            auto* dstLine = dstBmp.getLinePointer(y);
            if (sat <= 0.f)
            {
                for (int x = 0; x < bufW_; ++x)
                {
                    auto* dp = reinterpret_cast<juce::PixelRGB*>(dstLine + x * dps);
                    dp->setARGB(255,
                        gammaLut[juce::jlimit(0, 255, (int) ((float) aR[x] * k + 0.5f))],
                        gammaLut[juce::jlimit(0, 255, (int) ((float) aG[x] * k + 0.5f))],
                        gammaLut[juce::jlimit(0, 255, (int) ((float) aB[x] * k + 0.5f))]);
                }
            }
            else
            {
                const float invCnt = 1.0f / (float) cnt;
                for (int x = 0; x < bufW_; ++x)
                {
                    float r  = (float) aR[x] * invCnt;
                    float gv = (float) aG[x] * invCnt;
                    float bl = (float) aB[x] * invCnt;
                    const float lum = 0.299f * r + 0.587f * gv + 0.114f * bl;
                    r  = (r  + (lum - r ) * sat) * dim;
                    gv = (gv + (lum - gv) * sat) * dim;
                    bl = (bl + (lum - bl) * sat) * dim;
                    auto* dp = reinterpret_cast<juce::PixelRGB*>(dstLine + x * dps);
                    dp->setARGB(255,
                        gammaLut[juce::jlimit(0, 255, (int) (r  + 0.5f))],
                        gammaLut[juce::jlimit(0, 255, (int) (gv + 0.5f))],
                        gammaLut[juce::jlimit(0, 255, (int) (bl + 0.5f))]);
                }
            }
        }
    }

    // ── Horizontal fade blur: per-row box blur whose radius grows with the
    //    distance from the source. Running-sum prefix → O(width)/row. ─────────
    if (fade01 > 0.f)
    {
        juce::Image::BitmapData bmp(warpBuf_, juce::Image::BitmapData::readWrite);
        const int ps = bmp.pixelStride;
        if ((int) psR_.size() != bufW_ + 1) { psR_.resize(bufW_ + 1); psG_.resize(bufW_ + 1); psB_.resize(bufW_ + 1); }

        for (int y = 0; y < compH_; ++y)
        {
            const int hr = (int) std::lround(fade01 * agingAt((float) y + 0.5f) * kBlurMax);
            if (hr <= 0) continue;

            auto* line = bmp.getLinePointer(y);
            psR_[0] = psG_[0] = psB_[0] = 0;
            for (int x = 0; x < bufW_; ++x)
            {
                const auto* p = reinterpret_cast<const juce::PixelRGB*>(line + x * ps);
                psR_[x + 1] = psR_[x] + p->getRed();
                psG_[x + 1] = psG_[x] + p->getGreen();
                psB_[x + 1] = psB_[x] + p->getBlue();
            }
            for (int x = 0; x < bufW_; ++x)
            {
                const int x0 = juce::jmax(0, x - hr);
                const int x1 = juce::jmin(bufW_, x + hr + 1);   // exclusive
                const int cnt = x1 - x0;
                auto* dp = reinterpret_cast<juce::PixelRGB*>(line + x * ps);
                dp->setARGB(255,
                            (juce::uint8) ((psR_[x1] - psR_[x0]) / cnt),
                            (juce::uint8) ((psG_[x1] - psG_[x0]) / cnt),
                            (juce::uint8) ((psB_[x1] - psB_[x0]) / cnt));
            }
        }
    }

    warpReady_ = true;   // warpBuf_ now holds the current warped/aged frame.
    return true;
}

//==============================================================================
// drawWarp — the cheap per-destination pass: zoom / orientation / scale the
// prebuilt warpBuf_ into `g`. Called once per view; does NO per-pixel warp work.
//==============================================================================
void VideoScrollRenderCore::drawWarp(juce::Graphics& g, int destW, int destH)
{
    // Background/frame colour: fills the viewport wherever the zoomed/rotated
    // image doesn't reach (negative-zoom border) and the "no warp yet" state.
    // Default white (1,1,1) = the previous hard-coded blank paper.
    const juce::Colour bg = juce::Colour::fromFloatRGBA(
        juce::jlimit(0.f, 1.f, param("bgR", 1.f)),
        juce::jlimit(0.f, 1.f, param("bgG", 1.f)),
        juce::jlimit(0.f, 1.f, param("bgB", 1.f)), 1.f);
    g.fillAll(bg);
    if (!warpReady_ || !warpBuf_.isValid() || bufW_ <= 0 || compH_ <= 0)
        return;

    // ── Zoom + orientation rotation (applied to the warped/aged image) ───────
    //   Deg0 (0°) → vertical, Deg90/270 → horizontal, Deg180 → flipped vertical.
    const float zoom = juce::jlimit(0.5f, 4.0f, param("zoom", 1.f));
    const int modeVal = juce::jlimit(0, 3, (int) param("mode", 0.f));
    const float angle = (float) modeVal * juce::MathConstants<float>::halfPi;

    const float cw = (float) destW;
    const float ch = (float) destH;
    const float cx = cw * 0.5f;
    const float cy = ch * 0.5f;

    // Uniform "fit" placement: ONE scale factor, sized on the ROTATED footprint
    // (90°/270° swap width/height), so the whole frame sits inside the viewport
    // at zoom 1 whatever the window aspect. zoom is relative to that fit
    // (> 1 magnifies, < 1 shrinks into the bg border).
    const float sw = (float) bufW_;
    const float sh = (float) compH_;
    const bool  swapWH = (modeVal & 1) != 0;
    const float fitW = swapWH ? sh : sw;
    const float fitH = swapWH ? sw : sh;
    const float s = juce::jmin(cw / fitW, ch / fitH) * zoom;

    juce::AffineTransform t =
        juce::AffineTransform::translation(sw * -0.5f, sh * -0.5f)
            .rotated(angle)
            .scaled(s)
            .translated(cx, cy);

    // Medium-quality resampling: the warp pass already did a proper box-average
    // downsample, so the final blit is a mild rescale — medium is visually
    // indistinguishable from high here but far cheaper than a high-quality
    // resample of a multi-megapixel image every frame.
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
    g.drawImageTransformed(warpBuf_, t);
}
