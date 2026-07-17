#include "VideoDisplayComponent.h"
#include "../PluginProcessor.h"

extern "C"
{
#include "audio/buffers/audio_image_buffers.h"
#include "config/config_loader.h"
#include "processing/image_pipeline_types.h"
#include "processing/lux_pitch.h"
#include "processing/lux_mask.h"
}

/* Cap LuxPitch output buffer copy at the engine's max pixel capacity.
 * Defined in processing/lux_pitch.h.  Avoids a redefinition warning when the
 * macro is reused locally. */

#include <cstring>
#include <cmath>
#include <vector>

//==============================================================================
VideoDisplayComponent::VideoDisplayComponent(Sp3ctraAudioProcessor& proc)
    : processor_(proc), captureThread_(*this)
{
    setOpaque(true);

    // Adopt the current transport clear-pulse so a freshly-created view doesn't
    // spuriously clear on its first tick (it already starts blank).
    lastClearGen_ = processor_.getVideoScrollClearGen();

    // Pre-allocate ring buffer entries with empty vectors.
    // Actual pixel count is set lazily in captureCurrentFrame().
    frameRing_.resize(kRingSize);

    captureThread_.startThread(juce::Thread::Priority::high);
    startTimerHz(kTimerFps);
}

VideoDisplayComponent::~VideoDisplayComponent()
{
    stopTimer();
    captureThread_.stopThread(500);
}

//==============================================================================
// CaptureThread::run()
// Polls AudioImageBuffers every ~1 ms, detects new scanlines via lines_received
// counter, and pushes them into the lock-free SPSC ring.
//==============================================================================
void VideoDisplayComponent::CaptureThread::run()
{
    while (!threadShouldExit())
    {
        owner_.captureCurrentFrame();
        sleep(1); // ~1 ms → polls at ~1000 Hz, matches CIS acquisition rate
    }
}

