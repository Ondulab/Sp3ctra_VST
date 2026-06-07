#include "VideoDisplayComponent.h"
#include "../PluginProcessor.h"

extern "C"
{
#include "audio/buffers/audio_image_buffers.h"
#include "config/config_loader.h"
#include "processing/lux_pitch.h"
}

/* Cap LuxPitch output buffer copy at the engine's max pixel capacity.
 * Defined in processing/lux_pitch.h.  Avoids a redefinition warning when the
 * macro is reused locally. */

#include <cstring>

//==============================================================================
VideoDisplayComponent::VideoDisplayComponent(Sp3ctraAudioProcessor& proc)
    : processor_(proc), captureThread_(*this)
{
    setOpaque(true);

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

    // Detect new frame via lines_received counter (lock-free)
    const uint64_t received = (uint64_t)aib->lines_received;
    const uint64_t last     = lastLinesReceived_.load(std::memory_order_relaxed);
    if (received == last) return;
    lastLinesReceived_.store(received, std::memory_order_relaxed);

    const int count = get_cis_pixels_nb();
    if (count <= 0) return;

    // Select source buffer
    const int srcChoice = static_cast<int>(
        processor_.getAPVTS().getRawParameterValue("videoScrollSource")->load());

    uint8_t* pR = nullptr;
    uint8_t* pG = nullptr;
    uint8_t* pB = nullptr;

    switch (srcChoice)
    {
        case 0: // L — raw live UDP
            audio_image_buffers_get_raw_pointers(aib, &pR, &pG, &pB);
            break;
        case 1: // Sample
            audio_image_buffers_get_sampler_pointers(aib, &pR, &pG, &pB);
            break;
        case 3:
        {
            // LuxPitch Output — process the LuxPitch pitch-shift INDEPENDENTLY
            // for the video scroll.  We do NOT depend on the visualizer or on
            // the audio thread: we drive our own private LuxPitch instance
            // (g_lux_pitch_vid) so the waterfall keeps scrolling even when the
            // visualizer is showing another node.
            //
            // The visualizer is intentionally not coupled to this pipeline —
            // visualizers only display their own state.
            if (count > LUX_PITCH_MAX_PIXELS)
            {
                // Source too wide for the LuxPitch buffers → fallback to Mix.
                audio_image_buffers_get_read_pointers(aib, &pR, &pG, &pB);
                break;
            }

            // Resolve upstream source as configured by the user
            // (luxpitchSource: 0=Sampler, 1=Mix, 2=Live).
            const int lpChoice = static_cast<int>(
                processor_.getAPVTS().getRawParameterValue("luxpitchSource")->load());
            uint8_t* inR = nullptr;
            uint8_t* inG = nullptr;
            uint8_t* inB = nullptr;
            switch (lpChoice)
            {
                case 0:  audio_image_buffers_get_sampler_pointers(aib, &inR, &inG, &inB); break;
                case 2:  audio_image_buffers_get_raw_pointers    (aib, &inR, &inG, &inB); break;
                case 1:
                default: audio_image_buffers_get_read_pointers   (aib, &inR, &inG, &inB); break;
            }

            if (!inR || !inG || !inB)
            {
                // Upstream not ready → silently fallback to Mix to keep the UI alive.
                audio_image_buffers_get_read_pointers(aib, &pR, &pG, &pB);
                break;
            }

            // Run our private LuxPitch engine on the upstream frame.
            // Config has already been synced into g_lux_pitch_vid.config by
            // applyConfigurationToCore() (PluginProcessor), and MIDI events are
            // delivered to g_lux_pitch_vid in processBlock().
            const uint8_t *outR = nullptr;
            const uint8_t *outG = nullptr;
            const uint8_t *outB = nullptr;
            lux_pitch_process_frame(&g_lux_pitch_vid,
                                    inR, inG, inB,
                                    count,
                                    g_sp3ctra_config.num_octaves,
                                    &outR, &outG, &outB);

            // lux_pitch_process_frame may return the input pointers as-is
            // (bypass when disabled) or the internal out_* buffers (when active).
            pR = (uint8_t*)outR;
            pG = (uint8_t*)outG;
            pB = (uint8_t*)outB;
            break;
        }
        case 2: // Mix
        default:
            audio_image_buffers_get_read_pointers(aib, &pR, &pG, &pB);
            break;
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
    if (!scrollBuffer_.isValid()) return;

    // Always use the ACTUAL image dimensions (guards against bufW_/bufH_ drift)
    const int imgW = scrollBuffer_.getWidth();
    const int imgH = scrollBuffer_.getHeight();
    if (imgW <= 0 || imgH <= 0) return;

    // ── Circular rendering (no memcpy): two-pass draw ──────────────────────
    // writeRow_ is the NEXT row to be written → the "oldest" visible row.
    // Layout on screen (Forward direction, newest at bottom):
    //   [writeRow_ .. imgH-1] → screen top    (oldest data)
    //   [0 .. writeRow_-1]    → screen bottom  (newest data)
    //
    // For Reverse (newest at top): swap the two halves.
    // Both halves are stretched to fill the full component width.
    // ──────────────────────────────────────────────────────────────────────

    const auto& apvts = processor_.getAPVTS();
    const bool reverse  = apvts.getRawParameterValue("videoScrollDirection")->load() > 0.5f;

    const float compW = (float)getWidth();
    const float compH = (float)getHeight();

    // ── Orientation rotation ─────────────────────────────────────────────────
    // The scroll buffer always stores rows top-to-bottom (vertical waterfall).
    // The chosen orientation angle rotates the rendered output around the
    // component centre:
    //   Deg0   (0°)   → no transform  → vertical scroll,   new data at bottom
    //   Deg90  (90°)  → 90° CW       → horizontal scroll,  new data at right
    //   Deg180 (180°) → 180°         → vertical scroll,    new data at top
    //   Deg270 (270°) → 270° CW      → horizontal scroll,  new data at left
    // ─────────────────────────────────────────────────────────────────────────
    const int modeVal = static_cast<int>(
        apvts.getRawParameterValue("videoScrollMode")->load());
    const float angle = static_cast<float>(juce::jlimit(0, 3, modeVal))
                        * juce::MathConstants<float>::halfPi;
    const float cx = compW * 0.5f;
    const float cy = compH * 0.5f;

    g.saveState();
    if (modeVal != 0)
        g.addTransform(juce::AffineTransform::rotation(angle, cx, cy));

    // Heights of the two halves in image pixels
    const int topRows = reverse ? writeRow_         : (imgH - writeRow_);
    const int botRows = reverse ? (imgH - writeRow_): writeRow_;

    const int topSrcY  = reverse ? 0        : writeRow_;
    const int botSrcY  = reverse ? writeRow_ : 0;

    const float topScreenH = (imgH > 0) ? (compH * (float)topRows / (float)imgH) : 0.f;
    const float botScreenH = compH - topScreenH;

    if (topRows > 0)
    {
        // Source width = imgW (full image width); dest width = compW (full component)
        g.drawImage(scrollBuffer_,
                    0.f, 0.f, compW, topScreenH,
                    0, topSrcY, imgW, topRows);
    }
    if (botRows > 0)
    {
        g.drawImage(scrollBuffer_,
                    0.f, topScreenH, compW, botScreenH,
                    0, botSrcY, imgW, botRows);
    }

    g.restoreState();
    // Orientation label is displayed in the VideoWindow toolbar.
}

//==============================================================================
// Timer callback — drains the ring and renders pending frames
//==============================================================================

void VideoDisplayComponent::timerCallback()
{
    auto& apvts = processor_.getAPVTS();

    // Detect scroll mode, reset seq on transition
    const int modeVal = static_cast<int>(
        apvts.getRawParameterValue("videoScrollMode")->load());
    const VideoScrollMode mode = static_cast<VideoScrollMode>(
        juce::jlimit(0, (int)VideoScrollMode::COUNT - 1, modeVal));

    if (mode != prevMode_)
        prevMode_ = mode;

    if (bufW_ > 0 && bufH_ > 0)
        drainRingAndAdvance(mode);

    repaint();
}

//==============================================================================
// Drain ring buffer and advance waterfall
//==============================================================================

void VideoDisplayComponent::drainRingAndAdvance(VideoScrollMode mode)
{
    auto& apvts = processor_.getAPVTS();
    const float speed   = apvts.getRawParameterValue("videoScrollSpeed")->load();
    const bool  reverse = apvts.getRawParameterValue("videoScrollDirection")->load() > 0.5f;

    // How many ring frames were captured since last tick.
    const int available = ringWriteIdx_.load(std::memory_order_acquire) - ringReadIdx_;
    if (available <= 0) return;

    // One-time pre-fill: populate entire scroll buffer with first available frame
    // so the window shows CIS data immediately (no half-black startup).
    if (!bufferPreFilled_ && bufH_ > 0 && scrollBuffer_.isValid())
    {
        const auto& firstFr = frameRing_[ringReadIdx_ & (kRingSize - 1)];
        if (!firstFr.gray.empty())
        {
            for (int row = 0; row < bufH_; ++row)
                paintRowFromFrame(row, false, firstFr);
            bufferPreFilled_ = true;
        }
    }

    // ── Per-frame fractional row accumulator ──────────────────────────────────
    // ALL captured frames are drained from the ring every tick to prevent
    // overflow.  For each frame, rowAccumulator_ advances by `speed`:
    //
    //   speed=1.0 → 1 row painted per frame       → real-time scroll
    //   speed=0.5 → 1 row painted every 2 frames  → 2× slow-motion
    //   speed=0.1 → 1 row every 10 frames         → 10× slow-motion
    //   speed=2.0 → 2 rows per frame (frame reuse) → 2× fast scroll
    //   speed=3.0 → 3 rows per frame               → 3× fast scroll
    //
    // For speed > 1.0 each CIS line is painted multiple times (upsampling),
    // creating a visible stretch of each scanline in the waterfall.
    // Safety cap: at most bufH_ total rows per tick (one full screen refresh).
    // ─────────────────────────────────────────────────────────────────────────
    const int maxRowsThisTick = juce::jmax(1, bufH_); // avoid CPU spike at very high speed
    int rowsPaintedThisTick   = 0;

    for (int i = 0; i < available; ++i)
    {
        const int slot = ringReadIdx_ & (kRingSize - 1);
        ++ringReadIdx_;

        const RingFrame& fr = frameRing_[slot];
        if (fr.gray.empty()) continue;
        if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid()) continue;

        // How many waterfall rows to paint for this captured CIS frame
        rowAccumulator_ += speed;
        int rowsForThisFrame = (int)rowAccumulator_;
        rowAccumulator_ -= (float)rowsForThisFrame;

        for (int r = 0; r < rowsForThisFrame; ++r)
        {
            if (rowsPaintedThisTick >= maxRowsThisTick) break; // safety cap
            ++rowsPaintedThisTick;

            if (reverse)
                writeRow_ = (writeRow_ - 1 + bufH_) % bufH_;
            else
                writeRow_ = (writeRow_ + 1) % bufH_;

            // All 4 orientation modes (Deg0/90/180/270) store rows identically.
            // Orientation is handled purely by AffineTransform in paint().
            juce::ignoreUnused(mode);
            paintRowFromFrame(writeRow_, false, fr);

        } // for rowsForThisFrame
    } // for available
}

