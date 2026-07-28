/**
 * @file VideoRecorderStub.cpp
 * @brief Graceful no-op VideoRecorder for platforms without a backend.
 *
 * Recording is macOS-only for now (AVFoundation, VideoRecorder.mm). A Windows
 * Media Foundation backend (IMFSinkWriter) can replace this file following the
 * same PIMPL split used by VideoFileReader / VideoFileReaderMF.
 */
#include "VideoRecorder.h"

struct VideoRecorder::Impl {};

VideoRecorder::VideoRecorder() : impl(std::make_unique<Impl>()) {}
VideoRecorder::~VideoRecorder() = default;

bool VideoRecorder::start(const juce::File&, int, int, double, double, int,
                          juce::String& err)
{
    err = "Video recording is only available on macOS.";
    return false;
}

void         VideoRecorder::stop()                      {}
bool         VideoRecorder::isRecording() const noexcept { return false; }
juce::String VideoRecorder::lastError()   const          { return {}; }
void         VideoRecorder::pushVideoFrame(const juce::Image&, double) {}
void         VideoRecorder::pushAudio(const float* const*, int, int)   {}