void VideoDisplayComponent::captureCurrentFrame()
{
    auto* core = processor_.getSp3ctraCore();
    if (!core) return;
    auto* aib = core->getAudioImageBuffers();
    if (!aib || !aib->initialized) return;

    // Detect new frame via the AudioImageBuffers generation counters
    // (lock-free).  Two counters are required because the producer differs
    // depending on which path drives the waterfall:
    //   • lines_received  — UDP thread, incremented on complete_write().
    //                       Frozen while the LuxSampler is playing (UDP
    //                       write bus is suppressed in that case).
    //   • frame_seq       — bumped on EVERY engine-input tap publish
    //                       (udpThread / feeder / FramePlayerThread), i.e.
    //                       exactly when the frames this waterfall renders
    //                       changed. Keeps advancing during playback while
    //                       lines_received is frozen. (P4-M3 — the modulated
    //                       bus and its counter are gone.)
    //
    // We treat *any* counter advance as "new frame available".
    const uint64_t received  = (uint64_t)aib->lines_received;
    const uint64_t seq       = __atomic_load_n(&aib->frame_seq,
                                                __ATOMIC_ACQUIRE);
    const uint64_t lastR = lastLinesReceived_.load(std::memory_order_relaxed);
    const uint64_t lastS = lastFrameSeq_     .load(std::memory_order_relaxed);
    if (received == lastR && seq == lastS) return;
    lastLinesReceived_.store(received, std::memory_order_relaxed);
    lastFrameSeq_     .store(seq,      std::memory_order_relaxed);

    const int count = get_cis_pixels_nb();
    if (count <= 0) return;

    // ── Source selection (M7 — per-chain engine taps) ────────────────────────
    // videoScrollSource (APVTS choice) selects which synthesis engine the
    // waterfall mirrors:
    //   0 = LuxStral         → LuxStral engine input tap (frame the engine consumed)
    //   1 = LuxSynth/LuxWave → engine input tap Path-B
    //   2 = AllSynth         → 50/50 blend of the two taps
    // The taps are published by whichever thread owns each engine's commit
    // (udpThread / feeder / FramePlayerThread) — the display follows each
    // engine's OWN chain, never the legacy global source types.
    const int srcChoice = static_cast<int>(
        processor_.getAPVTS().getRawParameterValue("videoScrollSource")->load());

    auto resolveEngineInput = [&](int tapIndex,
                                  uint8_t*& outR, uint8_t*& outG, uint8_t*& outB)
    {
        outR = outG = outB = nullptr;
        audio_image_buffers_get_engine_input_pointers(aib, tapIndex,
                                                      &outR, &outG, &outB);
    };


    uint8_t* pR = nullptr;
    uint8_t* pG = nullptr;
    uint8_t* pB = nullptr;

    // Scratch buffers for AllSynth blend.  Single-producer thread → static OK.
    static constexpr int kMaxBlendPixels = 8192;
    static uint8_t blendR[kMaxBlendPixels];
    static uint8_t blendG[kMaxBlendPixels];
    static uint8_t blendB[kMaxBlendPixels];

    if (srcChoice == 2 && count <= kMaxBlendPixels)
    {
        // AllSynth: 50/50 blend per channel.
        uint8_t *lsR = nullptr, *lsG = nullptr, *lsB = nullptr;
        uint8_t *lxR = nullptr, *lxG = nullptr, *lxB = nullptr;
        resolveEngineInput(AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL, lsR, lsG, lsB);
        resolveEngineInput(AUDIO_IMAGE_ENGINE_TAP_PATHB,      lxR, lxG, lxB);
        const bool lsOk = (lsR && lsG && lsB);
        const bool lxOk = (lxR && lxG && lxB);
        if (lsOk && lxOk)
        {
            for (int i = 0; i < count; ++i)
            {
                blendR[i] = (uint8_t)(((int)lsR[i] + (int)lxR[i] + 1) >> 1);
                blendG[i] = (uint8_t)(((int)lsG[i] + (int)lxG[i] + 1) >> 1);
                blendB[i] = (uint8_t)(((int)lsB[i] + (int)lxB[i] + 1) >> 1);
            }
            pR = blendR; pG = blendG; pB = blendB;
        }
        else if (lsOk) { pR = lsR; pG = lsG; pB = lsB; }
        else if (lxOk) { pR = lxR; pG = lxG; pB = lxB; }
    }
    else
    {
        // LuxStral (srcChoice 0) or LuxSynth/LuxWave (srcChoice 1).
        resolveEngineInput(srcChoice == 1 ? AUDIO_IMAGE_ENGINE_TAP_PATHB
                                          : AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL,
                           pR, pG, pB);
    }

    if (!pR || !pG || !pB) return;

    // Write into ring slot (SPSC: only this thread writes ringWriteIdx_)
    const int slot = ringWriteIdx_.load(std::memory_order_relaxed) & (kRingSize - 1);
    auto& f = frameRing_[slot];

    f.r.assign(pR, pR + count);
    f.g.assign(pG, pG + count);
    f.b.assign(pB, pB + count);
    f.gray.resize(count);
    for (int i = 0; i < count; ++i)
        f.gray[i] = (uint8_t)(((int)pR[i] * 77 + (int)pG[i] * 150 + (int)pB[i] * 29) >> 8);

    cisCount_ = count; // safe: read on message thread only for display scaling

    // Publish: advance write pointer AFTER data is fully written
    ringWriteIdx_.fetch_add(1, std::memory_order_release);
}

//==============================================================================
// JUCE Component overrides
//==============================================================================

void VideoDisplayComponent::resized()
{
    allocateScrollBuffer();
}

void VideoDisplayComponent::mouseDoubleClick(const juce::MouseEvent&)
{
    if (onFullscreenRequested)
        onFullscreenRequested();
}

void VideoDisplayComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    if (!buffersInit_ || bufW_ <= 0 || bufH_ <= 0 || compH_ <= 0) return;

    // The freshly-drawn buffer is the one scrollStep() rendered into before the
    // swap (see curBuf_ bookkeeping in scrollStep()).
    const juce::Image& shown = (curBuf_ != 0) ? historyB_ : historyA_;
    if (!shown.isValid()) return;

    const auto& apvts = processor_.getAPVTS();

    // ── Birth line (the "source" the effects radiate from) ───────────────────
    //   Line Pos spans the viewport edge-to-edge: -1 = top edge, 0 = centre,
    //   +1 = bottom edge.  It positions the line both in the history buffer
    //   (where scrollStep stamps and splits) and on screen (where we centre the
    //   view), so the two always agree.
    const float posParam = juce::jlimit(-1.f, 1.f,
        apvts.getRawParameterValue("videoScrollLinePos")->load());
    const float posNorm  = (posParam + 1.f) * 0.5f;
    const int   birthBuf    = juce::jlimit(0, bufH_, (int) (posNorm * (float) bufH_));
    const float birthScreen = posNorm * (float) compH_;
    const float upSpan   = juce::jmax(1.0f, birthScreen);                 // px above
    const float downSpan = juce::jmax(1.0f, (float) compH_ - birthScreen);// px below

    // ── Distance-driven display effects (applied on a clean linear buffer) ───
    //   Compression → non-linear deceleration / time-squish: content is at full
    //                 scale and full speed AT the source, then decelerates and
    //                 packs tighter as it moves away (quadratic, G'(0)=1).
    //   Fade        → progressive aging away from the source: dim + desaturate
    //                 + blur (horizontal box blur; vertical softening comes from
    //                 the squish averaging).
    const float comp01 = juce::jlimit(0.f, 1.f,
        (apvts.getRawParameterValue("videoScrollMaxDuration")->load() - 1.0f) / 63.0f);
    const float fade01 = juce::jlimit(0.f, 1.f,
        apvts.getRawParameterValue("videoScrollFade")->load());

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
        const float g  = ad + cComp * ad * ad / sp;     // buffer px from birth
        return birthBuf + (int) std::lround(d < 0.f ? -g : g);
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
                        (juce::uint8) juce::jlimit(0, 255, (int) ((float) aR[x] * k + 0.5f)),
                        (juce::uint8) juce::jlimit(0, 255, (int) ((float) aG[x] * k + 0.5f)),
                        (juce::uint8) juce::jlimit(0, 255, (int) ((float) aB[x] * k + 0.5f)));
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
                        (juce::uint8) juce::jlimit(0, 255, (int) (r  + 0.5f)),
                        (juce::uint8) juce::jlimit(0, 255, (int) (gv + 0.5f)),
                        (juce::uint8) juce::jlimit(0, 255, (int) (bl + 0.5f)));
                }
            }
        }
    }

    // ── Horizontal fade blur: per-row box blur whose radius grows with the
    //    distance from the source, so old content loses sharpness across the
    //    CIS (frequency) axis.  Running-sum prefix → O(width)/row. ────────────
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

    // ── Zoom + orientation rotation (applied to the warped/aged image) ───────
    //   Deg0 (0°) → vertical, Deg90/270 → horizontal, Deg180 → flipped vertical.
    const float zoom = juce::jlimit(0.5f, 4.0f,
        apvts.getRawParameterValue("videoScrollZoom")->load());
    const int modeVal = static_cast<int>(
        apvts.getRawParameterValue("videoScrollMode")->load());
    const float angle = static_cast<float>(juce::jlimit(0, 3, modeVal))
                        * juce::MathConstants<float>::halfPi;

    const float cw = (float) getWidth();
    const float ch = (float) getHeight();
    const float cx = cw * 0.5f;
    const float cy = ch * 0.5f;

    juce::AffineTransform t =
        juce::AffineTransform::scale(cw / (float) bufW_, ch / (float) compH_)
            .scaled(zoom, zoom, cx, cy);
    if (modeVal != 0)
        t = t.rotated(angle, cx, cy);

    // High-quality resampling so zoom/rotation don't reintroduce aliasing
    // (the warp pass itself is already a proper box-average downsample).
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImageTransformed(warpBuf_, t);
    // Orientation label is displayed in the VideoWindow toolbar.
}

//==============================================================================
// Timer callback — drains the ring and renders pending frames
//==============================================================================

void VideoDisplayComponent::timerCallback()
{
    if (buffersInit_ && bufW_ > 0 && bufH_ > 0)
    {
        // Transport "Stop": blank the waterfall when the processor's clear pulse
        // advances (polled here so every open view reacts without a back-pointer).
        const uint32_t gen = processor_.getVideoScrollClearGen();
        if (gen != lastClearGen_)
        {
            lastClearGen_ = gen;
            clearHistory();
        }

        scrollStep();
    }

    repaint();
}