//==============================================================================
// Row painters
//==============================================================================

void VideoDisplayComponent::paintRowFromFrame(int rowY, bool mirror, const RingFrame& fr)
{
    const int count = (int)fr.gray.size();
    if (count <= 0 || bufW_ <= 0 || !scrollBuffer_.isValid()) return;

    auto& apvts = processor_.getAPVTS();
    const float brightness = apvts.getRawParameterValue("videoScrollBrightness")->load();
    const bool  invert     = apvts.getRawParameterValue("videoInvertColor")->load() > 0.5f;
    const bool  colorMode  = apvts.getRawParameterValue("videoColorMode")->load()  > 0.5f;

    juce::Image::BitmapData bmp(scrollBuffer_, juce::Image::BitmapData::writeOnly);

    for (int px = 0; px < bufW_; ++px)
    {
        int ci = (int)((float)px / (float)bufW_ * (float)count);
        if (mirror) ci = count - 1 - ci;
        ci = juce::jlimit(0, count - 1, ci);

        uint8_t r  = colorMode ? fr.r[ci]    : fr.gray[ci];
        uint8_t gv = colorMode ? fr.g[ci]    : fr.gray[ci];
        uint8_t b  = colorMode ? fr.b[ci]    : fr.gray[ci];

        if (invert) { r = 255-r; gv = 255-gv; b = 255-b; }
        if (brightness != 1.f)
        {
            r  = (uint8_t)juce::jlimit(0,255,(int)((float)r  * brightness));
            gv = (uint8_t)juce::jlimit(0,255,(int)((float)gv * brightness));
            b  = (uint8_t)juce::jlimit(0,255,(int)((float)b  * brightness));
        }
        bmp.setPixelColour(px, rowY, juce::Colour(r, gv, b));
    }
}

