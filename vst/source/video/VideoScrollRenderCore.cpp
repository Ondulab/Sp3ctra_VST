#include "VideoScrollRenderCore.h"
#include "../PluginProcessor.h"   // Sp3ctraAudioProcessor, vsParam(), getAPVTS()

// Per-instance capture ring API. video_scroll.h already wraps its declarations in
// its own extern "C" guard, so a plain include is correct (see file header note).
#include "../processing/video_scroll.h"
#include "VideoScrollMode.h"   // VideoScrollLimits (zoom bounds)
#include "VideoParallel.h"      // parallelChunks — the per-row fan-out
#include "VideoBlit.h"          // affineARGB — the zoom/rotate blit

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
// Output geometry — zoom / centre / birth line. See
// docs/PLAN_VIDEO_SCROLL_CHAIN_PAGES_ZOOM.md (D6/D7): the canvas is ALWAYS the
// whole output window (the sweep spans it edge to edge); zoom is the width of
// the generation band on the transverse axis, applied rigidly at draw time,
// and the centre params move the band (X) and the zoom frame hosting the
// birth line (Y).
//==============================================================================
float VideoScrollRenderCore::zoomParam() const
{
    return juce::jlimit(VideoScrollLimits::kZoomMin, VideoScrollLimits::kZoomMax,
                        param("zoom", 1.f));
}

float VideoScrollRenderCore::rotationRad() const
{
    // "rotation" in degrees, clockwise on screen (JUCE y-down rotation); 0 =
    // new lines at the bottom / scroll up, 90 = at the left, 180 = scroll
    // down, 270 = at the right — the former mode × 90°. Wraps.
    const float deg = std::fmod(param("rotation", 0.f), VideoScrollLimits::kRotationMax);
    return juce::degreesToRadians(deg < 0.f ? deg + VideoScrollLimits::kRotationMax : deg);
}

void VideoScrollRenderCore::visibleSpans(float& sx, float& sy) const
{
    // Bounding box of the view (viewW_ × viewH_) rotated into the canvas
    // frame: sx = W|cos θ| + H|sin θ|, sy = W|sin θ| + H|cos θ| (canvas px).
    // Never exceeds the canvas side (= the view diagonal).
    const float th = rotationRad();
    const float c  = std::abs(std::cos(th)), sn = std::abs(std::sin(th));
    const float W  = (float) juce::jmax(1, viewW_), Hh = (float) juce::jmax(1, viewH_);
    sx = juce::jmin((float) juce::jmax(1, compW_), W * c  + Hh * sn);
    sy = juce::jmin((float) juce::jmax(1, compH_), W * sn + Hh * c);
}

void VideoScrollRenderCore::frameSpans(float& fx, float& fy) const
{
    // Rotation-invariant: the view dims themselves (never the rotated bounding
    // box), capped at the canvas like visibleSpans.
    fx = juce::jmin((float) juce::jmax(1, compW_), (float) juce::jmax(1, viewW_));
    fy = juce::jmin((float) juce::jmax(1, compH_), (float) juce::jmax(1, viewH_));
}

void VideoScrollRenderCore::centreOffset(float& ox, float& oy) const
{
    // centerX/centerY are normalised WINDOW offsets (±1 = the generation
    // centre on the window edge; X → right, Y → down whatever the rotation).
    // Scale them by the view half-dims (canvas px), then rotate by −θ into
    // the canvas frame (drawWarp rotates the canvas by +θ).
    const float vx = juce::jlimit(-1.f, 1.f, param("centerX", 0.f)) * 0.5f * (float) juce::jmax(1, viewW_);
    const float vy = juce::jlimit(-1.f, 1.f, param("centerY", 0.f)) * 0.5f * (float) juce::jmax(1, viewH_);
    const float th = rotationRad();
    const float c  = std::cos(th), sn = std::sin(th);
    ox =  vx * c + vy * sn;
    oy = -vx * sn + vy * c;
}

float VideoScrollRenderCore::birthLine01() const
{
    // The zoom frame (z × view height — rotation-invariant, see frameSpans —
    // centred on the canvas centre + Center Y) hosts the birth line through
    // Line Pos exactly as the whole viewport used to; the frame no longer
    // clips the history, which flows on to the visible edge. Clamped to the
    // VISIBLE span so the line always stays on screen (a frame pushed
    // off-screen keeps generating at the edge).
    if (compH_ <= 0) return 1.0f;
    float spanX = 0.f, spanY = 0.f;
    visibleSpans(spanX, spanY);
    float frameX = 0.f, frameY = 0.f;
    frameSpans(frameX, frameY);
    float ox = 0.f, oy = 0.f;
    centreOffset(ox, oy);
    const float posNorm = (juce::jlimit(-1.f, 1.f, param("linePos", 1.f)) + 1.f) * 0.5f;
    const float mid   = 0.5f * (float) compH_;
    const float birth = mid + oy + (posNorm - 0.5f) * zoomParam() * frameY;
    return juce::jlimit(0.f, 1.f,
        juce::jlimit(mid - 0.5f * spanY, mid + 0.5f * spanY, birth) / (float) compH_);
}

//==============================================================================
// Display colour law — see the header note. The stamped lines, the blank paper
// and the frame border all go through THIS, so Invert / Color never leave one
// of them behind (a white border around a Luminance-inverted black image).
//==============================================================================
int VideoScrollRenderCore::invertMode() const
{
    // 0 Off / 1 Negative (255-RGB) / 2 Luminance (invert HSL lightness, keep
    // hue+saturation). Fall back to the legacy "invert" bool (== Negative) when
    // a pre-migration session left invertMode at Off.
    int invMode = (int) param("invertMode", 0.f);
    if (invMode == 0 && param("invert", 0.f) > 0.5f) invMode = 1;
    return invMode;
}

