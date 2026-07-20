/**
 * @file VideoFileReaderMF.cpp
 * @brief Windows implementation of VideoFileReader on Media Foundation.
 *
 * Mirrors the semantics of the macOS AVFoundation reader (VideoFileReader.mm):
 * headless, video-only, frame-pull decoding. Playback direction and looping
 * are driven by the engine (MediaSourceEngines.cpp) via seek() — this reader
 * only decodes forward, so canPlayReverse() returns false and the engine does
 * reverse by stepping seeks backward.
 *
 * ⚠️ Compiles and links against the Windows SDK, but the decode path has NOT
 * been exercised on a real Windows machine yet — treat as needing functional
 * validation (open a real .mp4, confirm pullFrame yields correct RGB frames).
 *
 * Threading: same contract as the .mm — open()/close() on the message thread,
 * the rest from the MediaSourceService thread.
 */
#include "VideoFileReader.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <Mfobjects.h>
#include <atomic>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace
{
    // One MFStartup per process, released at shutdown. Media Foundation ref-
    // counts internally, so pairing Startup/Shutdown per reader also works, but
    // a single init avoids repeated COM apartment churn.
    struct MFRuntime
    {
        MFRuntime()  { ok = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE)); }
        ~MFRuntime() { if (ok) MFShutdown(); }
        bool ok = false;
    };
    MFRuntime& mfRuntime() { static MFRuntime r; return r; }

    template <class T> void safeRelease (T*& p) { if (p) { p->Release(); p = nullptr; } }
}

struct VideoFileReader::Impl
{
    IMFSourceReader* reader = nullptr;
    int   width = 0, height = 0;
    LONG  stride = 0;            // signed: negative = bottom-up
    double durationS = 0.0;
    double fps = 0.0;
    double positionS = 0.0;
    std::atomic<float> rate { 0.0f };
    bool  ready = false;
    bool  atEnd = false;

    ~Impl() { safeRelease (reader); }
};

VideoFileReader::VideoFileReader() : impl (std::make_unique<Impl>()) {}
VideoFileReader::~VideoFileReader() { close(); }

bool VideoFileReader::open (const juce::File& file, juce::String& error)
{
    close();

    if (! mfRuntime().ok) { error = "Media Foundation could not start."; return false; }
    if (! file.existsAsFile()) { error = "File not found: " + file.getFullPathName(); return false; }

    impl = std::make_unique<Impl>();

    // Ask the source reader to insert a video processor so we can force RGB32
    // output regardless of the file's native codec/format.
    IMFAttributes* attrs = nullptr;
    if (FAILED (MFCreateAttributes (&attrs, 1)))
    { error = "MFCreateAttributes failed."; return false; }
    attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    const juce::String url = file.getFullPathName();
    HRESULT hr = MFCreateSourceReaderFromURL (url.toWideCharPointer(), attrs, &impl->reader);
    safeRelease (attrs);
    if (FAILED (hr)) { error = "Cannot open video (MFCreateSourceReaderFromURL)."; return false; }

    // Select only the first video stream.
    impl->reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
    impl->reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Force RGB32 output.
    IMFMediaType* want = nullptr;
    MFCreateMediaType (&want);
    want->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = impl->reader->SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
    safeRelease (want);
    if (FAILED (hr)) { error = "Video format not convertible to RGB32."; safeRelease (impl->reader); return false; }

    // Read back the negotiated type for frame size / stride / fps.
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED (impl->reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
    {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize (cur, MF_MT_FRAME_SIZE, &w, &h);
        impl->width  = (int) w;
        impl->height = (int) h;

        UINT32 num = 0, den = 0;
        if (SUCCEEDED (MFGetAttributeRatio (cur, MF_MT_FRAME_RATE, &num, &den)) && den != 0)
            impl->fps = (double) num / (double) den;

        LONG s = 0;
        if (SUCCEEDED (cur->GetUINT32 (MF_MT_DEFAULT_STRIDE, (UINT32*) &s)))
            impl->stride = s;
        else
            impl->stride = (LONG) w * 4; // top-down fallback

        safeRelease (cur);
    }

    // Duration (100-ns units) from the presentation descriptor.
    PROPVARIANT var;
    PropVariantInit (&var);
    if (SUCCEEDED (impl->reader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE,
                                                           MF_PD_DURATION, &var)))
    {
        impl->durationS = (double) var.uhVal.QuadPart / 1.0e7;
    }
    PropVariantClear (&var);

    if (impl->width <= 0 || impl->height <= 0)
    { error = "Video has no readable frame size."; safeRelease (impl->reader); return false; }

    impl->ready = true;
    return true;
}