//==============================================================================
// scrollStep — one ping-pong scroll generation (legacy birth-line model)
//==============================================================================
//
// Reproduces the legacy SFML bidirectional renderer:
//   • the image is split at a movable "birth line";
//   • the upper zone shifts one way and the lower zone the other, so content
//     scrolls AWAY from (forward) or TOWARD (reverse) the birth line;
//   • the freshly-built scanline is stamped at the birth line, with a
//     configurable thickness;
//   • a per-frame brightness decay (fade) gives optional trails.
//
// Source/destination ping-pong avoids the in-place overlap problem of shifting
// two zones apart within a single image.
//==============================================================================

void VideoDisplayComponent::scrollStep()
{
    auto& apvts = processor_.getAPVTS();

    // ── Transport: paused → freeze in place ──────────────────────────────────
    // Keep the current image untouched and perform no scroll/stamp.  The capture
    // ring is still drained so it never backs up while frozen; resuming picks up
    // from the live stream rather than replaying stale frames.
    if (apvts.getRawParameterValue("videoScrollPaused")->load() >= 0.5f)
    {
        ringReadIdx_       = ringWriteIdx_.load(std::memory_order_acquire);
        scrollAccumulator_ = 0.f;
        return;
    }

    // The history buffer is kept as a CLEAN linear waterfall (one buffer row per
    // unit of CIS time).  The artistic time-squish (Compression) and aging
    // (Fade) are NOT baked in here — they are applied at display time in paint()
    // as functions of the distance from the birth line, so they stay instant,
    // always visible and never freeze the scroll.

    // ── Speed → signed pixels this tick (absolute, exponential) ──────────────
    //   px = sign(s) * (2^(3*|s|) - 1)  → s=0 frozen, s=±1 → ±7 px/tick.
    const float speedParam = juce::jlimit(-1.f, 1.f,
        apvts.getRawParameterValue("videoScrollSpeed")->load());
    const float mag    = std::pow(2.0f, 3.0f * std::abs(speedParam)) - 1.0f;
    const float pxRate = (speedParam < 0.f) ? -mag : mag;
    const bool  reverse = (pxRate < 0.f);

    scrollAccumulator_ += std::abs(pxRate);
    const int scroll = (int) scrollAccumulator_;
    scrollAccumulator_ -= (float) scroll;

    // Fresh CIS frames captured since the previous tick (drained every tick so
    // capture never lags behind the display).
    const int wr        = ringWriteIdx_.load(std::memory_order_acquire);
    const int available = juce::jmax(0, wr - ringReadIdx_);
    ringReadIdx_ = wr;

    // ── Birth-line position ──────────────────────────────────────────────────
    const float posParam = juce::jlimit(-1.f, 1.f,
        apvts.getRawParameterValue("videoScrollLinePos")->load());
    const float posNorm  = (posParam + 1.f) * 0.5f;
    const int   birthY   = juce::jlimit(0, bufH_, (int) (posNorm * (float) bufH_));

    // ── Line geometry ────────────────────────────────────────────────────────
    //   coreH : the data rows that must exactly fill the motion gap (2*scroll px)
    //           at a fixed 1-row-per-time-slice scale.  Pinning the core to the
    //           gap keeps the waterfall's time-scale constant, so content never
    //           stretches/compresses inside the stamp (the old deformation).
    //   bandH : total stamped height.  Thickness simply DUPLICATES the birth
    //           line onto the rows above/below it (see buildLineImage): every
    //           row of the band is the SAME freshest line, so a thick line is a
    //           clean fat bar — never a temporal smear, and the freshest data
    //           stays AT the birth line (no thickness-dependent latency).
    const float thickParam = juce::jlimit(0.f, 1.f,
        apvts.getRawParameterValue("videoScrollLineThickness")->load());
    const int   thicknessPx = juce::jmax(1, (int) (1.0f + thickParam * (float) (compH_ - 1)));
    const int   coreH = juce::jmax(1, 2 * scroll);
    const int   bandH = juce::jmax(coreH, thicknessPx);

    // Build the fresh content (newest `available` frames, box-averaged on BOTH
    // the CIS axis and the temporal axis so wide CIS lines don't alias/hatch and
    // fast scrolls drop no CIS lines).
    juce::Image lineImg;
    const bool haveLine = buildLineImage(lineImg, coreH, bandH, available, wr);

    // Nothing changed and not scrolling — keep last frame (paint() still
    // re-applies the display effects each refresh).
    if (scroll == 0 && !haveLine)
        return;

    juce::Image& src = (curBuf_ != 0) ? historyB_ : historyA_;
    juce::Image& dst = (curBuf_ != 0) ? historyA_ : historyB_;

    {
        juce::Graphics g(dst);
        g.fillAll(juce::Colours::black);

        // Forward → zones move away from birth line; reverse → toward it.
        const int s = reverse ? -scroll : scroll;

        // Upper zone [0, birthY): shifts by -s (forward = up).
        if (birthY > 0)
            g.drawImage(src, 0, -s, bufW_, birthY,
                             0,  0, bufW_, birthY);
        // Lower zone [birthY, bufH_): shifts by +s (forward = down).
        const int lowerH = bufH_ - birthY;
        if (lowerH > 0)
            g.drawImage(src, 0, birthY + s, bufW_, lowerH,
                             0, birthY,     bufW_, lowerH);

        // Stamp the fresh strip at the birth line (already stampH px tall).
        if (haveLine && lineImg.isValid())
        {
            const int hPx  = lineImg.getHeight();
            const int yPos = (int) ((float) birthY - (float) hPx * 0.5f);
            g.drawImage(lineImg, 0, yPos, bufW_, hPx,
                                 0, 0, lineImg.getWidth(), hPx);
        }
    }

    curBuf_ = 1 - curBuf_;
}

