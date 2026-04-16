#include "VideoDisplayComponent.h"
#include "../PluginProcessor.h"

extern "C"
{
#include "audio/buffers/audio_image_buffers.h"
#include "config/config_loader.h"
}

#include <cstring>
#include <cmath>

//==============================================================================
VideoDisplayComponent::VideoDisplayComponent(Sp3ctraAudioProcessor& proc)
    : processor_(proc)
{
    setOpaque(true);
    startTimerHz(kTimerFps);
}

VideoDisplayComponent::~VideoDisplayComponent()
{
    stopTimer();
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

    if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid())
        return;

    const float zoom = processor_.getAPVTS()
                           .getRawParameterValue("videoScrollZoom")
                           ->load();

    const auto bounds = getLocalBounds().toFloat();

    if (zoom <= 1.0f)
    {
        // Stretch to fill component (no zoom)
        g.drawImage(scrollBuffer_, bounds);
    }
    else
    {
        // Centred zoom-in
        const float scaledW  = bounds.getWidth()  * zoom;
        const float scaledH  = bounds.getHeight() * zoom;
        const float offsetX  = (bounds.getWidth()  - scaledW) * 0.5f;
        const float offsetY  = (bounds.getHeight() - scaledH) * 0.5f;
        g.drawImage(scrollBuffer_,
                    offsetX, offsetY, scaledW, scaledH,
                    0, 0, bufW_, bufH_);
    }

    // Overlay: mode badge + status
    {
        auto& apvts = processor_.getAPVTS();
        const int modeVal = static_cast<int>(
            apvts.getRawParameterValue("videoScrollMode")->load());
        const VideoScrollMode mode = static_cast<VideoScrollMode>(
            juce::jlimit(0, static_cast<int>(VideoScrollMode::COUNT) - 1, modeVal));

        juce::String label = videoScrollModeLabel(mode);
        if (seqRecording_)
        {
            const int pct = (kMaxSeqFrames > 0)
                                ? (int)((float)seqFrames_.size() / kMaxSeqFrames * 100.0f)
                                : 0;
            label += "  [REC " + juce::String(pct) + "%]";
        }
        else if (seqFinished_)
        {
            label += "  [DONE]";
        }

        g.setColour(juce::Colour(0xaa000000));
        g.fillRoundedRectangle(6.f, 6.f, 220.f, 18.f, 4.f);
        g.setColour(juce::Colour(0xff66cc88));
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText(label, 8, 6, 216, 18, juce::Justification::centredLeft, true);
    }
}

//==============================================================================
// Public API
//==============================================================================

void VideoDisplayComponent::resetSequence()
{
    seqFrames_.clear();
    seqPlayHead_  = 0;
    seqPingFwd_   = true;
    seqRecording_ = true;
    seqFinished_  = false;
}

//==============================================================================
// Timer callback
//==============================================================================

void VideoDisplayComponent::timerCallback()
{
    auto& apvts = processor_.getAPVTS();

    const int modeVal = static_cast<int>(
        apvts.getRawParameterValue("videoScrollMode")->load());
    const VideoScrollMode mode = static_cast<VideoScrollMode>(
        juce::jlimit(0, static_cast<int>(VideoScrollMode::COUNT) - 1, modeVal));

    // Detect mode switch → reset sequence when entering a Seq* mode
    if (mode != prevMode_)
    {
        const bool nowSeq = (mode == VideoScrollMode::SeqLoopSimple ||
                             mode == VideoScrollMode::SeqLoopPingPong ||
                             mode == VideoScrollMode::SeqOneShot);
        if (nowSeq)
            resetSequence();
        prevMode_ = mode;
    }

    const float speed = apvts.getRawParameterValue("videoScrollSpeed")->load();

    // Read latest CIS data (always, needed for recording phase of Seq modes)
    readCisData();

    // Advance the waterfall: speed is a multiplier relative to the CIS acquisition rate.
    // Sp3ctra delivers ~1000 lines/sec; timer fires at kTimerFps Hz.
    //   speed=1.0 → 1000/30 ≈ 33 rows/tick → real-time CIS rate
    //   speed=0.1 → 100/30  ≈  3 rows/tick → slow-motion (10× slower)
    //   speed=3.0 → 3000/30 = 100 rows/tick → 3× faster than real-time
    static constexpr float kCisLinesPerSec = 1000.0f;
    scrollAccumulator_ += speed * kCisLinesPerSec / (float)kTimerFps;
    int rowsToAdvance = static_cast<int>(scrollAccumulator_);
    if (rowsToAdvance < 0) rowsToAdvance = 0;
    scrollAccumulator_ -= (float)rowsToAdvance;

    for (int i = 0; i < rowsToAdvance; ++i)
        advanceWaterfall(mode);

    repaint();
}

