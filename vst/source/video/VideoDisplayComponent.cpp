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
        case 3: // LuxPitch — route to whatever luxpitch_source_type reads
        {
            extern sp3ctra_config_t g_sp3ctra_config;
            const int lp = g_sp3ctra_config.luxpitch_source_type;
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
    if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid()) return;

    // ── Circular rendering (no memcpy): two-pass draw ──────────────────────
    // writeRow_ is the NEXT row to be written → the "oldest" visible row.
    // Layout on screen (Forward direction, newest at bottom):
    //   [writeRow_ .. bufH_-1] → screen top    (oldest data)
    //   [0 .. writeRow_-1]     → screen bottom  (newest data)
    //
    // For Reverse (newest at top): swap the two halves.
    //
    // We stretch both halves to fill the component bounds.
    // ──────────────────────────────────────────────────────────────────────

    const auto& apvts = processor_.getAPVTS();
    const float zoom    = apvts.getRawParameterValue("videoScrollZoom")->load();
    const bool reverse  = apvts.getRawParameterValue("videoScrollDirection")->load() > 0.5f;

    const float compW = (float)getWidth();
    const float compH = (float)getHeight();

    // Zoom: zoomed in → we draw a smaller source rect centred on the image.
    // srcW/srcX apply horizontally; vertical split is handled by the two-pass circular draw.
    const float srcW = (zoom > 0.0f) ? ((float)bufW_ / zoom) : (float)bufW_;
    const float srcX = ((float)bufW_ - srcW) * 0.5f;
    (void)srcX; // horizontal zoom not yet applied to split-draw; kept for future use

    // Heights of the two halves in buffer pixels
    const int topRows = reverse ? writeRow_          : (bufH_ - writeRow_);
    const int botRows = reverse ? (bufH_ - writeRow_): writeRow_;

    const int topSrcY  = reverse ? 0                 : writeRow_;
    const int botSrcY  = reverse ? writeRow_          : 0;

    const float topScreenH = compH * (float)topRows / (float)bufH_;
    const float botScreenH = compH - topScreenH;

    if (topRows > 0)
    {
        g.drawImage(scrollBuffer_,
                    0.f, 0.f, compW, topScreenH,
                    (int)srcX, topSrcY, bufW_, topRows);
    }
    if (botRows > 0)
    {
        g.drawImage(scrollBuffer_,
                    0.f, topScreenH, compW, botScreenH,
                    (int)srcX, botSrcY, bufW_, botRows);
    }

    // ── Mode / status overlay ──────────────────────────────────────────────
    {
        const int modeVal = static_cast<int>(
            apvts.getRawParameterValue("videoScrollMode")->load());
        const VideoScrollMode mode = static_cast<VideoScrollMode>(
            juce::jlimit(0, (int)VideoScrollMode::COUNT - 1, modeVal));

        juce::String label = videoScrollModeLabel(mode);
        if (seqRecording_)
        {
            const int pct = (maxSeqFrames_ > 0)
                ? (int)((float)seqFrames_.size() / (float)maxSeqFrames_ * 100.f)
                : 0;
            label += "  [REC " + juce::String(pct) + "%]";
        }
        else if (seqFinished_)
            label += "  [DONE]";

        g.setColour(juce::Colour(0xaa000000));
        g.fillRoundedRectangle(6.f, 6.f, 220.f, 18.f, 4.f);
        g.setColour(juce::Colour(0xff66cc88));
        g.setFont(juce::Font(juce::FontOptions(10.f)));
        g.drawText(label, 8, 6, 216, 18, juce::Justification::centredLeft, true);
    }
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

    // speed=1.0 → consume all available frames (real-time)
    // speed=0.5 → consume half (slow-motion)
    // speed=2.0 → consume twice (time-lapse, capped by available)
    int toConsume = juce::jlimit(1, available, (int)((float)available * speed));

    // For Seq modes: only record/play one frame per consumed slot
    for (int i = 0; i < toConsume; ++i)
    {
        const int slot = ringReadIdx_ & (kRingSize - 1);
        ++ringReadIdx_;

        const auto& fr = frameRing_[slot];
        if (fr.gray.empty()) continue;

        if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid()) continue;

        // Advance write position
        if (reverse)
            writeRow_ = (writeRow_ - 1 + bufH_) % bufH_;
        else
            writeRow_ = (writeRow_ + 1) % bufH_;

        switch (mode)
        {
            case VideoScrollMode::LiveLeftToRight:
                paintRowFromRing(writeRow_, false);
                break;
            case VideoScrollMode::LiveRightToLeft:
                paintRowFromRing(writeRow_, true);
                break;
            case VideoScrollMode::LiveDual:
            {
                const int interval = juce::jmax(1, bufH_);
                ++dualCounter_;
                if (dualCounter_ >= interval) { dualCounter_ = 0; dualForward_ = !dualForward_; }
                paintRowFromRing(writeRow_, !dualForward_);
                break;
            }
            case VideoScrollMode::SeqLoopSimple:
            case VideoScrollMode::SeqLoopPingPong:
            case VideoScrollMode::SeqOneShot:
            {
                if (seqRecording_)
                {
                    paintRowFromRing(writeRow_, false);
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
                paintRowFromRing(writeRow_, false);
                break;
        }
    }
}

//==============================================================================
// Row painters
//==============================================================================

void VideoDisplayComponent::paintRowFromRing(int rowY, bool mirror)
{
    const int slot = (ringReadIdx_ - 1) & (kRingSize - 1);
    const auto& fr = frameRing_[slot];
    const int count = (int)fr.gray.size();
    if (count <= 0 || !scrollBuffer_.isValid()) return;

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

        uint8_t r = colorMode ? fr.r[ci] : fr.gray[ci];
        uint8_t gv = colorMode ? fr.g[ci] : fr.gray[ci];
        uint8_t b = colorMode ? fr.b[ci] : fr.gray[ci];

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
        bufW_     = w;
        bufH_     = h;
        writeRow_ = 0;
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
