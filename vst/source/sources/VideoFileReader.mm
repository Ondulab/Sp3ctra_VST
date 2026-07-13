/**
 * @file VideoFileReader.mm
 * @brief M9 — AVFoundation implementation of VideoFileReader (macOS).
 */
#include "VideoFileReader.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>   // CACurrentMediaTime

struct VideoFileReader::Impl
{
    AVPlayer*                playere   = nil;
    AVPlayerItem*            item      = nil;
    AVPlayerItemVideoOutput* output    = nil;
    AVURLAsset*              asset     = nil;

    double duration   = 0.0;
    double fps        = 0.0;
    int    width      = 0;
    int    height     = 0;
    bool   reverseOk  = false;
    bool   open       = false;

    void teardown()
    {
        @autoreleasepool
        {
            if (playere != nil)
            {
                [playere pause];
                [playere replaceCurrentItemWithPlayerItem:nil];
            }
            if (item != nil && output != nil)
                [item removeOutput:output];

            [output release];  output  = nil;
            [item release];    item    = nil;
            [playere release]; playere = nil;
            [asset release];   asset   = nil;
        }
        duration = fps = 0.0;
        width = height = 0;
        reverseOk = false;
        open = false;
    }
};

VideoFileReader::VideoFileReader() : impl(std::make_unique<Impl>()) {}
VideoFileReader::~VideoFileReader() { close(); }

bool VideoFileReader::open(const juce::File& file, juce::String& error)
{
    close();

    if (! file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return false;
    }

    @autoreleasepool
    {
        NSURL* url = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];

        impl->asset = [[AVURLAsset alloc] initWithURL:url options:nil];

        // Keep open()'s synchronous contract by blocking on the modern async
        // loader (loadTracksWithMediaType: replaces the deprecated synchronous
        // tracksWithMediaType:). The completion is delivered on an internal
        // AVFoundation queue, never the caller's thread, so the semaphore wait
        // can't deadlock — local files resolve near-instantly.
        __block NSArray<AVAssetTrack*>* tracks = nil;
        dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
        [impl->asset loadTracksWithMediaType:AVMediaTypeVideo
                           completionHandler:^(NSArray<AVAssetTrack*>* result,
                                               NSError* err)
        {
            (void) err;
            tracks = [result retain];   // MRR: survive past the completion scope
            dispatch_semaphore_signal(loaded);
        }];
        dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
        [tracks autorelease];
        if (tracks.count == 0)
        {
            error = "No video track in " + file.getFileName();
            impl->teardown();
            return false;
        }

        AVAssetTrack* track = tracks.firstObject;
        const CGSize sz     = track.naturalSize;
        impl->width         = (int) sz.width;
        impl->height        = (int) sz.height;
        impl->fps           = (double) track.nominalFrameRate;
        impl->duration      = CMTimeGetSeconds(impl->asset.duration);

        NSDictionary* attrs = @{
            (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        };
        impl->output = [[AVPlayerItemVideoOutput alloc]
                            initWithPixelBufferAttributes:attrs];

        impl->item = [[AVPlayerItem alloc] initWithAsset:impl->asset];
        [impl->item addOutput:impl->output];

        impl->playere = [[AVPlayer alloc] initWithPlayerItem:impl->item];
        impl->playere.muted = YES;
        impl->playere.actionAtItemEnd = AVPlayerActionAtItemEndPause;

        impl->reverseOk = impl->item.canPlayReverse;
        impl->open = true;
    }
    return true;
}

void VideoFileReader::close()                { impl->teardown(); }
bool VideoFileReader::isOpen() const         { return impl->open; }

bool VideoFileReader::isReady() const
{
    return impl->open && impl->item != nil
        && impl->item.status == AVPlayerItemStatusReadyToPlay;
}

double VideoFileReader::getDurationS() const { return impl->duration; }
double VideoFileReader::getNominalFps() const{ return impl->fps; }
int    VideoFileReader::getWidth()  const    { return impl->width; }
int    VideoFileReader::getHeight() const    { return impl->height; }
bool   VideoFileReader::canPlayReverse() const { return impl->reverseOk; }

void VideoFileReader::setRate(float rate)
{
    if (impl->playere != nil)
        impl->playere.rate = rate;
}

float VideoFileReader::getRate() const
{
    return impl->playere != nil ? (float) impl->playere.rate : 0.0f;
}

void VideoFileReader::seek(double seconds)
{
    if (impl->playere == nil)
        return;
    @autoreleasepool
    {
        const CMTime t = CMTimeMakeWithSeconds(seconds, 600);
        [impl->playere seekToTime:t
                  toleranceBefore:kCMTimeZero
                   toleranceAfter:kCMTimeZero];
    }
}

double VideoFileReader::getPositionS() const
{
    if (impl->playere == nil)
        return 0.0;
    const CMTime t = impl->playere.currentTime;
    const double s = CMTimeGetSeconds(t);
    return std::isfinite(s) ? s : 0.0;
}

bool VideoFileReader::pullFrame(juce::Image& target)
{
    if (! isReady() || impl->output == nil)
        return false;

    @autoreleasepool
    {
        const CMTime now =
            [impl->output itemTimeForHostTime:CACurrentMediaTime()];

        if (! [impl->output hasNewPixelBufferForItemTime:now])
            return false;

        CVPixelBufferRef buf =
            [impl->output copyPixelBufferForItemTime:now itemTimeForDisplay:nil];
        if (buf == nullptr)
            return false;

        CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

        const int w      = (int) CVPixelBufferGetWidth(buf);
        const int h      = (int) CVPixelBufferGetHeight(buf);
        const int stride = (int) CVPixelBufferGetBytesPerRow(buf);
        const uint8_t* src = (const uint8_t*) CVPixelBufferGetBaseAddress(buf);

        if (w > 0 && h > 0 && src != nullptr)
        {
            if (target.getWidth() != w || target.getHeight() != h
                || target.getFormat() != juce::Image::ARGB)
                target = juce::Image(juce::Image::ARGB, w, h, false);

            juce::Image::BitmapData bd(target, juce::Image::BitmapData::writeOnly);
            // kCVPixelFormatType_32BGRA byte order == JUCE ARGB (little-endian)
            for (int y = 0; y < h; ++y)
                memcpy(bd.getLinePointer(y), src + (size_t) y * (size_t) stride,
                       (size_t) w * 4);
        }

        CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
        CVBufferRelease(buf);
        return w > 0 && h > 0;
    }
}
