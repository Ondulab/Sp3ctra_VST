#include "VideoDisplayComponent.h"
#include "../PluginProcessor.h"

extern "C"
{
#include "audio/buffers/audio_image_buffers.h"
#include "config/config_loader.h"
}

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
        case 3: // LuxPitch — mirrors the APVTS "luxpitchSource" selector
        {
            // luxpitchSource choices: 0=S-Sampler, 1=M-Mix, 2=L-Live
            // Same mapping as CisVisualizerComponent: S→0(SAMPLER), M→2(MIX), L→1(LIVE)
            static const int kLpChoiceToSrc[3] = { 0, 2, 1 };
            int lpChoice = static_cast<int>(
                processor_.getAPVTS().getRawParameterValue("luxpitchSource")->load());
            if (lpChoice < 0 || lpChoice > 2) lpChoice = 1; // default: Mix
            const int lp = kLpChoiceToSrc[lpChoice];
            if      (lp == 1) audio_image_buffers_get_raw_pointers    (aib, &pR, &pG, &pB);
            else if (lp == 0) audio_image_buffers_get_sampler_pointers(aib, &pR, &pG, &pB);
            else              audio_image_buffers_get_read_pointers    (aib, &pR, &pG, &pB);
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

    // Mode overlay removed — label is now displayed in the VideoWindow toolbar.
    // Seq recording/done status is visible via timerCallback → repaint of toolbar.
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
    {
        const bool nowSeq = (mode == VideoScrollMode::SeqLoopSimple  ||
                             mode == VideoScrollMode::SeqLoopPingPong ||
                             mode == VideoScrollMode::SeqOneShot);
        if (nowSeq) resetSequence();
        prevMode_ = mode;
    }

    // Update maxSeqFrames from APVTS parameter
    const float maxDur = apvts.getRawParameterValue("videoScrollMaxDuration")->load();
    maxSeqFrames_ = juce::jmax(10, (int)maxDur);

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

    // How many ring frames to consume this tick.
    // Available = frames captured since last tick.
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

    // speed=1.0 → consume all available frames (real-time)
    // speed=0.5 → consume half (slow-motion)
    // speed=2.0 → consume twice (time-lapse, capped by available)
    int toConsume = juce::jlimit(1, available, (int)((float)available * speed));

    // For Seq modes: only record/play one frame per consumed slot
    for (int i = 0; i < toConsume; ++i)
    {
        const int slot = ringReadIdx_ & (kRingSize - 1);
        ++ringReadIdx_;

        const RingFrame& fr = frameRing_[slot];
        if (fr.gray.empty()) continue;
        if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid()) continue;

        // Advance circular write position
        if (reverse)
            writeRow_ = (writeRow_ - 1 + bufH_) % bufH_;
        else
            writeRow_ = (writeRow_ + 1) % bufH_;

        switch (mode)
        {
            case VideoScrollMode::LiveLeftToRight:
                paintRowFromFrame(writeRow_, false, fr);
                break;

            case VideoScrollMode::LiveRightToLeft:
                paintRowFromFrame(writeRow_, true, fr);
                break;

            case VideoScrollMode::LiveDual:
            {
                const int interval = juce::jmax(1, bufH_);
                ++dualCounter_;
                if (dualCounter_ >= interval) { dualCounter_ = 0; dualForward_ = !dualForward_; }
                paintRowFromFrame(writeRow_, !dualForward_, fr);
                break;
            }

            case VideoScrollMode::SeqLoopSimple:
            case VideoScrollMode::SeqLoopPingPong:
            case VideoScrollMode::SeqOneShot:
            {
                if (seqRecording_)
                {
                    paintRowFromFrame(writeRow_, false, fr);
                    appendSeqFrame(fr.r, fr.g, fr.b, fr.gray);
                    if ((int)seqFrames_.size() >= maxSeqFrames_)
                    {
                        seqRecording_ = false;
                        seqPlayHead_  = reverse ? (int)seqFrames_.size() - 1 : 0;
                        seqPingFwd_   = !reverse;
                        seqFinished_  = false;
                    }
                }
                else if (seqFinished_)
                {
                    if (!seqFrames_.empty())
                        paintRowFromSeq(writeRow_, false, seqPlayHead_);
                }
                else
                {
                    paintRowFromSeq(writeRow_, false, seqPlayHead_);
                    advanceSeqPlayHead(mode, reverse);
                }
                break;
            }

            default:
                paintRowFromFrame(writeRow_, false, fr);
                break;
        }
    }
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

void VideoDisplayComponent::advanceSeqPlayHead(VideoScrollMode mode, bool reverse)
{
    const int total = (int)seqFrames_.size();
    if (total == 0) return;
    const int step = reverse ? -1 : 1;

    switch (mode)
    {
        case VideoScrollMode::SeqLoopSimple:
            seqPlayHead_ = (seqPlayHead_ + step + total) % total;
            break;
        case VideoScrollMode::SeqLoopPingPong:
            if (seqPingFwd_)
            {
                seqPlayHead_ += step;
                const bool past = reverse ? (seqPlayHead_ < 0) : (seqPlayHead_ >= total);
                if (past) { seqPlayHead_ = reverse ? 1 : total-2; seqPingFwd_ = false; }
            }
            else
            {
                seqPlayHead_ -= step;
                const bool past = reverse ? (seqPlayHead_ >= total) : (seqPlayHead_ < 0);
                if (past) { seqPlayHead_ = reverse ? total-2 : 1; seqPingFwd_ = true; }
            }
            seqPlayHead_ = juce::jlimit(0, total-1, seqPlayHead_);
            break;
        case VideoScrollMode::SeqOneShot:
            if (reverse) { if (seqPlayHead_ > 0)       --seqPlayHead_; else seqFinished_ = true; }
            else         { if (seqPlayHead_ < total-1) ++seqPlayHead_; else seqFinished_ = true; }
            break;
        default: break;
    }
}
