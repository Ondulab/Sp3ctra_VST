#include "ScoreGenRenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <vector>

namespace scoregen
{

//==============================================================================
WavInfo probeWav(const juce::File& file)
{
    WavInfo info;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr)
    {
        info.error = "Unsupported or unreadable audio file";
        return info;
    }
    info.ok            = true;
    info.sampleRate    = (int) reader->sampleRate;
    info.numChannels   = (int) reader->numChannels;
    info.lengthSamples = reader->lengthInSamples;
    info.durationSec   = (reader->sampleRate > 0.0)
                            ? (double) reader->lengthInSamples / reader->sampleRate
                            : 0.0;
    return info;
}

//==============================================================================
// Physical-scale helpers (port of spectral_raster.c get_* utilities).
namespace
{
    inline double mmToPixels(double dpi)  { return dpi / 25.4; }
    inline double pixelsToCm(double dpi)  { return 2.54 / dpi; }

    inline double pageWidthPx(int format, double dpi)
    {
        return (format == 1 ? SCORE_A3_WIDTH_MM : SCORE_A4_WIDTH_MM) * mmToPixels(dpi);
    }
    inline double pageHeightPx(int format, double dpi)
    {
        return (format == 1 ? SCORE_A3_HEIGHT_MM : SCORE_A4_HEIGHT_MM) * mmToPixels(dpi);
    }

    inline double overlapValue(int preset)
    {
        switch (preset)
        {
            case 0:  return SCORE_OVERLAP_LOW;
            case 2:  return SCORE_OVERLAP_HIGH;
            default: return SCORE_OVERLAP_MEDIUM;
        }
    }