//==============================================================================
// buildLineImage — average the `compression` most-recent CIS frames into a
// single 1-px-tall RGB scanline (width bufW_), applying invert / colour-mode.
// Drains the ring up to the newest frame to avoid overflow.
//==============================================================================

bool VideoDisplayComponent::buildLineImage(juce::Image& out, int coreH, int bandH,
                                           int available, int wr)
{
    if (available <= 0 || bufW_ <= 0) return false;
    coreH = juce::jmax(1, coreH);
    bandH = juce::jmax(coreH, bandH);

    auto& apvts = processor_.getAPVTS();
    const bool  invert     = apvts.getRawParameterValue("videoInvertColor")->load() > 0.5f;
    const bool  colorMode  = apvts.getRawParameterValue("videoColorMode")->load()  > 0.5f;

    const int nUse = available;
    const int base = wr - nUse;   // ring index of the oldest represented frame

    const RingFrame& newest = frameRing_[(wr - 1) & (kRingSize - 1)];
    const int count = (int) newest.gray.size();
    if (count <= 0) return false;

    out = juce::Image(juce::Image::RGB, bufW_, bandH, false, juce::SoftwareImageType());
    juce::Image::BitmapData bmp(out, juce::Image::BitmapData::writeOnly);
    const int dps = bmp.pixelStride;

    // ── Horizontal box-average map ───────────────────────────────────────────
    // Output column x covers the CIS range [col0[x], col0[x+1]).  Averaging the
    // whole range (instead of picking one nearest CIS pixel) removes the moiré /
    // "hatching" that appears when the CIS line has many more pixels than the
    // display is wide.  Per-frame prefix sums make each column an O(1) lookup.
    std::vector<int> col0(bufW_ + 1);
    for (int x = 0; x <= bufW_; ++x)
        col0[x] = juce::jlimit(0, count, (int) ((long long) x * count / bufW_));

    std::vector<int>   pfR(count + 1), pfG(count + 1), pfB(count + 1);
    std::vector<float> accR(bufW_, 0.f), accG(bufW_, 0.f), accB(bufW_, 0.f);

    // ── The birth line is ONE scanline, duplicated across the whole band ──────
    // Thickness simply makes the birth line fatter: the SAME line is copied onto
    // the rows above and below it (1 px up/down, 2 px up/down, …).  There is no
    // temporal gradient inside the stamp — every row is identical — so a thick
    // line stays a clean fat band and never degrades into the coarse constant
    // blocks ("pixelated" look) that the old mirrored-core + edge-padding made.
    // All ageing/compression/fade is a distance-from-birth effect applied later
    // in paint(), not baked here.
    //
    // The line itself is the `available` CIS frames captured this tick, box-
    // averaged on BOTH the CIS axis (no moiré) and the temporal axis (fast
    // scrolls drop no frames) into a single row.
    int used = 0;
    for (int f = 0; f < nUse; ++f)
    {
        const RingFrame& fr = frameRing_[(base + f) & (kRingSize - 1)];
        if ((int) fr.gray.size() != count) continue;  // skip size mismatch
        ++used;
        if (colorMode)
        {
            pfR[0] = pfG[0] = pfB[0] = 0;
            for (int i = 0; i < count; ++i)
            {
                pfR[i + 1] = pfR[i] + fr.r[i];
                pfG[i + 1] = pfG[i] + fr.g[i];
                pfB[i + 1] = pfB[i] + fr.b[i];
            }
            for (int x = 0; x < bufW_; ++x)
            {
                int a = col0[x], b = col0[x + 1];
                if (b <= a) b = juce::jmin(count, a + 1);
                const float inv = 1.f / (float) (b - a);
                accR[x] += (float) (pfR[b] - pfR[a]) * inv;
                accG[x] += (float) (pfG[b] - pfG[a]) * inv;
                accB[x] += (float) (pfB[b] - pfB[a]) * inv;
            }
        }
        else
        {
            pfR[0] = 0;
            for (int i = 0; i < count; ++i) pfR[i + 1] = pfR[i] + fr.gray[i];
            for (int x = 0; x < bufW_; ++x)
            {
                int a = col0[x], b = col0[x + 1];
                if (b <= a) b = juce::jmin(count, a + 1);
                const float gy = (float) (pfR[b] - pfR[a]) / (float) (b - a);
                accR[x] += gy; accG[x] += gy; accB[x] += gy;
            }
        }
    }
    if (used == 0) used = 1;  // leave the line black rather than divide by 0
    const float invUsed = 1.f / (float) used;

    // Compose the birth line into the first row, then duplicate it everywhere.
    auto* row0 = bmp.getLinePointer(0);
    for (int x = 0; x < bufW_; ++x)
    {
        int r  = (int) (accR[x] * invUsed + 0.5f);
        int gv = (int) (accG[x] * invUsed + 0.5f);
        int b  = (int) (accB[x] * invUsed + 0.5f);
        if (invert) { r = 255 - r; gv = 255 - gv; b = 255 - b; }
        auto* dp = reinterpret_cast<juce::PixelRGB*>(row0 + x * dps);
        dp->setARGB(255,
                    (juce::uint8) juce::jlimit(0, 255, r),
                    (juce::uint8) juce::jlimit(0, 255, gv),
                    (juce::uint8) juce::jlimit(0, 255, b));
    }

    const size_t rowBytes = (size_t) bufW_ * (size_t) dps;
    for (int r = 1; r < bandH; ++r)
        std::memcpy(bmp.getLinePointer(r), row0, rowBytes);

    return true;
}