void VideoFileReader::close()
{
    if (impl) { safeRelease (impl->reader); impl->ready = false; }
}

bool   VideoFileReader::isOpen()  const { return impl && impl->reader != nullptr; }
bool   VideoFileReader::isReady() const { return impl && impl->ready; }
double VideoFileReader::getDurationS() const { return impl ? impl->durationS : 0.0; }
double VideoFileReader::getNominalFps() const { return impl ? impl->fps : 0.0; }
int    VideoFileReader::getWidth()  const { return impl ? impl->width : 0; }
int    VideoFileReader::getHeight() const { return impl ? impl->height : 0; }
bool   VideoFileReader::canPlayReverse() const { return false; } // engine steps seeks

void  VideoFileReader::setRate (float r) { if (impl) impl->rate.store (r); }
float VideoFileReader::getRate() const   { return impl ? impl->rate.load() : 0.0f; }

void VideoFileReader::seek (double seconds)
{
    if (! isOpen()) return;
    PROPVARIANT var;
    PropVariantInit (&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG) (juce::jmax (0.0, seconds) * 1.0e7); // → 100-ns
    if (SUCCEEDED (impl->reader->SetCurrentPosition (GUID_NULL, var)))
    {
        impl->positionS = seconds;
        impl->atEnd = false;
    }
    PropVariantClear (&var);
}

double VideoFileReader::getPositionS() const { return impl ? impl->positionS : 0.0; }

bool VideoFileReader::pullFrame (juce::Image& target)
{
    if (! isReady()) return false;

    // Paused: keep whatever frame the caller already holds.
    if (impl->rate.load() == 0.0f) return false;

    DWORD streamFlags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;

    HRESULT hr = impl->reader->ReadSample ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                           0, nullptr, &streamFlags, &ts, &sample);
    if (FAILED (hr)) { safeRelease (sample); return false; }

    if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) { impl->atEnd = true; safeRelease (sample); return false; }
    if (sample == nullptr) return false; // gap/no data this call

    impl->positionS = (double) ts / 1.0e7;

    IMFMediaBuffer* buffer = nullptr;
    if (FAILED (sample->ConvertToContiguousBuffer (&buffer))) { safeRelease (sample); return false; }

    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED (buffer->Lock (&data, &maxLen, &curLen)))
    { safeRelease (buffer); safeRelease (sample); return false; }

    const int w = impl->width, h = impl->height;
    if (target.getWidth() != w || target.getHeight() != h || ! target.isValid())
        target = juce::Image (juce::Image::ARGB, w, h, false);

    // MF RGB32 memory order is B,G,R,A — identical to JUCE ARGB on little-endian
    // Windows, so rows copy verbatim. A negative default stride means bottom-up.
    const bool bottomUp = impl->stride < 0;
    const int  srcStride = (int) std::abs (impl->stride ? impl->stride : (LONG) w * 4);

    juce::Image::BitmapData dst (target, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
    {
        const int srcRow = bottomUp ? (h - 1 - y) : y;
        const BYTE* s = data + (size_t) srcRow * (size_t) srcStride;
        std::memcpy (dst.getLinePointer (y), s, (size_t) juce::jmin (srcStride, w * 4));
    }

    buffer->Unlock();
    safeRelease (buffer);
    safeRelease (sample);
    return true;
}