//==============================================================================
// CIS data reading
//==============================================================================

void VideoDisplayComponent::readCisData()
{
    auto* core = processor_.getSp3ctraCore();
    if (!core) return;

    auto* aib = core->getAudioImageBuffers();
    if (!aib || !aib->initialized) return;

    const int count = get_cis_pixels_nb();
    if (count <= 0) return;

    // Reallocate local buffers if needed
    if (cisCount_ != count)
    {
        cisR_.assign   (count, 0);
        cisG_.assign   (count, 0);
        cisB_.assign   (count, 0);
        cisGray_.assign(count, 0);
        cisCount_ = count;
    }

    // Route to the correct buffer based on source selection.
    // videoScrollSource: 0=L(Live/raw UDP), 1=Sample, 2=Mix, 3=LuxPitch(→Mix)
    const int srcChoice = static_cast<int>(
        processor_.getAPVTS().getRawParameterValue("videoScrollSource")->load());

    uint8_t* pR = nullptr;
    uint8_t* pG = nullptr;
    uint8_t* pB = nullptr;

    switch (srcChoice)
    {
        case 0:  // L — raw live UDP frame (before sampler mix)
            audio_image_buffers_get_raw_pointers(aib, &pR, &pG, &pB);
            break;
        case 1:  // Sample — pure sampler frame (before live mix)
            audio_image_buffers_get_sampler_pointers(aib, &pR, &pG, &pB);
            break;
        case 3:  // LuxPitch — route to whichever buffer LuxPitch is configured to analyze
        {
            // LuxPitch does not own an image buffer; it processes one of L/S/M.
            // Mirror the luxpitch_source_type from g_sp3ctra_config so the video
            // shows exactly what the pitch detection engine sees.
            extern sp3ctra_config_t g_sp3ctra_config;
            const int lpSrc = g_sp3ctra_config.luxpitch_source_type; // 0=SAMPLER 1=LIVE 2=MIX
            if (lpSrc == 1)
                audio_image_buffers_get_raw_pointers    (aib, &pR, &pG, &pB);
            else if (lpSrc == 0)
                audio_image_buffers_get_sampler_pointers(aib, &pR, &pG, &pB);
            else
                audio_image_buffers_get_read_pointers   (aib, &pR, &pG, &pB);
            break;
        }
        case 2:  // Mix — blended live+sampler output (default)
        default:
            audio_image_buffers_get_read_pointers(aib, &pR, &pG, &pB);
            break;
    }

    if (pR && pG && pB)
    {
        std::memcpy(cisR_.data(), pR, (size_t)count);
        std::memcpy(cisG_.data(), pG, (size_t)count);
        std::memcpy(cisB_.data(), pB, (size_t)count);

        // Luma: ITU-R BT.601  Y = 0.299R + 0.587G + 0.114B  (integer approx)
        for (int i = 0; i < count; ++i)
        {
            cisGray_[i] = (uint8_t)(
                ((int)pR[i] * 77 + (int)pG[i] * 150 + (int)pB[i] * 29) >> 8);
        }
    }
}

//==============================================================================
// Waterfall
//==============================================================================

void VideoDisplayComponent::allocateScrollBuffer()
{
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;

    if (w != bufW_ || h != bufH_)
    {
        bufW_ = w;
        bufH_ = h;
        scrollBuffer_ = juce::Image(juce::Image::RGB, w, h, true);
        scrollBuffer_.clear(scrollBuffer_.getBounds(), juce::Colours::black);
        scrollAccumulator_ = 0.0f;
    }
}