//==============================================================================
// Helpers
//==============================================================================

void VideoDisplayComponent::allocateScrollBuffer()
{
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;
    if (w != compW_ || h != compH_)
    {
        compW_ = w;
        compH_ = h;
        bufW_  = w;
        // 4× headroom: birth-line panning + extra history for the Compression
        // time-squish to actually have older content to pack into the far field.
        bufH_  = juce::jmax(2, h * 4);
        // SoftwareImageType → guaranteed packed RGB (pixelStride 3) so the raw
        // BitmapData pointer maths in paint()/buildLineImage is correct and fast
        // (the native macOS backend would store RGB as 4-byte ARGB).
        historyA_ = juce::Image(juce::Image::RGB, bufW_, bufH_, true,  juce::SoftwareImageType());
        historyB_ = juce::Image(juce::Image::RGB, bufW_, bufH_, true,  juce::SoftwareImageType());
        historyA_.clear(historyA_.getBounds(), juce::Colours::black);
        historyB_.clear(historyB_.getBounds(), juce::Colours::black);
        curBuf_            = 0;
        scrollAccumulator_ = 0.f;
        buffersInit_       = true;
    }
}

//==============================================================================
void VideoDisplayComponent::clearHistory()
{
    if (historyA_.isValid())
        historyA_.clear(historyA_.getBounds(), juce::Colours::black);
    if (historyB_.isValid())
        historyB_.clear(historyB_.getBounds(), juce::Colours::black);
    curBuf_            = 0;
    scrollAccumulator_ = 0.f;
}