    // Mono mix-down of a (possibly multichannel) reader into double[].
    bool loadMono(juce::AudioFormatReader& reader, juce::int64 framesToLoad,
                  std::vector<double>& out, bool normalize)
    {
        const int ch = juce::jmax(1, (int) reader.numChannels);
        const int n  = (int) juce::jmin(framesToLoad, reader.lengthInSamples);
        if (n <= 0)
            return false;

        juce::AudioBuffer<float> buf(ch, n);
        if (! reader.read(&buf, 0, n, 0, true, true))
            return false;

        out.resize((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;
            for (int c = 0; c < ch; ++c)
                s += buf.getSample(c, i);
            out[(size_t) i] = s / ch;
        }

        if (normalize)
        {
            double maxAbs = 0.0;
            for (double v : out) maxAbs = juce::jmax(maxAbs, std::abs(v));
            if (maxAbs > 0.0)
                for (double& v : out) v /= maxAbs;
        }
        return true;
    }
}

//==============================================================================
RenderResult renderScore(const juce::File& wav,
                         const ScoreSettings& settingsIn,
                         std::function<void(float)> progress,
                         std::function<bool()>      shouldAbort)
{
    RenderResult result;
    juce::StringArray logLines;
    auto fail = [&](const juce::String& msg) -> RenderResult
    {
        result.ok = false;
        result.log = msg;
        return result;
    };

    ScoreSettings s = settingsIn;

    // ── Open the file ────────────────────────────────────────────────────────
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
    if (reader == nullptr)
        return fail("Cannot read audio file: " + wav.getFileName());

    const int sampleRate = (int) reader->sampleRate;
    if (sampleRate <= 0)
        return fail("Invalid sample rate");

    const double dpi = (s.printerDpi >= 72.0) ? s.printerDpi : SCORE_DEFAULT_PRINTER_DPI;

    // ── Bins/s from writing speed (port of spectral_raster.c:350-380) ────────
    double binsPerSecond;
    if (s.writingSpeed > 0.0)
    {
        double bps = std::floor((dpi / 2.54) * s.writingSpeed);
        bps = juce::jlimit(SCORE_MIN_BINS_PER_SECOND, SCORE_MAX_BINS_PER_SECOND, bps);
        binsPerSecond = bps;
    }
    else
    {
        binsPerSecond = (s.binsPerSecond > 0.0) ? s.binsPerSecond
                                                : SCORE_DEFAULT_BINS_PER_SECOND;
    }

    // ── FFT window size (port of spectral_raster.c:401-426) ──────────────────
    int fftSize;
    if (s.fftSize > 0)
    {
        fftSize = s.fftSize;
    }
    else
    {
        const double hop  = (double) sampleRate / binsPerSecond;
        const double calc = hop / (1.0 - overlapValue(s.overlapPreset));
        fftSize = 1;
        while ((double) fftSize < calc) fftSize *= 2;
    }
    if (fftSize > SCORE_FFT_EFFECTIVE_SIZE)
        fftSize = SCORE_FFT_EFFECTIVE_SIZE;

    // ── Duration to load (port of spectral_raster.c:436-454) ─────────────────
    double duration = 0.0;     // 0 ⇒ whole file
    if (s.writingSpeed > 0.0)
    {
        const double pageWcm = pageWidthPx(s.pageFormat, dpi) / (dpi / 2.54);
        duration = pageWcm / s.writingSpeed;
    }

    juce::int64 framesToLoad = reader->lengthInSamples;
    if (duration > 0.0)
        framesToLoad = juce::jmin(framesToLoad,
                                  (juce::int64) (duration * sampleRate));

    // ── Load + mono mix-down ─────────────────────────────────────────────────
    std::vector<double> signal;
    if (! loadMono(*reader, framesToLoad, signal, s.enableNormalization != 0))
        return fail("Failed to decode audio samples");

    const int totalSamples = (int) signal.size();
    if (totalSamples < fftSize)
        return fail("Audio too short for FFT window (" + juce::String(totalSamples)
                    + " < " + juce::String(fftSize) + " samples)");

    if (progress) progress(0.05f);
    if (shouldAbort && shouldAbort()) return fail("Aborted");

    // ── Pre-filters (legacy order: high-pass then HF boost) ──────────────────
    if (s.enableHighPassFilter && s.highPassCutoffFreq > 0.0)
        score_apply_highpass(signal.data(), totalSamples, sampleRate,
                             s.highPassCutoffFreq, s.highPassFilterOrder);
    if (s.enableHighBoost)
        score_apply_high_freq_boost(signal.data(), totalSamples, s.highBoostAlpha);

    if (progress) progress(0.10f);
    if (shouldAbort && shouldAbort()) return fail("Aborted");

    // ── STFT ─────────────────────────────────────────────────────────────────
    ScoreSpectrogramData spec = {};
    const int rc = score_compute_spectrogram(signal.data(), totalSamples, sampleRate,
                                             fftSize, binsPerSecond,
                                             s.minFreq, s.maxFreq, &spec);
    if (rc != 0)
        return fail("Spectrogram computation failed (code " + juce::String(rc) + ")");

    if (progress) progress(0.55f);
    if (shouldAbort && shouldAbort()) { score_free_spectrogram(&spec); return fail("Aborted"); }

    score_apply_image_processing(&spec, s.dynamicRangeDB, s.gammaCorrection,
                                 s.enableDithering, s.contrastFactor,
                                 s.enableNoiseGate, s.noiseGateThreshold);

    if (progress) progress(0.60f);

    // ── Layout (port of spectral_raster.c:556-707) ───────────────────────────
    const double pageW = pageWidthPx(s.pageFormat, dpi);
    const double pageH = pageHeightPx(s.pageFormat, dpi);
    const int imageW = (int) pageW;
    const int imageH = (int) pageH;

    const double labelMargin    = 150.0 * (dpi / 400.0);
    const double mmPx           = mmToPixels(dpi);
    const double bottomMarginPx = s.bottomMarginMM  * mmPx;
    const double spectroHeightPx= s.spectroHeightMM * mmPx;

    const double spectroLeft   = labelMargin;
    const double spectroWidth  = pageW - labelMargin;
    const double spectroBottom = pageH - bottomMarginPx;
    const double spectroTop    = spectroBottom - spectroHeightPx;

    const double freqRange      = s.maxFreq - s.minFreq;
    const double freqResolution = (double) sampleRate / (double) SCORE_FFT_EFFECTIVE_SIZE;

    // Visible windows: clip to page width at the requested writing speed.
    int visibleWindows = spec.num_windows;
    if (s.writingSpeed > 0.0)
    {
        const double realDur = (double) totalSamples / (double) sampleRate;
        const double fftDur  = (duration > 0.0) ? duration : realDur;
        const double spectroWidthCm = spectroWidth / (dpi / 2.54);
        const double requiredCm     = fftDur * s.writingSpeed;
        if (requiredCm > spectroWidthCm && requiredCm > 0.0)
        {
            const double scale = spectroWidthCm / requiredCm;
            visibleWindows = juce::jmax(1, (int) (spec.num_windows * scale));
        }
    }

    // Per-window pixel width (port of spectral_raster.c:700-703).
    double windowWidth;
    if (s.writingSpeed > 0.0)
    {
        const double secondsPerWindow = 1.0 / binsPerSecond;
        const double cmPerWindow      = secondsPerWindow * s.writingSpeed;
        windowWidth = cmPerWindow / pixelsToCm(dpi);
    }
    else
    {
        windowWidth = (visibleWindows > 0) ? spectroWidth / visibleWindows : 1.0;
    }
    if (windowWidth <= 0.0) windowWidth = 1.0;

    // ── Render: white page + per-output-pixel sampling of the spectro band ───
    juce::Image img(juce::Image::RGB, imageW, imageH, true);
    {
        juce::Graphics g(img);
        g.fillAll(juce::Colours::white);
    }

    const int numBins = spec.num_bins;
    const double* data = spec.data;

    const double drawnW = visibleWindows * windowWidth;
    int xStart = (int) std::floor(spectroLeft);
    int xEnd   = (int) std::ceil (spectroLeft + drawnW);
    int yTop   = (int) std::floor(spectroTop);
    int yBot   = (int) std::ceil (spectroBottom);
    xStart = juce::jmax(0, xStart);
    xEnd   = juce::jmin(imageW, xEnd);
    yTop   = juce::jmax(0, yTop);
    yBot   = juce::jmin(imageH, yBot);

    {
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
        const int span = juce::jmax(1, xEnd - xStart);

        for (int x = xStart; x < xEnd; ++x)
        {
            int w = (int) ((x - spectroLeft) / windowWidth);
            w = juce::jlimit(0, visibleWindows - 1, w);
            const double* col = data + (size_t) w * numBins;

            for (int y = yTop; y < yBot; ++y)
            {
                // Linear frequency mapping (USE_LOG_FREQUENCY = 0). The LuxSampler
                // score loader remaps this linear axis onto the synth's log
                // oscillator bank, so pitch is preserved.
                const double lin = (spectroBottom - (y + 0.5)) / spectroHeightPx;
                if (lin < 0.0 || lin > 1.0)
                    continue;
                const double binFreq = s.minFreq + lin * freqRange;
                const int b = (int) (binFreq / freqResolution);
                if (b < spec.index_min || b > spec.index_max || b >= numBins)
                    continue;

                const double intensity = col[b];
                const auto v = (juce::uint8) juce::jlimit(0, 255, (int) (intensity * 255.0 + 0.5));
                bmp.setPixelColour(x, y, juce::Colour(v, v, v));
            }

            if (progress && ((x - xStart) % 64 == 0))
                progress(0.60f + 0.38f * (float) (x - xStart) / (float) span);
            if (shouldAbort && shouldAbort())
            {
                score_free_spectrogram(&spec);
                return fail("Aborted");
            }
        }
    }

    logLines.add("Page: " + juce::String(imageW) + " x " + juce::String(imageH)
                 + " px @ " + juce::String(dpi, 0) + " DPI");
    logLines.add("Sample rate: " + juce::String(sampleRate) + " Hz");
    logLines.add("FFT window: " + juce::String(fftSize)
                 + " (pad " + juce::String(SCORE_FFT_EFFECTIVE_SIZE) + ")");
    logLines.add("Bins/s: " + juce::String(binsPerSecond, 1)
                 + "  windows: " + juce::String(spec.num_windows)
                 + " (visible " + juce::String(visibleWindows) + ")");
    logLines.add("Freq band: " + juce::String(s.minFreq, 0) + "-"
                 + juce::String(s.maxFreq, 0) + " Hz (bins "
                 + juce::String(spec.index_min) + "-" + juce::String(spec.index_max) + ")");

    score_free_spectrogram(&spec);

    if (progress) progress(1.0f);

    result.image       = img;
    result.ok          = true;
    result.pixelWidth  = imageW;
    result.pixelHeight = imageH;
    result.spectroBand = juce::Rectangle<int>(xStart, yTop,
                                              juce::jmax(1, xEnd - xStart),
                                              juce::jmax(1, yBot - yTop));
    result.log         = logLines.joinIntoString("\n");
    return result;
}

//==============================================================================
bool exportImage(const juce::Image& img, const juce::File& dest, bool asPng)
{
    if (! img.isValid())
        return false;

    dest.deleteFile();
    juce::FileOutputStream out(dest);
    if (out.failedToOpen())
        return false;

    bool ok;
    if (asPng)
    {
        juce::PNGImageFormat fmt;
        ok = fmt.writeImageToStream(img, out);
    }
    else
    {
        juce::JPEGImageFormat fmt;
        fmt.setQuality(0.9f);
        ok = fmt.writeImageToStream(img, out);
    }

    if (! ok)
    {
        out.flush();
        dest.deleteFile();
        return false;
    }
    return true;
}

} // namespace scoregen
