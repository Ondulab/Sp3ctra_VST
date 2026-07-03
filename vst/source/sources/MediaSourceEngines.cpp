/**
 * @file MediaSourceEngines.cpp
 * @brief M9 — IMAGE / VIDEO / CAMERA source engine implementations.
 */
#include "MediaSourceEngines.h"
#include "VideoFileReader.h"

#include <juce_video/juce_video.h>

#include <cmath>

//==============================================================================
// Shared helper
//==============================================================================
void MediaSrc::extractLineFromImage(const juce::Image& img, float frac,
                                    uint8_t* r, uint8_t* g, uint8_t* b, int nPixels)
{
    if (! img.isValid() || nPixels <= 0)
        return;

    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
    const int w = bd.width, h = bd.height;
    if (w <= 0 || h <= 0)
        return;

    const int y = juce::jlimit(0, h - 1, (int) std::lround((double) frac * (h - 1)));
    const uint8_t* line = bd.getLinePointer(y);
    const int ps = bd.pixelStride;   // 4 = ARGB, 3 = RGB, 1 = single channel
                                     // (B,G,R[,A] memory order, little-endian)
    for (int i = 0; i < nPixels; ++i)
    {
        const double xf = (nPixels > 1) ? (double) i * (w - 1) / (nPixels - 1) : 0.0;
        const int    x0 = (int) xf;
        const int    x1 = juce::jmin(x0 + 1, w - 1);
        const double t  = xf - x0;

        const uint8_t* p0 = line + (size_t) x0 * (size_t) ps;
        const uint8_t* p1 = line + (size_t) x1 * (size_t) ps;

        if (ps == 1)
        {
            const double v = (1.0 - t) * p0[0] + t * p1[0];
            r[i] = g[i] = b[i] = (uint8_t) std::lround(v);
        }
        else
        {
            b[i] = (uint8_t) std::lround((1.0 - t) * p0[0] + t * p1[0]);
            g[i] = (uint8_t) std::lround((1.0 - t) * p0[1] + t * p1[1]);
            r[i] = (uint8_t) std::lround((1.0 - t) * p0[2] + t * p1[2]);
        }
    }
}

//==============================================================================
// ImageSourceEngine
//==============================================================================
bool ImageSourceEngine::loadFile(const juce::File& f, juce::String& error)
{
    juce::Image img = juce::ImageFileFormat::loadFrom(f);
    if (! img.isValid())
    {
        error = "Cannot decode image: " + f.getFileName();
        return false;
    }

    const int width = INTERNAL_SRC_MAX_PIXELS;
    int rows = (int) std::lround((double) img.getHeight() * width
                                 / (double) juce::jmax(1, img.getWidth()));
    rows = juce::jlimit(1, 8192, rows);

    // Resample to the chain width once; each row is then a ready-to-publish line.
    juce::Image resized = img.rescaled(width, rows, juce::Graphics::highResamplingQuality);

    std::vector<uint8_t> sr((size_t) rows * width);
    std::vector<uint8_t> sg((size_t) rows * width);
    std::vector<uint8_t> sb((size_t) rows * width);
    {
        juce::Image::BitmapData bd(resized, juce::Image::BitmapData::readOnly);
        const int ps = bd.pixelStride;
        for (int y = 0; y < rows; ++y)
        {
            const uint8_t* line = bd.getLinePointer(y);
            const size_t   o    = (size_t) y * (size_t) width;
            for (int x = 0; x < width; ++x)
            {
                const uint8_t* p = line + (size_t) x * (size_t) ps;
                if (ps == 1) { sr[o + x] = sg[o + x] = sb[o + x] = p[0]; }
                else         { sb[o + x] = p[0]; sg[o + x] = p[1]; sr[o + x] = p[2]; }
            }
        }
    }

    const int prevW = juce::jmin(720, img.getWidth());
    const int prevH = juce::jmax(1, (int) std::lround((double) img.getHeight() * prevW
                                                      / (double) juce::jmax(1, img.getWidth())));
    juce::Image preview = img.rescaled(prevW, prevH, juce::Graphics::highResamplingQuality);

    {
        std::lock_guard<std::mutex> lk(mediaMutex_);
        stripR_  = std::move(sr);
        stripG_  = std::move(sg);
        stripB_  = std::move(sb);
        rows_    = rows;
        width_   = width;
        file_    = f;
        preview_ = preview;
    }
    loaded_.store(true, std::memory_order_release);
    seekPending_.store(true);
    updateActive();
    return true;
}

void ImageSourceEngine::unload()
{
    playing_.store(false);
    loaded_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mediaMutex_);
        stripR_.clear(); stripG_.clear(); stripB_.clear();
        rows_ = width_ = 0;
        file_ = juce::File();
        preview_ = {};
    }
    updateActive();
}

