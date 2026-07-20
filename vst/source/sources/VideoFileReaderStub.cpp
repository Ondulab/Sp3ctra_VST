/**
 * @file VideoFileReaderStub.cpp
 * @brief Non-macOS stand-in for the AVFoundation frame reader: open() always
 *        fails with an explanatory error, so the VIDEO source module shows
 *        "could not open" instead of crashing. A real Windows implementation
 *        (Media Foundation IMFSourceReader) can replace this file without
 *        touching the header.
 */
#include "VideoFileReader.h"

struct VideoFileReader::Impl {};

VideoFileReader::VideoFileReader() = default;
VideoFileReader::~VideoFileReader() = default;

bool VideoFileReader::open(const juce::File& file, juce::String& error)
{
    juce::ignoreUnused(file);
    error = "The VIDEO file source is not available on this platform yet "
            "(macOS only). IMAGE and CAMERA sources work normally.";
    return false;
}

void VideoFileReader::close() {}

bool   VideoFileReader::isOpen()  const { return false; }
bool   VideoFileReader::isReady() const { return false; }
double VideoFileReader::getDurationS() const { return 0.0; }
double VideoFileReader::getNominalFps() const { return 0.0; }
int    VideoFileReader::getWidth()  const { return 0; }
int    VideoFileReader::getHeight() const { return 0; }
bool   VideoFileReader::canPlayReverse() const { return false; }

void  VideoFileReader::setRate(float) {}
float VideoFileReader::getRate() const { return 0.0f; }

void   VideoFileReader::seek(double) {}
double VideoFileReader::getPositionS() const { return 0.0; }

bool VideoFileReader::pullFrame(juce::Image&) { return false; }