void VideoDisplayComponent::paintRowFromRing(int rowY, bool mirror)
{
    paintRowFromFrame(rowY, mirror, frameRing_[(ringReadIdx_ - 1) & (kRingSize - 1)]);
}

void VideoDisplayComponent::paintRowFromSeq(int rowY, bool mirror, int seqIdx) const
{
    if (seqFrames_.empty() || !scrollBuffer_.isValid()) return;
    seqIdx = juce::jlimit(0, (int)seqFrames_.size() - 1, seqIdx);
    const auto& frame = seqFrames_[seqIdx];
    const int count = (int)frame.gray.size();
    if (count <= 0) return;

    auto& apvts = processor_.getAPVTS();
    const float brightness = apvts.getRawParameterValue("videoScrollBrightness")->load();
    const bool  invert     = apvts.getRawParameterValue("videoInvertColor")->load() > 0.5f;
    const bool  colorMode  = apvts.getRawParameterValue("videoColorMode")->load()  > 0.5f;

    juce::Image::BitmapData bmp(const_cast<juce::Image&>(scrollBuffer_),
                                juce::Image::BitmapData::writeOnly);

    for (int px = 0; px < bufW_; ++px)
    {
        int ci = (int)((float)px / (float)bufW_ * (float)count);
        if (mirror) ci = count - 1 - ci;
        ci = juce::jlimit(0, count - 1, ci);

        uint8_t r  = colorMode && !frame.r.empty() ? frame.r[ci] : frame.gray[ci];
        uint8_t gv = colorMode && !frame.g.empty() ? frame.g[ci] : frame.gray[ci];
        uint8_t b  = colorMode && !frame.b.empty() ? frame.b[ci] : frame.gray[ci];

        if (invert) { r = 255-r; gv = 255-gv; b = 255-b; }
        if (brightness != 1.f)
        {
            r  = (uint8_t)juce::jlimit(0,255,(int)((float)r  * brightness));
            gv = (uint8_t)juce::jlimit(0,255,(int)((float)gv * brightness));
            b  = (uint8_t)juce::jlimit(0,255,(int)((float)b  * brightness));
        }
        bmp.setPixelColour(px, rowY, juce::Colour(r, gv, b));
    }
}