juce::File ImageSourceEngine::getFile() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return file_;
}

juce::Image ImageSourceEngine::getPreviewImage() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return preview_;
}

int ImageSourceEngine::getRowCount() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return rows_;
}

void ImageSourceEngine::setPosition(float frac)
{
    targetPos_.store(juce::jlimit(0.0f, 1.0f, frac));
    seekPending_.store(true);
}

void ImageSourceEngine::setPlaying(bool p)
{
    if (p == playing_.load())
        return;
    if (p)
        justStarted_.store(true);
    playing_.store(p, std::memory_order_release);
}

void ImageSourceEngine::updateActive()
{
    internal_source_set_active(INTERNAL_SRC_IMAGE,
                               present_.load() && loaded_.load() ? 1 : 0);
}

void ImageSourceEngine::publishRow(double frac)
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    if (rows_ <= 0 || width_ <= 0)
        return;
    const int    row = juce::jlimit(0, rows_ - 1, (int) std::lround(frac * (rows_ - 1)));
    const size_t o   = (size_t) row * (size_t) width_;
    internal_source_publish(INTERNAL_SRC_IMAGE,
                            stripR_.data() + o, stripG_.data() + o, stripB_.data() + o,
                            width_);
}

void ImageSourceEngine::tick(double nowMs)
{
    if (! present_.load() || ! loaded_.load())
    {
        lastMs_ = nowMs;
        return;
    }

    const double dt = (lastMs_ > 0.0) ? juce::jlimit(0.0, 0.1, (nowMs - lastMs_) / 1000.0)
                                      : 0.0;
    lastMs_ = nowMs;

    bool doPublish = false;

    if (seekPending_.exchange(false))
    {
        head_ = targetPos_.load();
        doPublish = true;
    }

    if (playing_.load(std::memory_order_acquire))
    {
        const int lm = loopMode_.load();

        if (justStarted_.exchange(false))
        {
            dir_ = (lm == MediaSrc::Reverse) ? -1 : +1;
            // ONCE restart from the matching edge when the head already sits there
            if (lm == MediaSrc::Once && dir_ > 0 && head_ >= 1.0)
                head_ = 0.0;
        }

        head_ += dir_ * (dt / (double) durS_.load());

        switch (lm)
        {
            case MediaSrc::Once:
                if (head_ >= 1.0)
                {
                    head_ = 1.0;
                    playing_.store(false, std::memory_order_release);
                    if (onPlaybackFinished)
                    {
                        auto cb = onPlaybackFinished;
                        juce::MessageManager::callAsync([cb] { cb(); });
                    }
                }
                else if (head_ < 0.0)
                    head_ = 0.0;
                break;

            case MediaSrc::Loop:
            case MediaSrc::Reverse:
                while (head_ > 1.0) head_ -= 1.0;
                while (head_ < 0.0) head_ += 1.0;
                break;

            case MediaSrc::PingPong:
                if (head_ >= 1.0) { head_ = juce::jmax(0.0, 2.0 - head_); dir_ = -1; }
                else if (head_ <= 0.0) { head_ = juce::jmin(1.0, -head_); dir_ = +1; }
                break;
        }
        doPublish = true;
    }

    if (doPublish)
    {
        publishRow(head_);
        headPub_.store((float) head_, std::memory_order_relaxed);
    }
}

//==============================================================================
// VideoSourceEngine
//==============================================================================
VideoSourceEngine::VideoSourceEngine()  : reader_(std::make_unique<VideoFileReader>()) {}
VideoSourceEngine::~VideoSourceEngine() { unload(); }

bool VideoSourceEngine::loadFile(const juce::File& f, juce::String& error)
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    playing_.store(false);
    loaded_.store(false, std::memory_order_release);

    if (! reader_->open(f, error))
    {
        internal_source_set_active(INTERNAL_SRC_VIDEO, 0);
        return false;
    }

    file_     = f;
    frame_    = {};
    preview_  = {};
    dir_      = +1;
    loaded_.store(true, std::memory_order_release);
    lineDirty_.store(true);
    rateDirty_.store(true);
    internal_source_set_active(INTERNAL_SRC_VIDEO, present_.load() ? 1 : 0);
    return true;
}

void VideoSourceEngine::unload()
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    playing_.store(false);
    loaded_.store(false, std::memory_order_release);
    reader_->close();
    file_ = juce::File();
    frame_ = {};
    preview_ = {};
    internal_source_set_active(INTERNAL_SRC_VIDEO, 0);
}

juce::File VideoSourceEngine::getFile() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return file_;
}

