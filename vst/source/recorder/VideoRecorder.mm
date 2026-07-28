/**
 * @file VideoRecorder.mm
 * @brief AVFoundation implementation of VideoRecorder (macOS).
 *
 * AVAssetWriter → .mov, HEVC video + AAC audio. A single background writer
 * thread ("VideoRecWriter") drains two producer-fed queues and appends to the
 * writer inputs:
 *   - video : a small mutex-guarded deque of ready CVPixelBuffers (produced on
 *             the mixer's render thread by pushVideoFrame);
 *   - audio : a lock-free SPSC frame ring (produced on the RT audio thread by
 *             pushAudio — no alloc, no lock, drops on overrun).
 *
 * A/V sync: the writer session starts at kCMTimeZero. Video is VFR, stamped
 * with the real wall-clock PTS handed in by the render thread. Audio PTS is the
 * running sample count / sampleRate. No resampling.
 *
 * Manual retain/release (no ARC, matching VideoFileReader.mm).
 */
#include "VideoRecorder.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <pthread.h>

//==============================================================================
namespace
{
    constexpr int    kVideoTimescale   = 90000;   // CMTime timescale for video PTS
    constexpr size_t kVideoQueueMax     = 12;      // frames buffered before drop
    constexpr int    kAudioChunkFrames  = 2048;    // frames appended per audio buffer
    constexpr double kFinishTimeoutSec  = 10.0;
    constexpr double kStopDrainMs       = 2000.0;  // best-effort tail flush on stop
}

//==============================================================================
struct VideoRecorder::Impl
{
    // ── AVFoundation objects (owned; released in teardown) ───────────────────
    AVAssetWriter*                          writer    = nil;
    AVAssetWriterInput*                     vInput    = nil;
    AVAssetWriterInputPixelBufferAdaptor*   vAdaptor  = nil;
    AVAssetWriterInput*                     aInput    = nil;
    CMFormatDescriptionRef                  audioFmt  = nullptr;

    // ── Fixed stream parameters ──────────────────────────────────────────────
    int    width       = 0;
    int    height      = 0;
    double fps         = 60.0;
    double sampleRate  = 48000.0;
    int    numCh       = 2;

    // ── State ────────────────────────────────────────────────────────────────
    std::atomic<bool> recording    { false };  // start() succeeded, not yet stopped
    std::atomic<bool> accepting    { false };  // producers may push
    std::atomic<bool> stopRequested{ false };
    mutable std::mutex errMutex;
    juce::String       error;

    // ── Video producer → writer queue ────────────────────────────────────────
    struct VidFrame { CVPixelBufferRef buf; CMTime pts; };
    std::mutex             vqMutex;
    std::deque<VidFrame>   vq;
    std::atomic<uint64_t>  vDropped { 0 };

    // ── Audio SPSC frame ring (producer: RT thread; consumer: writer thread) ──
    std::vector<float>     aRing;               // framesCap * numCh, interleaved
    int                    framesCap = 0;
    std::atomic<uint64_t>  aWrite    { 0 };     // total frames written
    std::atomic<uint64_t>  aRead     { 0 };     // total frames read
    std::atomic<uint64_t>  aDropped  { 0 };
    uint64_t               audioFramesWritten = 0;   // writer thread only → PTS
    std::vector<float>     drainTmp;            // writer-thread scratch (interleaved)

    // ── Writer thread ────────────────────────────────────────────────────────
    std::thread             thread;
    std::mutex              cvMutex;
    std::condition_variable cv;

    //--------------------------------------------------------------------------
    void setError(const juce::String& e)
    {
        std::lock_guard<std::mutex> lk(errMutex);
        if (error.isEmpty()) error = e;
    }

