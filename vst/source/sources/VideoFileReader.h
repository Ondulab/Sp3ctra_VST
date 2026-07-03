/**
 * @file VideoFileReader.h
 * @brief M9 — Frame-access video file reader (macOS AVFoundation).
 *
 * Thin wrapper around AVPlayer + AVPlayerItemVideoOutput giving pixel access
 * to the currently-presented frame — something juce::VideoComponent does not
 * expose. Playback is headless (video only, audio muted); the engine polls
 * pullFrame() and drives looping/direction itself.
 *
 * Threading: open()/close() on the message thread; setRate/seek/getPosition/
 * pullFrame from the MediaSourceService thread (AVFoundation is thread-safe
 * for these calls).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <memory>

class VideoFileReader
{
public:
    VideoFileReader();
    ~VideoFileReader();

    /** Open a video file. Loading is asynchronous inside AVFoundation:
     *  isReady() flips true once the item can present frames. Returns false
     *  only on immediate errors (missing file, no video track). */
    bool open(const juce::File& file, juce::String& error);
    void close();

    bool   isOpen()  const;
    bool   isReady() const;              ///< item ready + video output attached
    double getDurationS() const;         ///< 0 until known
    double getNominalFps() const;        ///< 0 until known
    int    getWidth()  const;
    int    getHeight() const;
    bool   canPlayReverse() const;

    /** Signed playback rate: 1 = forward, -1 = backward, 0 = paused. */
    void  setRate(float rate);
    float getRate() const;

    void   seek(double seconds);
    double getPositionS() const;

    /** Copy the most recent presented frame into `target` (recreated as ARGB
     *  when the size changes). Returns true when a NEW frame was copied. */
    bool pullFrame(juce::Image& target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoFileReader)
};