void VideoScrollRenderCore::applyDisplayColour(int& r, int& g, int& b,
                                               bool colorMode, int invMode)
{
    if (! colorMode)
    {
        // Luma 0.299/0.587/0.114 via the 77/150/29 fixed-point form used in
        // captureCurrentFrame().
        r = g = b = (r * 77 + g * 150 + b * 29) >> 8;
    }
    if (invMode == 1)          // Negative — flip each channel
    {
        r = 255 - r; g = 255 - g; b = 255 - b;
    }
    else if (invMode == 2)     // Luminance only — invert HSL lightness
    {
        // Inverting L in HSL while keeping hue+saturation is exactly a uniform
        // per-channel shift by (1 - (max+min)) [proof: chroma C = (1-|2L-1|)·S
        // is unchanged by L→1-L, so only the L-C/2 offset moves, by the same
        // amount on every channel]. In 0..255: delta = 255 - max - min. No
        // clipping possible (new range stays in [1-max, 1-min]), but the
        // jlimit below guards anyway.
        const int mx = juce::jmax(r, g, b);
        const int mn = juce::jmin(r, g, b);
        const int delta = 255 - mx - mn;
        r += delta; g += delta; b += delta;
    }
    r = juce::jlimit(0, 255, r);
    g = juce::jlimit(0, 255, g);
    b = juce::jlimit(0, 255, b);
}

juce::Colour VideoScrollRenderCore::displayColourOf(float r01, float g01, float b01) const
{
    int r = (int) std::lround(juce::jlimit(0.f, 1.f, r01) * 255.f);
    int g = (int) std::lround(juce::jlimit(0.f, 1.f, g01) * 255.f);
    int b = (int) std::lround(juce::jlimit(0.f, 1.f, b01) * 255.f);
    applyDisplayColour(r, g, b, param("colorMode", 1.f) > 0.5f, invertMode());
    return juce::Colour((juce::uint8) r, (juce::uint8) g, (juce::uint8) b);
}

juce::Colour VideoScrollRenderCore::frameColour() const
{
    // Frame/border colour (SETUP face), expressed in the SOURCE space like the
    // paper — default white = the chain's blank paper. Pushed through the same
    // Invert / Color law as the image so the border always matches the paper.
    return displayColourOf(param("bgR", 1.f), param("bgG", 1.f), param("bgB", 1.f));
}

void VideoScrollRenderCore::fillPaperRow(uint8_t* row, int pixelStride, juce::Colour paper) const
{
    const juce::uint8 r = paper.getRed(), g = paper.getGreen(), b = paper.getBlue();
    for (int x = 0; x < bufW_; ++x)
    {
        auto* p = reinterpret_cast<juce::PixelARGB*>(row + x * pixelStride);
        p->setARGB(255, r, g, b);
    }
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
        // BEFORE touching the dimensions: the rings are expressed in the
        // CURRENT bufW_/bufH_, and rolling them back to linear walks the
        // existing buffer with them. Doing it after the reassignment would
        // stride the old image with the new width.
        linearizeHistory();

        compW_ = w;
        compH_ = h;
        bufW_  = w;
        // 4× headroom: birth-line panning + extra history for the Compression
        // time-squish to actually have older content to pack into the far field.
        const int newBufH = juce::jmax(2, h * 4);
        // ARGB + SoftwareImageType → a guaranteed packed 4-BYTE layout. Four
        // bytes rather than three is what lets every pass here address pixels as
        // aligned 32-bit units: the compiler can vectorise the warp, and the
        // final blit can hand the buffer straight to videoblit (which needs a
        // 4-byte stride). The extra third of memory buys back several times its
        // cost in bandwidth. SoftwareImageType stays mandatory: these buffers are
        // written from the VIDEO MIX render thread, and JUCE 8's Direct2D images
        // (the Windows default) fault when read there.
        // Blank paper = white in the source space (an empty chain streams white,
        // never black), pushed through the Invert / Color law like every pixel.
        juce::Image next(juce::Image::ARGB, bufW_, newBufH, true, juce::SoftwareImageType());
        next.clear(next.getBounds(), paperColour());
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
        upOff_ = loOff_ = 0;         // fresh buffer → the rings start linear
        ringBirth_ = -1;
    }
}

void VideoScrollRenderCore::setDisplaySize(int w, int h)
{
    // Square canvas on the view DIAGONAL: the rotated view fits inside it at
    // ANY angle, so a continuous rotation (even automated) never reallocates
    // or rescales the history. Cost stays bounded because the mixer budgets
    // the diagonal (kMaxRenderDim) rather than the longest side.
    if (w <= 0 || h <= 0) return;
    viewW_ = w;
    viewH_ = h;
    const int d = juce::jmax(2, (int) std::ceil(std::hypot((double) w, (double) h)));
    allocateScrollBuffer(d, d);
}