    void teardown()
    {
        @autoreleasepool
        {
            [vInput release];   vInput   = nil;
            [vAdaptor release]; vAdaptor = nil;
            [aInput release];   aInput   = nil;
            [writer release];   writer   = nil;
        }
        if (audioFmt != nullptr) { CFRelease(audioFmt); audioFmt = nullptr; }

        { std::lock_guard<std::mutex> lk(vqMutex);
          for (auto& f : vq) CVPixelBufferRelease(f.buf);
          vq.clear(); }

        aRing.clear(); drainTmp.clear();
        framesCap = 0;
        aWrite = aRead = 0; aDropped = 0; vDropped = 0;
        audioFramesWritten = 0;
    }

    //--------------------------------------------------------------------------
    // Consumer (writer thread): copy up to `maxFrames` frames out of the ring
    // into `out` (interleaved). Returns frames copied (0 = ring empty).
    int drainAudio(std::vector<float>& out, int maxFrames)
    {
        const uint64_t r = aRead.load(std::memory_order_relaxed);
        const uint64_t w = aWrite.load(std::memory_order_acquire);
        const uint64_t avail = w - r;
        if (avail == 0) return 0;

        const int n = (int) std::min<uint64_t>(avail, (uint64_t) maxFrames);
        if ((int) out.size() < n * numCh) out.resize((size_t) n * numCh);

        for (int f = 0; f < n; ++f)
        {
            const size_t src = (size_t) ((r + (uint64_t) f) % (uint64_t) framesCap) * numCh;
            std::memcpy(&out[(size_t) f * numCh], &aRing[src], (size_t) numCh * sizeof(float));
        }
        aRead.store(r + (uint64_t) n, std::memory_order_release);
        return n;
    }

    // Writer thread: wrap `numFrames` interleaved float frames into a
    // CMSampleBuffer and append to the audio input.
    void appendAudio(const float* interleaved, int numFrames)
    {
        if (numFrames <= 0 || audioFmt == nullptr) return;
        const size_t dataSize = (size_t) numFrames * numCh * sizeof(float);

        CMBlockBufferRef block = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, dataSize, kCFAllocatorDefault, nullptr,
            0, dataSize, kCMBlockBufferAssureMemoryNowFlag, &block);
        if (st != noErr || block == nullptr) return;

        st = CMBlockBufferReplaceDataBytes(interleaved, block, 0, dataSize);
        if (st != noErr) { CFRelease(block); return; }

        CMSampleTimingInfo timing;
        timing.duration             = CMTimeMake(1, (int32_t) sampleRate);
        timing.presentationTimeStamp= CMTimeMake((int64_t) audioFramesWritten,
                                                 (int32_t) sampleRate);
        timing.decodeTimeStamp      = kCMTimeInvalid;

        CMSampleBufferRef sbuf = nullptr;
        st = CMSampleBufferCreate(kCFAllocatorDefault, block, TRUE, nullptr, nullptr,
                                  audioFmt, (CMItemCount) numFrames, 1, &timing,
                                  0, nullptr, &sbuf);
        CFRelease(block);
        if (st != noErr || sbuf == nullptr) return;

