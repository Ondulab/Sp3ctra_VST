/**
 * @file VideoRecorder.h
 * @brief High-quality VIDEO MIX + master-audio recorder (macOS AVFoundation).
 *
 * Muxes the VIDEO MIX composite (pushed from the mixer's render thread) and the
 * master stereo audio (tapped in processBlock) into a single .mov — HEVC video
 * + AAC audio, A/V synchronised. There is one recorder, owned by the processor
 * so the RT audio thread can feed it and it outlives the editor.
 *
 * Threading contract:
 *   - start()/stop()          : message thread.
 *   - pushAudio()             : RT audio thread — lock-free write into a
 *                               preallocated FIFO, NO alloc / NO lock.
 *   - pushVideoFrame()        : the mixer's background render thread.
 *   - isRecording()/lastError(): any thread (atomic / lock-guarded).
 * A dedicated writer thread drains both streams and appends to the AVAssetWriter.
 *
 * PIMPL, platform-split like VideoFileReader: VideoRecorder.mm (macOS) or
 * VideoRecorderStub.cpp (elsewhere — recording unavailable).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <memory>

class VideoRecorder
{
public:
    VideoRecorder();
    ~VideoRecorder();

    /** Begin writing `out` (.mov). Video is locked to `w`×`h` (must be even);
     *  `fps` is a nominal hint (the stream is VFR, stamped by pushVideoFrame).
     *  Audio is `numAudioCh` channels at `sampleRate`. Returns false + fills
     *  `err` on failure (and leaves the recorder idle). Message thread. */
    bool start(const juce::File& out, int w, int h, double fps,
               double sampleRate, int numAudioCh, juce::String& err);

    /** Finish + flush the file and join the writer thread. Safe if idle.
     *  Message thread. */
    void stop();

    bool        isRecording() const noexcept;
    juce::String lastError()  const;

    /** Render thread: hand off the latest composite with its presentation time
     *  (seconds since recording started). Converts to a pixel buffer and
     *  enqueues; frames are dropped (counted) if the writer falls behind. */
    void pushVideoFrame(const juce::Image& composite, double tSeconds);

    /** RT audio thread: append `numSamples` of interleaveable float channels.
     *  Lock-free write into the preallocated FIFO; excess (writer stalled) is
     *  dropped. Never allocates or locks. */
    void pushAudio(const float* const* chans, int numCh, int numSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoRecorder)
};