//==============================================================================
// History ring addressing. The waterfall pushes content AWAY from the birth
// line in BOTH directions, so it is two back-to-back waterfalls, not one: the
// physical rows below the split hold the upper zone, those above it the lower
// zone, and each rotates independently. Reading a logical row is one add and a
// conditional subtract; advancing the whole history is two integer updates
// instead of the full-buffer shift the previous model performed every tick.
//==============================================================================
int VideoScrollRenderCore::histRow(int y) const noexcept
{
    if (ringBirth_ < 0) return y;                 // linear layout (no rotation)
    const int U = ringBirth_;
    if (y < U)
    {
        const int p = upOff_ + y;                 // upOff_ ∈ [0,U) → p < 2U
        return p < U ? p : p - U;
    }
    const int L = bufH_ - U;
    if (L <= 0) return y;
    const int q = loOff_ + (y - U);               // loOff_ ∈ [0,L) → q < 2L
    return U + (q < L ? q : q - L);
}

void VideoScrollRenderCore::ensureScratch(int chunks)
{
    chunks = juce::jmax(1, chunks);
    if ((int) scratch_.size() < chunks) scratch_.resize((size_t) chunks);
    for (auto& sc : scratch_)
    {
        if ((int) sc.accR.size() != bufW_)
        { sc.accR.assign((size_t) bufW_, 0); sc.accG.assign((size_t) bufW_, 0);
          sc.accB.assign((size_t) bufW_, 0); }
        if ((int) sc.psR.size() != bufW_ + 1)
        { sc.psR.assign((size_t) bufW_ + 1, 0); sc.psG.assign((size_t) bufW_ + 1, 0);
          sc.psB.assign((size_t) bufW_ + 1, 0); }
    }
}

//==============================================================================
// Roll both rings back to identity, so the physical row order IS the logical
// one. Rotation by triple reversal: it needs a ONE-ROW temporary rather than a
// second full history buffer (which at record resolutions would be another
// ~70 MB per output). Called only when the split moves — the birth line is a
// parameter, so this is a user gesture, not a per-frame cost — and before any
// buffer resize, whose rescale reads the history in logical order.
//==============================================================================
void VideoScrollRenderCore::linearizeHistory()
{
    if (ringBirth_ < 0 || ! history_.isValid() || bufW_ <= 0 || bufH_ <= 0)
    { upOff_ = loOff_ = 0; ringBirth_ = -1; return; }

    const int U = juce::jlimit(0, bufH_, ringBirth_);
    const int L = bufH_ - U;
    if ((U <= 1 || upOff_ == 0) && (L <= 1 || loOff_ == 0))
    { upOff_ = loOff_ = 0; ringBirth_ = -1; return; }

    juce::Image::BitmapData bmp(history_, juce::Image::BitmapData::readWrite);
    const size_t rowBytes = (size_t) bufW_ * (size_t) bmp.pixelStride;
    std::vector<uint8_t> tmp(rowBytes);

    auto swapRows = [&](int a, int b)
    {
        uint8_t* ra = bmp.getLinePointer(a);
        uint8_t* rb = bmp.getLinePointer(b);
        std::memcpy(tmp.data(), ra, rowBytes);
        std::memcpy(ra, rb, rowBytes);
        std::memcpy(rb, tmp.data(), rowBytes);
    };
    auto reverseRows = [&](int lo, int hi)          // [lo, hi)
    { for (--hi; lo < hi; ++lo, --hi) swapRows(lo, hi); };
    // Rotate [base, base+n) LEFT by k: new[j] = old[(k+j) mod n].
    auto rotateLeft = [&](int base, int n, int k)
    {
        if (n <= 1) return;
        k %= n; if (k < 0) k += n;
        if (k == 0) return;
        reverseRows(base, base + k);
        reverseRows(base + k, base + n);
        reverseRows(base, base + n);
    };

    rotateLeft(0, U, upOff_);
    rotateLeft(U, L, loOff_);

    upOff_ = loOff_ = 0;
    ringBirth_ = -1;
}

