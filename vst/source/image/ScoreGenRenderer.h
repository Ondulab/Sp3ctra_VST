/**
 * @file ScoreGenRenderer.h
 * @brief Offline "graphical score" renderer — turns a WAV file into a printable
 *        greyscale spectrogram image (juce::Image), exportable to PNG/JPEG.
 *
 * Non-UI bridge between the ported C engine (score_engine.c) and JUCE:
 *   WAV → mono double[] (juce::AudioFormatReader)
 *       → pre-filters (high-pass, HF boost)
 *       → score_compute_spectrogram + score_apply_image_processing
 *       → physical scale / layout (port of spectral_raster.c)
 *       → juce::Image (one grey rect per time×bin cell, LINEAR frequency axis)
 *
 * Heavy (64K FFT × N windows): call renderScore() off the message thread
 * (see ScoreGenThread). All members are stateless free functions.
 */
#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_core/juce_core.h>
#include <functional>

extern "C" {
#include "processing/score_engine.h"
}

namespace scoregen
{

struct RenderResult
{
    juce::Image  image;          ///< the generated page (RGB), invalid on failure
    juce::String log;            ///< human-readable summary / error message
    bool         ok = false;
    int          pixelWidth  = 0;
    int          pixelHeight = 0;
    /** The spectrogram band inside `image` (x,y,w,h) — the region a CIS sensor
     *  would actually scan: X = drawn time columns, Y = [maxFreq..minFreq].
     *  Everything outside is white page margin. The playback reader extracts
     *  ONLY this band so the frequency axis matches the sensor span. */
    juce::Rectangle<int> spectroBand;
};

/** Loaded-file probe shown in the UI before generation. */
struct WavInfo
{
    bool         ok = false;
    double       durationSec = 0.0;
    int          sampleRate  = 0;
    int          numChannels = 0;
    juce::int64  lengthSamples = 0;
    juce::String error;
};

/** Reads basic metadata of a WAV/AIFF/FLAC file (cheap, no full decode). */
WavInfo probeWav(const juce::File& file);

/** Seconds of audio that fill one page at the given writing speed + page format
 *  (DPI cancels out). Returns 0 when writingSpeed<=0 (⇒ whole file). This is the
 *  width of the export region the user positions over the waveform. */
double pageWindowSeconds(const ScoreSettings& settings);

/** Full pipeline: load → filter → STFT → image. `progress` (0..1, optional) is
 *  called from the calling thread; `shouldAbort` lets a worker cancel early.
 *  Never throws; returns ok=false with a message on any failure. */
RenderResult renderScore(const juce::File& wav,
                         const ScoreSettings& settings,
                         std::function<void(float)> progress = {},
                         std::function<bool()>      shouldAbort = {});

/** Writes `img` to `dest` as PNG (asPng=true) or JPEG (quality 0.9). */
bool exportImage(const juce::Image& img, const juce::File& dest, bool asPng);

} // namespace scoregen