juce::Image VideoSourceEngine::getPreviewImage() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return preview_;
}

double VideoSourceEngine::getDurationS() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return reader_->getDurationS();
}

bool VideoSourceEngine::canPlayReverse() const
{
    std::lock_guard<std::mutex> lk(mediaMutex_);
    return reader_->canPlayReverse();
}

void VideoSourceEngine::setPlaying(bool p)
{
    if (p)
        justStarted_.store(true);
    playing_.store(p, std::memory_order_release);
    rateDirty_.store(true);
}

void VideoSourceEngine::seekFrac(double f)
{
    pendingSeek_.store(juce::jlimit(0.0, 1.0, f));
}

double VideoSourceEngine::getPositionFrac() const
{
    return posFracPub_.load(std::memory_order_relaxed);
}

void VideoSourceEngine::updateActive()
{
    internal_source_set_active(INTERNAL_SRC_VIDEO,
                               present_.load() && loaded_.load() ? 1 : 0);
}

void VideoSourceEngine::applyRate()
{
    float rate = 0.0f;
    if (playing_.load())
    {
        rate = speed_.load() * (float) dir_;
        // Reverse unsupported by this media → keep AVPlayer paused; tick()
        // emulates backward playback with step-seeks.
        if (dir_ < 0 && ! reader_->canPlayReverse())
            rate = 0.0f;
    }
    reader_->setRate(rate);
}

void VideoSourceEngine::publishLine()
{
    if (! frame_.isValid())
        return;

    uint8_t r[INTERNAL_SRC_MAX_PIXELS], g[INTERNAL_SRC_MAX_PIXELS], b[INTERNAL_SRC_MAX_PIXELS];
    MediaSrc::extractLineFromImage(frame_, lineFrac_.load(), r, g, b, INTERNAL_SRC_MAX_PIXELS);
    internal_source_publish(INTERNAL_SRC_VIDEO, r, g, b, INTERNAL_SRC_MAX_PIXELS);
}

void VideoSourceEngine::tick(double nowMs)
{
    if (! present_.load() || ! loaded_.load())
        return;

    std::lock_guard<std::mutex> lk(mediaMutex_);
    if (! reader_->isReady())
        return;

    const double dur = reader_->getDurationS();
    if (dur <= 0.0)
        return;

    const double sk = pendingSeek_.exchange(-1.0);
    if (sk >= 0.0)
    {
        reader_->seek(sk * dur);
        lineDirty_.store(true);
    }

    if (justStarted_.exchange(false))
    {
        const int lm = loopMode_.load();
        dir_ = (lm == MediaSrc::Reverse) ? -1 : +1;
        const double pos0 = reader_->getPositionS();
        if (dir_ > 0 && lm == MediaSrc::Once && pos0 >= dur - 0.05)
            reader_->seek(0.0);
        if (dir_ < 0 && pos0 <= 0.05)
            reader_->seek(juce::jmax(0.0, dur - 0.05));
        lastStepMs_ = nowMs;
        rateDirty_.store(true);
    }

    if (rateDirty_.exchange(false))
        applyRate();

    double pos = reader_->getPositionS();

    if (playing_.load(std::memory_order_acquire))
    {
        const int    lm  = loopMode_.load();
        const double eps = 0.05;

        if (dir_ > 0 && pos >= dur - eps)
        {
            switch (lm)
            {
                case MediaSrc::Loop:     reader_->seek(0.0); break;
                case MediaSrc::PingPong: dir_ = -1; applyRate(); break;
                case MediaSrc::Reverse:  dir_ = -1; applyRate(); break;
                case MediaSrc::Once:
                default:
                    playing_.store(false, std::memory_order_release);
                    applyRate();
                    if (onPlaybackFinished)
                    {
                        auto cb = onPlaybackFinished;
                        juce::MessageManager::callAsync([cb] { cb(); });
                    }
                    break;
            }
        }
        else if (dir_ < 0 && pos <= eps)
        {
            switch (lm)
            {
                case MediaSrc::Reverse:  reader_->seek(juce::jmax(0.0, dur - eps)); break;
                case MediaSrc::PingPong: dir_ = +1; applyRate(); break;
                default:                 dir_ = +1; applyRate(); break;
            }
        }

        // Backward playback fallback (media without reverse support): AVPlayer
        // stays paused and we walk the play head with coarse seeks (~15 fps).
        if (dir_ < 0 && ! reader_->canPlayReverse())
        {
            if (nowMs - lastStepMs_ >= 66.0)
            {
                const double step = (nowMs - lastStepMs_) / 1000.0 * speed_.load();
                lastStepMs_ = nowMs;
                reader_->seek(juce::jmax(0.0, pos - step));
                lineDirty_.store(true);
            }
        }
        else
        {
            lastStepMs_ = nowMs;
        }
    }

    const bool newFrame = reader_->pullFrame(frame_);
    if (newFrame || lineDirty_.exchange(false))
        publishLine();

    if (newFrame && (nowMs - lastPreviewMs_) > 100.0 && frame_.isValid())
    {
        const int pw = juce::jmin(640, frame_.getWidth());
        const int ph = juce::jmax(1, frame_.getHeight() * pw / juce::jmax(1, frame_.getWidth()));
        preview_ = frame_.rescaled(pw, ph);
        lastPreviewMs_ = nowMs;
    }

    posFracPub_.store((float) (pos / dur), std::memory_order_relaxed);
}