        if ([aInput isReadyForMoreMediaData])
            [aInput appendSampleBuffer:sbuf];
        CFRelease(sbuf);
        audioFramesWritten += (uint64_t) numFrames;
    }

    //--------------------------------------------------------------------------
    void run()
    {
        pthread_setname_np("VideoRecWriter");
        double drainDeadline = 0.0;

        for (;;)
        {
            bool didWork = false;

            // ── Video: append every ready queued frame ───────────────────────
            for (;;)
            {
                if (! [vInput isReadyForMoreMediaData]) break;
                VidFrame vf;
                { std::lock_guard<std::mutex> lk(vqMutex);
                  if (vq.empty()) break;
                  vf = vq.front(); vq.pop_front(); }
                @autoreleasepool { [vAdaptor appendPixelBuffer:vf.buf withPresentationTime:vf.pts]; }
                CVPixelBufferRelease(vf.buf);
                didWork = true;
            }

            // ── Audio: append while the input keeps accepting ────────────────
            while ([aInput isReadyForMoreMediaData])
            {
                const int n = drainAudio(drainTmp, kAudioChunkFrames);
                if (n == 0) break;
                appendAudio(drainTmp.data(), n);
                didWork = true;
            }

            // ── Stop when drained (or after a bounded tail-flush window) ──────
            if (stopRequested.load(std::memory_order_acquire))
            {
                if (drainDeadline == 0.0)
                    drainDeadline = juce::Time::getMillisecondCounterHiRes() + kStopDrainMs;

                bool vEmpty;
                { std::lock_guard<std::mutex> lk(vqMutex); vEmpty = vq.empty(); }
                const bool aEmpty = (aWrite.load(std::memory_order_acquire)
                                     - aRead.load(std::memory_order_relaxed)) == 0;
                if ((vEmpty && aEmpty)
                    || juce::Time::getMillisecondCounterHiRes() > drainDeadline)
                    break;
            }

            if (! didWork)
            {
                std::unique_lock<std::mutex> lk(cvMutex);
                cv.wait_for(lk, std::chrono::milliseconds(4));
            }
        }

        // ── Finalise the file ────────────────────────────────────────────────
        [vInput markAsFinished];
        [aInput markAsFinished];

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
        dispatch_semaphore_wait(done,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t) (kFinishTimeoutSec * NSEC_PER_SEC)));

        if (writer.status == AVAssetWriterStatusFailed && writer.error != nil)
            setError(juce::String::fromUTF8(writer.error.localizedDescription.UTF8String));
    }
};

//==============================================================================
VideoRecorder::VideoRecorder() : impl(std::make_unique<Impl>()) {}
VideoRecorder::~VideoRecorder() { stop(); }

