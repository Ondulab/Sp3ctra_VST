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
    bool         stereo = false; ///< true when the image is a colour L/R composite
                                 ///< (false ⇒ greyscale; e.g. stereo asked on a mono file)
    int          pixelWidth  = 0;
    int          pixelHeight = 0;
    /** The spectrogram band inside `image` (x,y,w,h) — the region a CIS sensor
     *  would actually scan: X = drawn time columns, Y = [maxFreq..minFreq].
     *  Everything outside is white page margin. The playback reader extracts
     *  ONLY this band so the frequency axis matches the sensor span. */
    juce::Rectangle<int> spectroBand;
};

/** Calibration a Sp3ctra spectrogram carries so the SAMPLER (or any reader) can
 *  reload the EXACT frequency / band mapping the generator played, instead of
 *  the calibration-blind row/column scan. Embedded on export in a PNG
 *  "Sp3ctraCal" tEXt chunk; read back by readCalibration(). */
struct SpectroCalibration
{
    juce::Rectangle<int> band;      ///< spectro band inside the image (x,y,w,h)
    double minHz  = 0.0;            ///< band bottom frequency (low edge)
    double maxHz  = 0.0;            ///< band top frequency (high edge)
    bool   stereo = false;          ///< colour L/R composite (see SCORE stereo)
    bool   valid  = false;          ///< false ⇒ no Sp3ctra calibration present
};

/** Reads the calibration embedded by exportImage() from a PNG file. Returns
 *  {valid=false} for non-Sp3ctra images (arbitrary user PNGs) or non-PNG. */
SpectroCalibration readCalibration(const juce::File& pngFile);

/** Same, from an in-memory PNG (e.g. a persisted session take). */
SpectroCalibration readCalibration(const void* pngData, size_t numBytes);

/** PNG-encodes @p img with the "Sp3ctraCal" chunk embedded — the in-memory
 *  counterpart of exportImage(asPng=true, cal). The bytes round-trip through
 *  readCalibration() + ScorePlayerService::buildFramesFromImage back to the
 *  exact frames the generator loaded. Empty block on invalid image/encoder
 *  failure or a non-valid calibration (nothing to rebuild from). */
juce::MemoryBlock encodeCalibratedPng(const juce::Image& img,
                                      const SpectroCalibration& cal);

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

/** Writes `img` to `dest` as PNG (asPng=true) or JPEG (quality 0.9), embedding
 *  the physical resolution `dpi` so a print at "100%" reproduces the true size
 *  (PNG pHYs chunk / JPEG JFIF density). Without this tag, viewers assume 72/96
 *  DPI and a 400-DPI page prints ~5× too large. `dpi <= 0` ⇒ no tag written.
 *  When `cal != nullptr && cal->valid` and asPng, also embeds a "Sp3ctraCal"
 *  tEXt chunk so the sampler can reload the image with the exact score mapping
 *  (see readCalibration / LuxSampler::loadSlotFromImageFile). */
bool exportImage(const juce::Image& img, const juce::File& dest, bool asPng, double dpi,
                 const SpectroCalibration* cal = nullptr);

} // namespace scoregen