//==============================================================================
// Helpers
//==============================================================================

void VideoDisplayComponent::allocateScrollBuffer()
{
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;
    if (w != bufW_ || h != bufH_)
    {
        bufW_            = w;
        bufH_            = h;
        writeRow_        = 0;
        bufferPreFilled_ = false;  // force re-fill with first frame after resize
        scrollBuffer_ = juce::Image(juce::Image::RGB, w, h, true);
        scrollBuffer_.clear(scrollBuffer_.getBounds(), juce::Colours::black);
    }
}

void VideoDisplayComponent::resetSequence()
{
    seqFrames_.clear();
    seqPlayHead_  = 0;
    seqPingFwd_   = true;
    seqRecording_ = true;
    seqFinished_  = false;
}

void VideoDisplayComponent::appendSeqFrame(const std::vector<uint8_t>& r,
                                           const std::vector<uint8_t>& g,
                                           const std::vector<uint8_t>& b,
                                           const std::vector<uint8_t>& gray)
{
    SeqFrame f;
    f.r = r; f.g = g; f.b = b; f.gray = gray;
    seqFrames_.push_back(std::move(f));
}

void VideoDisplayComponent::advanceSeqPlayHead(VideoScrollMode /*mode*/, bool /*reverse*/)
{
    // Seq modes removed — 4 simple orientation angles (Deg0/90/180/270) only.
}