//==============================================================================
// CameraSourceEngine
//==============================================================================
struct CameraSourceEngine::FrameListener : public juce::CameraDevice::Listener
{
    explicit FrameListener(CameraSourceEngine& e) : owner(e) {}

    void imageReceived(const juce::Image& image) override
    {
        // The macOS implementation hands out a fresh Image per frame, so the
        // ref-counted copy is safe to keep across the callback.
        std::lock_guard<std::mutex> lk(owner.frameMutex_);
        owner.lastFrame_ = image;
        owner.newFrame_.store(true, std::memory_order_release);
    }

    CameraSourceEngine& owner;
};

CameraSourceEngine::CameraSourceEngine()  = default;
CameraSourceEngine::~CameraSourceEngine() { closeDevice(); }

juce::StringArray CameraSourceEngine::getDeviceNames()
{
    return juce::CameraDevice::getAvailableDevices();
}

bool CameraSourceEngine::openDevice(int index, juce::String& error)
{
    closeDevice();

    const auto names = juce::CameraDevice::getAvailableDevices();
    if (index < 0 || index >= names.size())
    {
        error = "Camera index out of range";
        return false;
    }

    device_.reset(juce::CameraDevice::openDevice(index, 128, 64, 1920, 1080, true));
    if (device_ == nullptr)
    {
        error = "Cannot open camera \"" + names[index]
              + "\" (busy, or camera permission denied)";
        return false;
    }

    listener_ = std::make_unique<FrameListener>(*this);
    device_->addListener(listener_.get());

    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        deviceName_ = names[index];
        lastFrame_  = {};
        preview_    = {};
    }
    deviceIndex_.store(index);
    open_.store(true, std::memory_order_release);
    updateActive();
    return true;
}

void CameraSourceEngine::closeDevice()
{
    open_.store(false, std::memory_order_release);
    updateActive();

    if (device_ != nullptr && listener_ != nullptr)
        device_->removeListener(listener_.get());
    device_.reset();
    listener_.reset();

    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        deviceName_ = {};
        lastFrame_  = {};
        preview_    = {};
    }
    deviceIndex_.store(-1);
}

juce::String CameraSourceEngine::getOpenDeviceName() const
{
    std::lock_guard<std::mutex> lk(frameMutex_);
    return deviceName_;
}

juce::Image CameraSourceEngine::getPreviewImage() const
{
    std::lock_guard<std::mutex> lk(frameMutex_);
    return preview_;
}

void CameraSourceEngine::updateActive()
{
    internal_source_set_active(INTERNAL_SRC_CAMERA,
                               present_.load() && open_.load() ? 1 : 0);
}

void CameraSourceEngine::tick(double nowMs)
{
    if (! present_.load() || ! open_.load())
        return;

    const bool fresh = newFrame_.exchange(false);
    if (! fresh && ! lineDirty_.exchange(false))
        return;

    juce::Image frame;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        frame = lastFrame_;
    }
    if (! frame.isValid())
        return;

    uint8_t r[INTERNAL_SRC_MAX_PIXELS], g[INTERNAL_SRC_MAX_PIXELS], b[INTERNAL_SRC_MAX_PIXELS];
    MediaSrc::extractLineFromImage(frame, lineFrac_.load(), r, g, b, INTERNAL_SRC_MAX_PIXELS);
    internal_source_publish(INTERNAL_SRC_CAMERA, r, g, b, INTERNAL_SRC_MAX_PIXELS);

    if (fresh && (nowMs - lastPreviewMs_) > 100.0)
    {
        const int pw = juce::jmin(640, frame.getWidth());
        const int ph = juce::jmax(1, frame.getHeight() * pw / juce::jmax(1, frame.getWidth()));
        auto scaled = frame.rescaled(pw, ph);
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            preview_ = scaled;
        }
        lastPreviewMs_ = nowMs;
    }
}