//==============================================================================
bool VideoRecorder::start(const juce::File& out, int w, int h, double fps,
                          double sampleRate, int numAudioCh, juce::String& err)
{
    stop();   // idempotent — clears any prior session

    if (w < 2 || h < 2) { err = "Invalid video dimensions"; return false; }
    w &= ~1; h &= ~1;                                  // even dimensions
    numAudioCh = juce::jlimit(1, 8, numAudioCh);

    impl->width      = w;
    impl->height     = h;
    impl->fps        = (fps > 1.0 ? fps : 60.0);
    impl->sampleRate = (sampleRate > 0.0 ? sampleRate : 48000.0);
    impl->numCh      = numAudioCh;
    { std::lock_guard<std::mutex> lk(impl->errMutex); impl->error.clear(); }

    // AVFoundation throws NSInvalidArgumentException on bad settings (e.g. an
    // unsupported audio rate). Convert any such throw into a clean failure so
    // it can never unwind through start() and half-arm the recorder.
    @try
    {
    @autoreleasepool
    {
        out.deleteFile();
        NSURL* url = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String: out.getFullPathName().toRawUTF8()]];

        NSError* nsErr = nil;
        impl->writer = [[AVAssetWriter alloc] initWithURL:url
                                                 fileType:AVFileTypeQuickTimeMovie
                                                    error:&nsErr];
        if (impl->writer == nil)
        {
            err = "AVAssetWriter: " + (nsErr != nil
                     ? juce::String::fromUTF8(nsErr.localizedDescription.UTF8String)
                     : juce::String("init failed"));
            impl->teardown();
            return false;
        }

        // ── Video input (HEVC, high bitrate) ─────────────────────────────────
        const double bppf   = 0.14;   // bits per pixel per frame → HQ HEVC
        long long bitrate   = (long long) ((double) w * (double) h * impl->fps * bppf);
        bitrate = juce::jlimit<long long>(8'000'000, 120'000'000, bitrate);

        NSDictionary* compression = @{
            AVVideoAverageBitRateKey            : @(bitrate),
            AVVideoExpectedSourceFrameRateKey   : @((int) impl->fps),
            AVVideoMaxKeyFrameIntervalKey       : @((int) (impl->fps * 2.0)),
            AVVideoAllowFrameReorderingKey      : @NO,
        };
        NSDictionary* videoSettings = @{
            AVVideoCodecKey                     : AVVideoCodecTypeHEVC,
            AVVideoWidthKey                     : @(w),
            AVVideoHeightKey                    : @(h),
            AVVideoCompressionPropertiesKey     : compression,
        };
        impl->vInput = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeVideo outputSettings:videoSettings];
        impl->vInput.expectsMediaDataInRealTime = YES;

        if (! [impl->writer canAddInput:impl->vInput])
        {
            err = "Video input rejected (HEVC unavailable?)";
            impl->teardown();
            return false;
        }
        [impl->writer addInput:impl->vInput];

        NSDictionary* srcAttrs = @{
            (id) kCVPixelBufferPixelFormatTypeKey     : @(kCVPixelFormatType_32BGRA),
            (id) kCVPixelBufferWidthKey               : @(w),
            (id) kCVPixelBufferHeightKey              : @(h),
            (id) kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        impl->vAdaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:impl->vInput
            sourcePixelBufferAttributes:srcAttrs];

        // ── Audio input (AAC) ────────────────────────────────────────────────
        // The AAC encoder rejects rates above 48 kHz (throws for e.g. 96 kHz).
        // Cap the OUTPUT rate; the appended LPCM stays at the true source rate
        // and AVFoundation resamples during encode, so the timeline is preserved.
        const double aacRate = impl->sampleRate > 48000.0 ? 48000.0 : impl->sampleRate;
        NSDictionary* audioSettings = @{
            AVFormatIDKey          : @(kAudioFormatMPEG4AAC),
            AVNumberOfChannelsKey  : @(numAudioCh),
            AVSampleRateKey        : @(aacRate),
            AVEncoderBitRateKey    : @(256000),
        };
        impl->aInput = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeAudio outputSettings:audioSettings];
        impl->aInput.expectsMediaDataInRealTime = YES;
        if ([impl->writer canAddInput:impl->aInput])
            [impl->writer addInput:impl->aInput];
        else { err = "Audio input rejected"; impl->teardown(); return false; }

        // ── Source LPCM format description for the appended sample buffers ────
        AudioStreamBasicDescription asbd;
        std::memset(&asbd, 0, sizeof(asbd));
        asbd.mSampleRate       = impl->sampleRate;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mFramesPerPacket  = 1;
        asbd.mChannelsPerFrame = (UInt32) numAudioCh;
        asbd.mBitsPerChannel   = 32;
        asbd.mBytesPerFrame    = (UInt32) numAudioCh * sizeof(float);
        asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
        if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr,
                                           0, nullptr, nullptr, &impl->audioFmt) != noErr)
        {
            err = "CMAudioFormatDescription failed";
            impl->teardown();
            return false;
        }

        // ── Go live ──────────────────────────────────────────────────────────
        if (! [impl->writer startWriting])
        {
            err = "startWriting failed: " + (impl->writer.error != nil
                     ? juce::String::fromUTF8(impl->writer.error.localizedDescription.UTF8String)
                     : juce::String("unknown"));
            impl->teardown();
            return false;
        }
        [impl->writer startSessionAtSourceTime:kCMTimeZero];
    }
    }
    @catch (NSException* ex)
    {
        err = "AVFoundation exception: " + juce::String::fromUTF8(
                  ex.reason != nil ? ex.reason.UTF8String : "unknown");
        impl->teardown();
        return false;
    }

    // Allocate the audio ring (≈2 s) and start the writer thread.
    impl->framesCap = juce::jmax(1, (int) (impl->sampleRate * 2.0));
    impl->aRing.assign((size_t) impl->framesCap * impl->numCh, 0.0f);
    impl->drainTmp.assign((size_t) kAudioChunkFrames * impl->numCh, 0.0f);
    impl->audioFramesWritten = 0;
    impl->aWrite = impl->aRead = 0;

    impl->stopRequested = false;
    impl->recording     = true;
    impl->accepting     = true;
    impl->thread = std::thread([p = impl.get()] { p->run(); });

    return true;
}