void VideoDisplayComponent::advanceWaterfall(VideoScrollMode mode)
{
    if (bufW_ <= 0 || bufH_ <= 0 || !scrollBuffer_.isValid()) return;

    // videoScrollDirection: 0=Forward (new row at bottom, scroll up)
    //                       1=Reverse (new row at top, scroll down)
    const bool reverse = processor_.getAPVTS()
                             .getRawParameterValue("videoScrollDirection")
                             ->load() > 0.5f;

    if (reverse)
    {
        // Shift DOWN: existing rows move from 0..H-2 → 1..H-1
        scrollBuffer_.moveImageSection(0, 1,           // dest: (0, 1)
                                       0, 0,           // src:  (0, 0)
                                       bufW_, bufH_ - 1);
    }
    else
    {
        // Shift UP: existing rows move from 1..H-1 → 0..H-2
        scrollBuffer_.moveImageSection(0, 0,           // dest: (0, 0)
                                       0, 1,           // src:  (0, 1)
                                       bufW_, bufH_ - 1);
    }

    // Target row for the new line
    const int targetRow = reverse ? 0 : (bufH_ - 1);

    switch (mode)
    {
        //── Live modes ─────────────────────────────────────────────────────────
        case VideoScrollMode::LiveLeftToRight:
            paintRowFromLive(targetRow, /*mirror=*/false);
            break;

        case VideoScrollMode::LiveRightToLeft:
            paintRowFromLive(targetRow, /*mirror=*/true);
            break;

        case VideoScrollMode::LiveDual:
        {
            const int flipInterval = juce::jmax(1, bufH_);
            ++dualCounter_;
            if (dualCounter_ >= flipInterval)
            {
                dualCounter_ = 0;
                dualForward_ = !dualForward_;
            }
            paintRowFromLive(targetRow, /*mirror=*/!dualForward_);
            break;
        }

        //── Sequence modes ─────────────────────────────────────────────────────
        case VideoScrollMode::SeqLoopSimple:
        case VideoScrollMode::SeqLoopPingPong:
        case VideoScrollMode::SeqOneShot:
        {
            if (seqRecording_)
            {
                paintRowFromLive(targetRow, /*mirror=*/false);
                appendSeqFrame();
                if ((int)seqFrames_.size() >= kMaxSeqFrames)
                {
                    seqRecording_ = false;
                    // Reverse: start playback from end of buffer
                    seqPlayHead_ = reverse ? (int)seqFrames_.size() - 1 : 0;
                    seqPingFwd_  = !reverse;
                    seqFinished_ = false;
                }
            }
            else if (seqFinished_)
            {
                if (!seqFrames_.empty())
                    paintRowFromSeq(targetRow, /*mirror=*/false);
            }
            else
            {
                paintRowFromSeq(targetRow, /*mirror=*/false);
                advanceSeqPlayHead(mode, reverse);
            }
            break;
        }

        default:
            paintRowFromLive(targetRow, /*mirror=*/false);
            break;
    }
}

//==============================================================================
// Row painters
//==============================================================================

void VideoDisplayComponent::paintRowFromLive(int rowY, bool mirror) const
{
    if (cisCount_ <= 0 || !scrollBuffer_.isValid()) return;

    auto& apvts = processor_.getAPVTS();
    const float brightness = apvts.getRawParameterValue("videoScrollBrightness")->load();
    const bool  invert     = apvts.getRawParameterValue("videoInvertColor")->load() > 0.5f;
    const bool  colorMode  = apvts.getRawParameterValue("videoColorMode")->load() > 0.5f;

    juce::Image::BitmapData bmp(scrollBuffer_, juce::Image::BitmapData::writeOnly);

    for (int px = 0; px < bufW_; ++px)
    {
        // Map display pixel → CIS pixel (with optional mirror)
        int cisIdx = (int)((float)px / (float)bufW_ * (float)cisCount_);
        if (mirror) cisIdx = cisCount_ - 1 - cisIdx;
        cisIdx = juce::jlimit(0, cisCount_ - 1, cisIdx);

        uint8_t r, g, b;
        if (colorMode)
        {
            r = cisR_[cisIdx];
            g = cisG_[cisIdx];
            b = cisB_[cisIdx];
        }
        else
        {
            r = g = b = cisGray_[cisIdx];
        }

        if (invert) { r = 255 - r; g = 255 - g; b = 255 - b; }

        if (brightness != 1.0f)
        {
            r = (uint8_t)juce::jlimit(0, 255, (int)((float)r * brightness));
            g = (uint8_t)juce::jlimit(0, 255, (int)((float)g * brightness));
            b = (uint8_t)juce::jlimit(0, 255, (int)((float)b * brightness));
        }

        bmp.setPixelColour(px, rowY, juce::Colour(r, g, b));
    }
}