//==============================================================================
void VideoScrollRenderCore::clear()
{
    if (history_.isValid())
        history_.clear(history_.getBounds(), paperColour());   // blank paper (through the law)
    upOff_ = loOff_ = 0;       // a uniform buffer is linear by definition
    ringBirth_ = -1;
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
// Flow envelopes — see flowNow()/flowPeak()/flowPower() in the header.
void VideoScrollRenderCore::noteFlow(int freshLines, double dtMs)
{
    // Instantaneous drive — an LED, not a VU: the ink of the lines that
    // ACTUALLY arrived this tick (1 − luminance in INPUT space — a white
    // input line is what the display law maps to the paper, whatever the
    // paper colour), full once a few percent of ink is there (a typical
    // sheet is mostly white — a handful of traces must still light the LED;
    // the exactly-white unfed sweep and sensor noise on blank paper stay
    // under the floor) × the share of the nominal rate that arrived,
    // saturating at a quarter of it.
    //
    // EVERY fresh line is weighed, not just the newest: at ~1000 lps a tick
    // drains a dozen or more, and judging a whole tick on one of them made
    // the reading flicker between outputs — the source of a burst could not
    // be told apart. Sub-sampled across the line; runs on the render thread.
    float x = 0.0f;
    if (freshLines > 0)
    {
        const int nLines = juce::jmin(freshLines, (int) capPx_.size());
        double inkSum = 0.0;
        int    inkCnt = 0;
        for (int li = 0; li < nLines; ++li)
        {
            const int px = capPx_[(size_t) li];
            if (px <= 0) continue;
            const auto& r = capR_[(size_t) li];
            const auto& g = capG_[(size_t) li];
            const auto& b = capB_[(size_t) li];
            int sum = 0, cnt = 0;
            for (int i = 0; i < px; i += 8)
            { sum += (int) r[(size_t) i] + g[(size_t) i] + b[(size_t) i]; ++cnt; }
            if (cnt > 0) { inkSum += 1.0 - (double) sum / (765.0 * (double) cnt); ++inkCnt; }
        }
        if (inkCnt > 0)
        {
            const float mean = (float) (inkSum / (double) inkCnt);
            const float ink  = juce::jlimit(0.0f, 1.0f, (mean - kInkFloor) / kInkFull);
            const float nominal = kNominalLps * (float) (juce::jlimit(1.0, 100.0, dtMs) / 1000.0);
            const float rate = juce::jlimit(0.0f, 1.0f,
                                            (float) inkCnt / juce::jmax(1.0f, 0.25f * nominal));
            x = ink * rate;
        }
    }
    const float dt   = (float) juce::jlimit(1.0, 100.0, dtMs);
    const float now  = juce::jmax(x, flowNow_.load (std::memory_order_relaxed) * std::exp(-dt / kFlowNowMs));
    const float peak = juce::jmax(x, flowPeak_.load(std::memory_order_relaxed) * std::exp(-dt / kFlowPeakMs));
    // The printed power follows the drive BOTH ways (no peak hold): it is a
    // level to read, not an activity light.
    const float k     = 1.0f - std::exp(-dt / kFlowPwrMs);
    const float prev  = flowPower_.load(std::memory_order_relaxed);
    const float power = prev + (x - prev) * k;
    flowNow_.store  (now   < 0.002f ? 0.0f : now,   std::memory_order_relaxed);
    flowPeak_.store (peak  < 0.002f ? 0.0f : peak,  std::memory_order_relaxed);
    flowPower_.store(power < 0.002f ? 0.0f : power, std::memory_order_relaxed);
}

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
        noteFlow(0, dtMs);   // frozen → the LED dies out
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
    // × zoom: the scroll axis is never rescaled at draw time (the canvas IS the
    // window), so the time density of a uniform reduction / magnification is
    // reproduced here — a 0.25× band advances a quarter as fast, as its lines
    // would after a 0.25× downscale.
    const float zoom   = zoomParam();
    const float pxRate = ((speedParam < 0.f) ? -mag : mag) * dtScale * zoom;
    const bool  reverse = (pxRate < 0.f);

    scrollAccumulator_ += std::abs(pxRate);
    const int scroll = (int) scrollAccumulator_;
    scrollAccumulator_ -= (float) scroll;

    // Fresh CIS frames captured since the previous tick (drained every tick so
    // capture never lags behind the display).
    int newestPx = 0;
    int captured = drainRing(&newestPx);
    noteFlow(captured, dtMs);   // FRESH lines only — before the bridge re-stamps

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

    // ── Birth-line position (zoom frame + Line Pos — see birthLine01) ────────
    const int   birthY   = juce::jlimit(0, bufH_, (int) (birthLine01() * (float) bufH_));

    // ── Line geometry ────────────────────────────────────────────────────────
    //   coreH : data rows that exactly fill the motion gap (2*scroll px) at a
    //           fixed 1-row-per-time-slice scale (keeps the time-scale constant).
    //   bandH : total stamped height. Thickness DUPLICATES the birth line onto
    //           the rows above/below it (see buildLineImage): a clean fat bar.
    const float thickParam  = juce::jlimit(0.f, 1.f, param("thickness", 0.f));
    // Full thickness = the zoom FRAME height (z × view height, rotation-
    // invariant), like the uniform reduction it stands for; never more than
    // the canvas.
    float frameX = 0.f, frameY = 0.f;
    frameSpans(frameX, frameY);
    const int   thicknessPx = juce::jlimit(1, juce::jmax(1, compH_),
        (int) (1.0f + thickParam * (zoom * frameY - 1.0f)));
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

    // ── Advance the two rings ────────────────────────────────────────────────
    // Moving the history is now two integer updates instead of shifting the
    // whole 4×canvas buffer: the zones rotate, and only the rows the scroll
    // VACATED (which the stamp band then covers) are actually written. The
    // split must describe the current birth line, so when the user moves the
    // line the rings are first rolled back to linear — the only path left here
    // that touches the whole buffer, and it runs on a gesture, not per frame.
    if (ringBirth_ != birthY)
    {
        linearizeHistory();
        ringBirth_ = birthY;
        upOff_ = loOff_ = 0;
    }

    const int upSize = juce::jlimit(0, bufH_, birthY);      // upper zone rows
    const int loSize = bufH_ - upSize;                      // lower zone rows

    auto rotUp = [&](int d) { if (upSize > 0) { int v = (upOff_ + d) % upSize; upOff_ = v < 0 ? v + upSize : v; } };
    auto rotLo = [&](int d) { if (loSize > 0) { int v = (loOff_ + d) % loSize; loOff_ = v < 0 ? v + loSize : v; } };

    if (scroll > 0)
    {
        if (! reverse) { rotUp( scroll); rotLo(-scroll); }  // zones move AWAY from the line
        else           { rotUp(-scroll); rotLo( scroll); }  // zones converge TOWARD it
    }

    {
        juce::Image::BitmapData bmp(history_, juce::Image::BitmapData::readWrite);
        const size_t rowBytes = (size_t) bufW_ * (size_t) bmp.pixelStride;
        auto rowPtr = [&](int y) { return bmp.getLinePointer(histRow(y)); };
        const juce::Colour paper = paperColour();   // blank paper through the law

        if (scroll > 0)
        {
            const int upN = juce::jmin(scroll, upSize);
            const int loN = juce::jmin(scroll, loSize);
            if (! reverse)
            {
                // Forward: the vacated 2×scroll band sits against the birth line
                // and the stamp below covers it (bandH >= 2*scroll). With nothing
                // to stamp (source stopped past the hold window) it gets the
                // honest "no stream" blank paper — source-space white through the
                // Invert / Color law, so it matches the frame border.
                if (! haveLine)
                {
                    for (int y = upSize - upN; y < upSize; ++y)
                        fillPaperRow(rowPtr(y), bmp.pixelStride, paper);
                    for (int y = upSize; y < upSize + loN; ++y)
                        fillPaperRow(rowPtr(y), bmp.pixelStride, paper);
                }
            }
            else
            {
                // Reverse: the zones converge on the birth line, so it is the
                // OUTER edges that empty out; the stamp covers where they meet.
                for (int y = 0; y < upN; ++y)
                    fillPaperRow(rowPtr(y), bmp.pixelStride, paper);
                for (int y = bufH_ - loN; y < bufH_; ++y)
                    fillPaperRow(rowPtr(y), bmp.pixelStride, paper);
            }
        }

        // Stamp the fresh strip at the birth line (already bandH px tall).
        // lineImg is packed ARGB at bufW_ like the history → straight row copies.
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

    // Display colour law (shared with the paper and the frame border): Color
    // folds to luma during the box-average below, Invert is applied per pixel
    // at the end via applyDisplayColour().
    const int  invMode   = invertMode();
    const bool colorMode = param("colorMode", 1.f) > 0.5f;

    // The reference width is the newest captured line's pixel count.
    const int count = captureCount;
    if (count <= 0) return false;

    out = juce::Image(juce::Image::ARGB, bufW_, bandH, false, juce::SoftwareImageType());
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
        // No width match → blank paper (through the law, like the border).
        fillPaperRow(bmp.getLinePointer(0), dps, paperColour());
        for (int r = 1; r < bandH; ++r)
            std::memcpy(bmp.getLinePointer(r), bmp.getLinePointer(0), rowBytes);
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
            // Luma was already folded above when !colorMode → only Invert here.
            applyDisplayColour(rr, gv, bb, true, invMode);
            auto* dp = reinterpret_cast<juce::PixelARGB*>(dst + x * dps);
            dp->setARGB(255, (juce::uint8) rr, (juce::uint8) gv, (juce::uint8) bb);
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
    // (frozen history + identical birth line/compress/fade/blur/gamma + same
    // render size). This is what makes a paused output free. The transverse
    // zoom / Center X are applied later in drawWarp, so they are deliberately
    // NOT part of this signature; the birth line is the RESOLVED position
    // (linePos + zoom + centerY + mode folded by birthLine01), so any of those
    // moving invalidates the cache.
    const float pBirth    = birthLine01();
    const float pRot      = rotationRad();   // → visible span → aging distances
    const float pCompress = param("pack",     0.f);   // bipolar (2026-09-04)
    const float pFade     = param("fade",     0.f);
    const float pMidX     = param("fadeMidX", 0.5f);
    const float pMidY     = param("fadeMidY", 1.f);
    const float pMidFree  = param("fadeMidFree", 0.f);
    const float pBlur     = param("blur",     0.f);
    const float pGamma    = param("gamma",    1.f);
    if (warpReady_ && !warpDirty_
        && pBirth == wsBirth_ && pRot == wsRot_ && pCompress == wsCompress_
        && pFade == wsFade_ && pBlur == wsBlur_ && pGamma == wsGamma_
        && pMidX == wsMidX_ && pMidY == wsMidY_ && pMidFree == wsMidFree_
        && bufW_ == wsBufW_ && compH_ == wsCompH_
        && viewW_ == wsViewW_ && viewH_ == wsViewH_)
        return false;
    wsBirth_ = pBirth; wsRot_ = pRot; wsCompress_ = pCompress; wsFade_ = pFade;
    wsBlur_ = pBlur; wsGamma_ = pGamma;
    wsMidX_ = pMidX; wsMidY_ = pMidY; wsMidFree_ = pMidFree;
    wsBufW_ = bufW_; wsCompH_ = compH_; wsViewW_ = viewW_; wsViewH_ = viewH_;
    warpDirty_ = false;

    // ── Birth line + visible span ────────────────────────────────────────────
    // The canvas is a square on the view diagonal: only the rows inside the
    // rotated view's bounding box [rowLo, rowHi) can ever show, so the warp is
    // computed for those alone and the aging distances reach ITS edges (= the
    // output window). The history flows past the zoom frame, so the effects
    // age it all the way to the border.
    float spanX = 0.f, spanY = 0.f;
    visibleSpans(spanX, spanY);
    const int   rowLo = juce::jlimit(0, compH_, (int) std::floor(0.5f * ((float) compH_ - spanY)));
    const int   rowHi = juce::jlimit(rowLo, compH_, (int) std::ceil (0.5f * ((float) compH_ + spanY)));
    const int   birthBuf    = juce::jlimit(0, bufH_, (int) (pBirth * (float) bufH_));
    const float birthScreen = pBirth * (float) compH_;
    const float upSpan   = juce::jmax(1.0f, birthScreen - (float) rowLo);   // px above
    const float downSpan = juce::jmax(1.0f, (float) rowHi - birthScreen);   // px below

    // ── Distance-driven display effects (applied on a clean linear buffer) ───
    //   Compression → non-linear deceleration, BOTH ways since 2026-09-04:
    //                 pack > 0 squishes the far field (the historical
    //                 behaviour), pack < 0 spreads it back out.
    //   Attenuation → progressive level loss with the distance from the
    //                 source, shaped by the editable curve
    //                 (videoScrollAttenLevel), applied AFTER gamma
    //                 (2026-08-28 — it used to run before, so a lifting gamma
    //                 pulled the aged material back up and the knob read weak).
    //   Blur        → progressive horizontal smear (radius grows with the age).
    // Attenuation and Blur were one knob until 2026-08-28; they are independent
    // now so a line can age in brightness without smearing (and vice versa).
    const float fade01 = juce::jlimit(0.f, 1.f, pFade);
    const float blur01 = juce::jlimit(0.f, 1.f, pBlur);
    const bool  midFree = pMidFree > 0.5f;

    // Gamma gain LUT (photo convention pow(x, 1/gamma): >1 brightens midtones,
    // identity at 1 — endpoints 0/255 are fixed, so the paper-white background
    // stays white). Applied per channel as the last step of the per-pixel pass.
    const float gammaVal = juce::jlimit(0.01f, 10.0f, pGamma);
    uint8_t gammaLut[256];
    for (int i = 0; i < 256; ++i)
        gammaLut[i] = (uint8_t) juce::jlimit(0, 255, (int) std::lround(
            255.0 * std::pow((double) i / 255.0, 1.0 / (double) gammaVal)));

    if (warpBuf_.getWidth() != bufW_ || warpBuf_.getHeight() != compH_)
        warpBuf_ = juce::Image(juce::Image::ARGB, juce::jmax(1, bufW_),
                               juce::jmax(1, compH_), false, juce::SoftwareImageType());

    // Squish strength now lives with the bounds it shares with the UI
    // (VideoScrollLimits::kPackMin/kPackMax, via videoScrollPackCoeff).
    // Far-edge blur radius at blur = 1. The knob is squared before the map
    // (see the blur pass below) so the low end keeps its fine dosing — the old
    // 8 px full-scale now sits around knob ≈ 0.35. The box blur is prefix-sum
    // (O(width)/row whatever the radius), so a big radius costs nothing.
    constexpr float kBlurMax  = 64.0f;
    const float cComp = videoScrollPackCoeff(pCompress);

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

    // Per-chunk row scratch — buildWarp and the blur below fan out over
    // videoparallel, so no two threads may share an accumulator row.
    const int warpChunks = videoparallel::chunkCount();
    ensureScratch(warpChunks);
    // Smallest row slice worth a thread: below this the pass runs inline.
    constexpr int kWarpMinRows = 24;

    {
        const juce::Image::BitmapData srcBmp(shown,    juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData       dstBmp(warpBuf_, juce::Image::BitmapData::writeOnly);
        const int dps = dstBmp.pixelStride;
        const int sps = srcBmp.pixelStride;   // 4 — the history is packed ARGB

        // Rows are INDEPENDENT: each screen row box-averages its own span of
        // history rows and shades it. Fanning the range out over the cores is
        // what turns this pass from the video budget into a fraction of it —
        // it used to run alone on the render thread while eleven cores idled.
        videoparallel::parallelChunks(rowLo, rowHi, kWarpMinRows,
            [&](int chunk, int yA, int yB)
        {
            auto& sc = scratch_[(size_t) juce::jlimit(0, warpChunks - 1, chunk)];
            int* aR = sc.accR.data();
            int* aG = sc.accG.data();
            int* aB = sc.accB.data();

            for (int y = yA; y < yB; ++y)
            {
                int lo = warpEdge_[y];
                int hi = warpEdge_[y + 1];
                if (hi < lo) { const int t = lo; lo = hi; hi = t; }
                // Always resolve to at least ONE real history row. A row whose
                // compressed span falls past the end of the buffer used to leave
                // the accumulator holding the previous row's sum — harmless while
                // the pass was sequential, meaningless now that the previous row
                // may belong to another thread's chunk.
                lo = juce::jlimit(0, bufH_ - 1, lo);
                hi = juce::jlimit(lo + 1, bufH_, hi);
                const int cnt = hi - lo;

                // Box-average every buffer row this screen row covers (proper
                // anti-aliased downsample). The history is a RING, so the rows of
                // a span are contiguous LOGICALLY, not physically — histRow maps
                // each one (an add and a conditional subtract).
                for (int by = lo; by < hi; ++by)
                {
                    const uint8_t* srcLine = srcBmp.getLinePointer(histRow(by));
                    if (by == lo)
                        for (int x = 0; x < bufW_; ++x)
                        {
                            const auto* p = reinterpret_cast<const juce::PixelARGB*>(srcLine + x * sps);
                            aR[x] = p->getRed(); aG[x] = p->getGreen(); aB[x] = p->getBlue();
                        }
                    else
                        for (int x = 0; x < bufW_; ++x)
                        {
                            const auto* p = reinterpret_cast<const juce::PixelARGB*>(srcLine + x * sps);
                            aR[x] += p->getRed(); aG[x] += p->getGreen(); aB[x] += p->getBlue();
                        }
                }

                // Per-pixel chain: box-average → GAMMA → desaturate → dim. Gamma
                // shapes the averaged sample first; the attenuation then acts on
                // that value, so it can never be undone by a lifting gamma. The
                // law is the editable curve — THE same function the grid's
                // Attenuation cell draws, so the drawn curve is the obeyed one.
                // All three steps run in 16.16 fixed point: the row constants are
                // resolved once, and the inner loop keeps to integers so the
                // compiler can vectorise it (the float form could not).
                const float a   = agingAt((float) y + 0.5f);
                const float dim = (fade01 > 0.f || midFree)
                                    ? videoScrollAttenLevel(a, fade01, pMidX, pMidY, midFree)
                                    : 1.0f;
                const float sat = 1.0f - dim;

                const int invQ = (65536 + cnt / 2) / cnt;                     // 1/cnt
                const int dimQ = (int) std::lround(juce::jlimit(0.0f, 2.0f, dim) * 65536.0f);
                const int satQ = (int) std::lround(juce::jlimit(-1.0f, 1.0f, sat) * 65536.0f);

                auto* dstLine = dstBmp.getLinePointer(y);
                if (sat <= 0.001f)
                {
                    // No aging on this row: average → gamma, nothing else.
                    for (int x = 0; x < bufW_; ++x)
                    {
                        auto* dp = reinterpret_cast<juce::PixelARGB*>(dstLine + x * dps);
                        dp->setARGB(255,
                            gammaLut[juce::jlimit(0, 255, (aR[x] * invQ + 32768) >> 16)],
                            gammaLut[juce::jlimit(0, 255, (aG[x] * invQ + 32768) >> 16)],
                            gammaLut[juce::jlimit(0, 255, (aB[x] * invQ + 32768) >> 16)]);
                    }
                }
                else
                {
                    for (int x = 0; x < bufW_; ++x)
                    {
                        const int r  = gammaLut[juce::jlimit(0, 255, (aR[x] * invQ + 32768) >> 16)];
                        const int gv = gammaLut[juce::jlimit(0, 255, (aG[x] * invQ + 32768) >> 16)];
                        const int bl = gammaLut[juce::jlimit(0, 255, (aB[x] * invQ + 32768) >> 16)];
                        // Luma 0.299/0.587/0.114 in the 77/150/29 fixed-point form
                        // used everywhere else in this file.
                        const int lum = (r * 77 + gv * 150 + bl * 29) >> 8;
                        const int rr = (((r  + (((lum - r ) * satQ) >> 16)) * dimQ) >> 16);
                        const int gg = (((gv + (((lum - gv) * satQ) >> 16)) * dimQ) >> 16);
                        const int bb = (((bl + (((lum - bl) * satQ) >> 16)) * dimQ) >> 16);
                        auto* dp = reinterpret_cast<juce::PixelARGB*>(dstLine + x * dps);
                        dp->setARGB(255,
                            (juce::uint8) juce::jlimit(0, 255, rr),
                            (juce::uint8) juce::jlimit(0, 255, gg),
                            (juce::uint8) juce::jlimit(0, 255, bb));
                    }
                }
            }
        });
    }

    // ── Horizontal blur: per-row box blur whose radius grows with the distance
    //    from the source. Running-sum prefix → O(width)/row; software only (the
    //    whole warp is CPU-rendered, no GPU is involved anywhere in this pass).
    //    The radius is FRACTIONAL: the integer core [x-hr, x+hr] gets full
    //    weight and the two pixels just outside it get the fractional part, so
    //    the kernel widens continuously from row to row. The previous integer
    //    radius (lround) jumped 1→2→3 px between rows, which printed visible
    //    stair-steps across the aged zone. ────────────────────────────────────
    if (blur01 > 0.f)
    {
        juce::Image::BitmapData bmp(warpBuf_, juce::Image::BitmapData::readWrite);
        const int ps = bmp.pixelStride;

        // Each row blurs only itself (the kernel is horizontal), so the range
        // fans out exactly like the warp above — with per-chunk prefix sums, as
        // two threads must never share one.
        videoparallel::parallelChunks(rowLo, rowHi, kWarpMinRows,
            [&](int chunk, int yA, int yB)
        {
            auto& sc = scratch_[(size_t) juce::jlimit(0, warpChunks - 1, chunk)];
            int* psR = sc.psR.data();
            int* psG = sc.psG.data();
            int* psB = sc.psB.data();

            for (int y = yA; y < yB; ++y)
            {
                const float rf = blur01 * blur01 * agingAt((float) y + 0.5f) * kBlurMax;
                if (rf < 0.01f) continue;
                const int   hr = (int) rf;            // integer core half-width
                const float fr = rf - (float) hr;     // weight of the two outer taps

                auto* line = bmp.getLinePointer(y);
                psR[0] = psG[0] = psB[0] = 0;
                for (int x = 0; x < bufW_; ++x)
                {
                    const auto* p = reinterpret_cast<const juce::PixelARGB*>(line + x * ps);
                    psR[x + 1] = psR[x] + p->getRed();
                    psG[x + 1] = psG[x] + p->getGreen();
                    psB[x + 1] = psB[x] + p->getBlue();
                }
                for (int x = 0; x < bufW_; ++x)
                {
                    const int x0 = juce::jmax(0, x - hr);
                    const int x1 = juce::jmin(bufW_, x + hr + 1);   // exclusive
                    float sr = (float) (psR[x1] - psR[x0]);
                    float sg = (float) (psG[x1] - psG[x0]);
                    float sb = (float) (psB[x1] - psB[x0]);
                    float wsum = (float) (x1 - x0);
                    // Outer taps (one pixel beyond the core on each side), weighted
                    // by the fractional radius; clipped at the row ends.
                    if (x0 > 0)
                    {
                        sr += fr * (float) (psR[x0] - psR[x0 - 1]);
                        sg += fr * (float) (psG[x0] - psG[x0 - 1]);
                        sb += fr * (float) (psB[x0] - psB[x0 - 1]);
                        wsum += fr;
                    }
                    if (x1 < bufW_)
                    {
                        sr += fr * (float) (psR[x1 + 1] - psR[x1]);
                        sg += fr * (float) (psG[x1 + 1] - psG[x1]);
                        sb += fr * (float) (psB[x1 + 1] - psB[x1]);
                        wsum += fr;
                    }
                    const float inv = 1.0f / wsum;
                    auto* dp = reinterpret_cast<juce::PixelARGB*>(line + x * ps);
                    dp->setARGB(255,
                                (juce::uint8) juce::jlimit(0, 255, (int) (sr * inv + 0.5f)),
                                (juce::uint8) juce::jlimit(0, 255, (int) (sg * inv + 0.5f)),
                                (juce::uint8) juce::jlimit(0, 255, (int) (sb * inv + 0.5f)));
                }
            }
        });
    }

    warpReady_ = true;   // warpBuf_ now holds the current warped/aged frame.
    return true;
}

//==============================================================================
// drawWarp — the cheap per-destination pass: zoom / orientation / scale the
// prebuilt warpBuf_ into `g`. Called once per view; does NO per-pixel warp work.
//==============================================================================
juce::AffineTransform VideoScrollRenderCore::warpTransform(int destW, int destH) const
{
    // ── Band zoom / centre + continuous rotation ──────────────────────────────
    // The canvas (D × D, D = the view diagonal) is drawn 1:1 on the view's
    // diagonal and rotated about the window centre, so whatever the angle the
    // rotated view lies inside it and the sweep runs edge to edge. Zoom only
    // scales the TRANSVERSE axis: at zoom 1 the stamped line (D canvas px)
    // is fitted to the view WIDTH — rotation-invariant (frameSpans), so
    // turning never changes the apparent zoom — then Center X shifts the
    // band; the scroll axis is never rescaled (its time density follows
    // through scrollStep's × zoom).
    const float zoom  = zoomParam();
    const float theta = rotationRad();
    float frameX = 0.f, frameY = 0.f;
    frameSpans(frameX, frameY);
    float ox = 0.f, oy = 0.f;
    centreOffset(ox, oy);
    juce::ignoreUnused(oy, frameY);   // Center Y acts through the birth line (birthLine01)

    const float cw = (float) destW;
    const float ch = (float) destH;
    const float cx = cw * 0.5f;
    const float cy = ch * 0.5f;

    const float sw = (float) bufW_;
    const float sh = (float) compH_;
    // Uniform fit: the canvas side IS the (budgeted) view diagonal, so this is
    // the plain budget ratio in the mixer's steady state and a mild uniform
    // rescale while a resize is in flight.
    const float s         = std::hypot(cw, ch) / juce::jmax(1.0f, sw);
    const float bandScale = (frameX / juce::jmax(1.0f, sw)) * zoom;   // line → view width × zoom

    return juce::AffineTransform::translation(sw * -0.5f, sh * -0.5f)
               .scaled(bandScale, 1.0f)
               .translated(ox, 0.0f)
               .rotated(theta)
               .scaled(s)
               .translated(cx, cy);
}

//==============================================================================
// Fast path — the picture straight into a 4-byte ARGB target. videoblit's
// affine blit is backward mapped, fixed point and parallel per row, and it
// paints the border itself, so the separate full-surface fill that the
// Graphics path needs disappears with it. That fill plus JUCE's generic
// edge-table rasteriser were the single most expensive step of the chain at
// record resolutions. Falls back to the Graphics path for any other format.
//==============================================================================
void VideoScrollRenderCore::drawWarp(juce::Image& dest)
{
    if (! dest.isValid()) return;
    const int destW = dest.getWidth();
    const int destH = dest.getHeight();
    if (destW <= 0 || destH <= 0) return;

    const juce::Colour frame = frameColour();
    if (! warpReady_ || ! warpBuf_.isValid() || bufW_ <= 0 || compH_ <= 0)
    {
        dest.clear(dest.getBounds(), frame);
        return;
    }

    if (videoblit::affineARGB(dest, warpBuf_, warpTransform(destW, destH), frame))
        return;

    juce::Graphics g(dest);
    drawWarp(g, destW, destH);
}

//==============================================================================
void VideoScrollRenderCore::drawWarp(juce::Graphics& g, int destW, int destH)
{
    // Background/frame colour: fills the viewport wherever the zoomed/rotated
    // image doesn't reach (negative-zoom border) and the "no warp yet" state.
    // Default white (1,1,1) = the blank paper; pushed through the same Invert /
    // Color law as the image (frameColour), so Luminance/Negative inversion
    // turns the border black along with the paper instead of leaving a white
    // frame around a black image.
    g.fillAll(frameColour());
    if (!warpReady_ || !warpBuf_.isValid() || bufW_ <= 0 || compH_ <= 0)
        return;

    // Medium-quality resampling: the warp pass already did a proper box-average
    // downsample, so the final blit is a mild rescale — medium is visually
    // indistinguishable from high here but far cheaper than a high-quality
    // resample of a multi-megapixel image every frame.
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
    g.drawImageTransformed(warpBuf_, warpTransform(destW, destH));
}