//==============================================================================
void VideoRecorder::stop()
{
    if (! impl->recording.load())
    {
        if (impl->thread.joinable()) impl->thread.join();
        return;
    }

    impl->accepting.store(false, std::memory_order_release);
    impl->stopRequested.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(impl->cvMutex); }
    impl->cv.notify_all();

    if (impl->thread.joinable())
        impl->thread.join();

    impl->recording.store(false, std::memory_order_release);
    impl->teardown();
}

//==============================================================================
bool VideoRecorder::isRecording() const noexcept
{
    return impl->recording.load(std::memory_order_acquire);
}

juce::String VideoRecorder::lastError() const
{
    std::lock_guard<std::mutex> lk(impl->errMutex);
    return impl->error;
}

//==============================================================================
void VideoRecorder::pushVideoFrame(const juce::Image& composite, double tSeconds)
{
    if (! impl->accepting.load(std::memory_order_acquire)) return;
    if (impl->vAdaptor == nil) return;

    CVPixelBufferPoolRef pool = impl->vAdaptor.pixelBufferPool;
    if (pool == nullptr) return;   // pool ready only after startSession

    // The mixer renders at exactly recW×recH, but stay robust to a mismatch.
    juce::Image img = composite;
    if (img.getWidth() != impl->width || img.getHeight() != impl->height)
        img = img.rescaled(impl->width, impl->height, juce::Graphics::highResamplingQuality);
    if (img.getFormat() != juce::Image::ARGB)
        img = img.convertedToFormat(juce::Image::ARGB);

    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) != kCVReturnSuccess
        || pb == nullptr)
        return;

    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t*     dst       = (uint8_t*) CVPixelBufferGetBaseAddress(pb);
    const size_t dstStride = CVPixelBufferGetBytesPerRow(pb);
    if (dst != nullptr)
    {
        juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
        const size_t rowBytes = (size_t) impl->width * 4;
        // JUCE ARGB byte order == kCVPixelFormatType_32BGRA (little-endian).
        for (int y = 0; y < impl->height; ++y)
            std::memcpy(dst + (size_t) y * dstStride, bd.getLinePointer(y), rowBytes);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    const CMTime pts = CMTimeMakeWithSeconds(juce::jmax(0.0, tSeconds), kVideoTimescale);

    { std::lock_guard<std::mutex> lk(impl->vqMutex);
      if (impl->vq.size() >= kVideoQueueMax)
      {
          CVPixelBufferRelease(pb);          // writer behind → drop this frame (VFR-safe)
          impl->vDropped.fetch_add(1, std::memory_order_relaxed);
          return;
      }
      impl->vq.push_back({ pb, pts }); }
    impl->cv.notify_all();
}

//==============================================================================
void VideoRecorder::pushAudio(const float* const* chans, int numCh, int numSamples)
{
    if (! impl->accepting.load(std::memory_order_acquire)) return;
    if (numSamples <= 0 || chans == nullptr || impl->framesCap == 0) return;

    const int dstCh = impl->numCh;
    const int srcCh = juce::jmax(1, numCh);

    const uint64_t wr    = impl->aWrite.load(std::memory_order_relaxed);
    const uint64_t rd    = impl->aRead.load(std::memory_order_acquire);
    const uint64_t used  = wr - rd;
    const uint64_t freeF = (uint64_t) impl->framesCap - used;
    const int toWrite    = (int) juce::jmin<uint64_t>((uint64_t) numSamples, freeF);

    for (int f = 0; f < toWrite; ++f)
    {
        const size_t base = (size_t) ((wr + (uint64_t) f) % (uint64_t) impl->framesCap) * dstCh;
        for (int c = 0; c < dstCh; ++c)
        {
            const int sc = (c < srcCh) ? c : (srcCh - 1);   // mono→stereo dup
            impl->aRing[base + (size_t) c] = chans[sc][f];
        }
    }
    impl->aWrite.store(wr + (uint64_t) toWrite, std::memory_order_release);

    if (toWrite < numSamples)
        impl->aDropped.fetch_add((uint64_t) (numSamples - toWrite), std::memory_order_relaxed);
}