void VideoDisplayComponent::paintRowFromSeq(int rowY, bool mirror) const
{
    if (seqFrames_.empty() || !scrollBuffer_.isValid()) return;

    const int frameIdx = juce::jlimit(0, (int)seqFrames_.size() - 1, seqPlayHead_);
    const auto& frame  = seqFrames_[frameIdx];
    const int   count  = (int)frame.gray.size();
    if (count <= 0) return;

    auto& apvts = processor_.getAPVTS();
    const float brightness = apvts.getRawParameterValue("videoScrollBrightness")->load();
    const bool  invert     = apvts.getRawParameterValue("videoInvertColor")->load() > 0.5f;
    const bool  colorMode  = apvts.getRawParameterValue("videoColorMode")->load() > 0.5f;

    juce::Image::BitmapData bmp(scrollBuffer_, juce::Image::BitmapData::writeOnly);

    for (int px = 0; px < bufW_; ++px)
    {
        int cisIdx = (int)((float)px / (float)bufW_ * (float)count);
        if (mirror) cisIdx = count - 1 - cisIdx;
        cisIdx = juce::jlimit(0, count - 1, cisIdx);

        uint8_t r, g, b;
        if (colorMode && !frame.r.empty())
        {
            r = frame.r[cisIdx];
            g = frame.g[cisIdx];
            b = frame.b[cisIdx];
        }
        else
        {
            r = g = b = frame.gray[cisIdx];
        }

        if (invert) { r = 255 - r; g = 255 - g; b = 255 - b; }

        if (brightness != 1.0f)
        {
            r = (uint8_t)juce::jlimit(0, 255, (int)((float)r * brightness));
            g = (uint8_t)juce::jlimit(0, 255, (int)((float)g * brightness));
            b = (uint8_t)juce::jlimit(0, 255, (int)((float)b * brightness));
        }

        bmp.setPixelColour(px, rowY, juce::Colour(r, g, b));
    }
}

//==============================================================================
// Sequence helpers
//==============================================================================

void VideoDisplayComponent::appendSeqFrame()
{
    if (cisCount_ <= 0) return;

    SequenceFrame frame;
    frame.gray = cisGray_;
    frame.r    = cisR_;
    frame.g    = cisG_;
    frame.b    = cisB_;
    seqFrames_.push_back(std::move(frame));
}

void VideoDisplayComponent::advanceSeqPlayHead(VideoScrollMode mode, bool reverse)
{
    const int total = (int)seqFrames_.size();
    if (total == 0) return;

    // Step direction: +1 forward, -1 reverse
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
                const bool pastEnd = reverse ? (seqPlayHead_ < 0)
                                             : (seqPlayHead_ >= total);
                if (pastEnd)
                {
                    seqPlayHead_ = reverse ? 1 : total - 2;
                    seqPingFwd_  = false;
                    seqPlayHead_ = juce::jlimit(0, total - 1, seqPlayHead_);
                }
            }
            else
            {
                seqPlayHead_ -= step;
                const bool pastStart = reverse ? (seqPlayHead_ >= total)
                                               : (seqPlayHead_ < 0);
                if (pastStart)
                {
                    seqPlayHead_ = reverse ? total - 2 : 1;
                    seqPingFwd_  = true;
                    seqPlayHead_ = juce::jlimit(0, total - 1, seqPlayHead_);
                }
            }
            break;

        case VideoScrollMode::SeqOneShot:
            if (reverse)
            {
                if (seqPlayHead_ > 0) --seqPlayHead_;
                else seqFinished_ = true;
            }
            else
            {
                if (seqPlayHead_ < total - 1) ++seqPlayHead_;
                else seqFinished_ = true;
            }
            break;

        default:
            break;
    }
}
