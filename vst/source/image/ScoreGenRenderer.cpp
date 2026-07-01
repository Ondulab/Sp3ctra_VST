#include "ScoreGenRenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstring>
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

    // Mono mix-down of a (possibly multichannel) reader into double[], starting
    // at `startSample` in the file.
    bool loadMono(juce::AudioFormatReader& reader, juce::int64 startSample,
                  juce::int64 framesToLoad, std::vector<double>& out, bool normalize)
    {
        const int ch = juce::jmax(1, (int) reader.numChannels);
        startSample = juce::jlimit((juce::int64) 0, reader.lengthInSamples, startSample);
        const int n  = (int) juce::jmin(framesToLoad, reader.lengthInSamples - startSample);
        if (n <= 0)
            return false;

        juce::AudioBuffer<float> buf(ch, n);
        if (! reader.read(&buf, 0, n, startSample, true, true))
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

    // Stereo load: channel 0 → outL, channel 1 → outR (a mono file duplicates ch0
    // into both). When normalising we scale BOTH channels by their COMMON peak so
    // the inter-channel level difference — the stereo image — is preserved.
    bool loadStereo(juce::AudioFormatReader& reader, juce::int64 startSample,
                    juce::int64 framesToLoad, std::vector<double>& outL,
                    std::vector<double>& outR, bool normalize)
    {
        const int ch = juce::jmax(1, (int) reader.numChannels);
        startSample = juce::jlimit((juce::int64) 0, reader.lengthInSamples, startSample);
        const int n  = (int) juce::jmin(framesToLoad, reader.lengthInSamples - startSample);
        if (n <= 0)
            return false;

        juce::AudioBuffer<float> buf(ch, n);
        if (! reader.read(&buf, 0, n, startSample, true, true))
            return false;

        outL.resize((size_t) n);
        outR.resize((size_t) n);
        const int rch = (ch >= 2) ? 1 : 0;   // mono file ⇒ right = left
        for (int i = 0; i < n; ++i)
        {
            outL[(size_t) i] = buf.getSample(0,   i);
            outR[(size_t) i] = buf.getSample(rch, i);
        }

        if (normalize)
        {
            double maxAbs = 0.0;
            for (double v : outL) maxAbs = juce::jmax(maxAbs, std::abs(v));
            for (double v : outR) maxAbs = juce::jmax(maxAbs, std::abs(v));
            if (maxAbs > 0.0)
            {
                for (double& v : outL) v /= maxAbs;
                for (double& v : outR) v /= maxAbs;
            }
        }
        return true;
    }
}

//==============================================================================
double pageWindowSeconds(const ScoreSettings& s)
{
    if (s.writingSpeed <= 0.0)
        return 0.0;   // whole file
    const double widthMM = (s.pageFormat == 1) ? SCORE_A3_WIDTH_MM : SCORE_A4_WIDTH_MM;
    return (widthMM / 10.0) / s.writingSpeed;   // mm→cm, then cm / (cm/s)
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

    // ── Region to extract: one page-width window starting at startTimeSec ─────
    const double duration = pageWindowSeconds(s);   // 0 ⇒ whole file
    const juce::int64 startSample = juce::jlimit(
        (juce::int64) 0, reader->lengthInSamples,
        (juce::int64) (s.startTimeSec * sampleRate));

    juce::int64 framesToLoad = reader->lengthInSamples - startSample;
    if (duration > 0.0)
        framesToLoad = juce::jmin(framesToLoad,
                                  (juce::int64) (duration * sampleRate));

    // ── Load signal(s) ───────────────────────────────────────────────────────
    // Stereo SCORE builds TWO spectrograms (left → red, right → blue) so the
    // synth's existing colour-temperature panning reproduces the stereo image.
    // A mono file (or <2 channels) falls back to the regular mono path.
    const bool stereo = (s.enableStereoMode != 0) && (reader->numChannels >= 2);

    std::vector<double> signal, signalR;
    if (stereo)
    {
        if (! loadStereo(*reader, startSample, framesToLoad, signal, signalR,
                         s.enableNormalization != 0))
            return fail("Failed to decode audio samples");
    }
    else
    {
        if (! loadMono(*reader, startSample, framesToLoad, signal, s.enableNormalization != 0))
            return fail("Failed to decode audio samples");
    }

    const int totalSamples = (int) signal.size();
    if (totalSamples < fftSize)
        return fail("Audio too short for FFT window (" + juce::String(totalSamples)
                    + " < " + juce::String(fftSize) + " samples)");

    if (progress) progress(0.05f);
    if (shouldAbort && shouldAbort()) return fail("Aborted");

    // ── Pre-filters (legacy order: high-pass then HF boost), per channel ─────
    auto applyFilters = [&](std::vector<double>& sig)
    {
        const int n = (int) sig.size();
        if (s.enableHighPassFilter && s.highPassCutoffFreq > 0.0)
            score_apply_highpass(sig.data(), n, sampleRate,
                                 s.highPassCutoffFreq, s.highPassFilterOrder);
        if (s.enableHighBoost)
            score_apply_high_freq_boost(sig.data(), n, s.highBoostAlpha);
    };
    applyFilters(signal);
    if (stereo) applyFilters(signalR);

    if (progress) progress(0.10f);
    if (shouldAbort && shouldAbort()) return fail("Aborted");

    // ── STFT (left/primary, plus right when stereo) ──────────────────────────
    ScoreSpectrogramData spec  = {};
    ScoreSpectrogramData specR = {};
    int rc = score_compute_spectrogram(signal.data(), totalSamples, sampleRate,
                                       fftSize, binsPerSecond,
                                       s.minFreq, s.maxFreq, &spec);
    if (rc != 0)
        return fail("Spectrogram computation failed (code " + juce::String(rc) + ")");

    if (progress) progress(stereo ? 0.35f : 0.55f);
    if (shouldAbort && shouldAbort()) { score_free_spectrogram(&spec); return fail("Aborted"); }

    if (stereo)
    {
        rc = score_compute_spectrogram(signalR.data(), totalSamples, sampleRate,
                                       fftSize, binsPerSecond,
                                       s.minFreq, s.maxFreq, &specR);
        if (rc != 0)
        {
            score_free_spectrogram(&spec);
            return fail("Spectrogram computation failed (code " + juce::String(rc) + ")");
        }

        // Share ONE magnitude reference across both channels so the dB/intensity
        // mapping preserves the L-vs-R level difference (a quieter channel must
        // stay quieter; independent self-normalisation would flatten the image
        // toward centre).
        const double sharedMax = juce::jmax(spec.global_max, specR.global_max);
        spec.global_max  = sharedMax;
        specR.global_max = sharedMax;

        if (progress) progress(0.55f);
        if (shouldAbort && shouldAbort())
        {
            score_free_spectrogram(&spec);
            score_free_spectrogram(&specR);
            return fail("Aborted");
        }
    }

    score_apply_image_processing(&spec, s.dynamicRangeDB, s.gammaCorrection,
                                 s.enableDithering, s.contrastFactor,
                                 s.enableNoiseGate, s.noiseGateThreshold);
    if (stereo)
        score_apply_image_processing(&specR, s.dynamicRangeDB, s.gammaCorrection,
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
    const double* data  = spec.data;
    const double* dataR = stereo ? specR.data : spec.data;   // mono ⇒ same buffer

    const double drawnW = visibleWindows * windowWidth;
    int xStart = (int) std::floor(spectroLeft);
    int xEnd   = (int) std::ceil (spectroLeft + drawnW);
    int yTop   = (int) std::floor(spectroTop);
    int yBot   = (int) std::ceil (spectroBottom);
    xStart = juce::jmax(0, xStart);
    xEnd   = juce::jmin(imageW, xEnd);
    yTop   = juce::jmax(0, yTop);
    yBot   = juce::jmin(imageH, yBot);

    // ── Per-output-row FFT bin cell [lo,hi] (LOG frequency axis) ─────────────
    // Matches PhonoPaper and the Sp3ctra reader (image row → oscillator on a
    // LOG-distributed bank, equal vertical space per octave). Each output pixel
    // covers a *band* of frequencies; at high frequency that band spans many FFT
    // bins (~27 Hz/px @ 16 kHz vs ~11 Hz FFT resolution), so the old nearest-bin
    // sampling read a single bin and MISSED peaks that fell between rows → faint
    // / lost highs (visible on the calibration sweep & top octaves). We instead
    // take the LOUDEST bin in each cell (peak-hold). data[] holds intensity where
    // 0 = black = loud, so "loudest" = MIN intensity. The mapping is x-independent
    // so it is computed once here. At low frequency a cell is sub-bin → one bin,
    // identical to before.
    const bool   logMap    = (s.minFreq > 0.0 && s.maxFreq > s.minFreq);
    const double freqRatio = logMap ? (s.maxFreq / s.minFreq) : 1.0;
    const int    rows      = (yBot > yTop) ? (yBot - yTop) : 0;
    std::vector<int> rowBinLo((size_t) rows, -1);
    std::vector<int> rowBinHi((size_t) rows, -1);
    for (int y = yTop; y < yBot; ++y)
    {
        double posLo = (spectroBottom - (y + 1)) / spectroHeightPx; // bottom edge (lower freq)
        double posHi = (spectroBottom -  y)      / spectroHeightPx; // top edge    (higher freq)
        if (posHi < 0.0 || posLo > 1.0)
            continue;                                                // pixel outside the band
        posLo = juce::jlimit(0.0, 1.0, posLo);
        posHi = juce::jlimit(0.0, 1.0, posHi);
        const double fLo = logMap ? s.minFreq * std::pow(freqRatio, posLo)
                                  : s.minFreq + posLo * freqRange;
        const double fHi = logMap ? s.minFreq * std::pow(freqRatio, posHi)
                                  : s.minFreq + posHi * freqRange;
        int bLo = (int) std::floor(fLo / freqResolution);
        int bHi = (int) std::ceil (fHi / freqResolution);
        bLo = juce::jmax(bLo, spec.index_min);
        bHi = juce::jmin(juce::jmin(bHi, spec.index_max), numBins - 1);
        if (bLo > bHi)
            continue;
        rowBinLo[(size_t) (y - yTop)] = bLo;
        rowBinHi[(size_t) (y - yTop)] = bHi;
    }

    {
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
        const int span = juce::jmax(1, xEnd - xStart);

        for (int x = xStart; x < xEnd; ++x)
        {
            int w = (int) ((x - spectroLeft) / windowWidth);
            w = juce::jlimit(0, visibleWindows - 1, w);
            const double* col  = data  + (size_t) w * numBins;
            const double* colR = dataR + (size_t) w * numBins;

            for (int y = yTop; y < yBot; ++y)
            {
                const int bLo = rowBinLo[(size_t) (y - yTop)];
                if (bLo < 0)
                    continue;                                  // row outside the band
                const int bHi = rowBinHi[(size_t) (y - yTop)];

                // Peak-hold over the cell: darkest (loudest) bin = MIN intensity
                // (0 = black = energy, 1 = white = silence).
                double leftVal = col[bLo];
                for (int b = bLo + 1; b <= bHi; ++b)
                    if (col[b] < leftVal) leftVal = col[b];

                double rightVal = leftVal;
                if (stereo)
                {
                    rightVal = colR[bLo];
                    for (int b = bLo + 1; b <= bHi; ++b)
                        if (colR[b] < rightVal) rightVal = colR[b];
                }

                // Composite (inverted convention): R = right energy, B = left
                // energy, G = min ⇒ left-only = red, right-only = blue, both =
                // black, silence = white. Mono (rightVal == leftVal) ⇒ R=G=B grey,
                // byte-identical to the legacy greyscale output.
                const auto rr = (juce::uint8) juce::jlimit(0, 255, (int) (rightVal * 255.0 + 0.5));
                const auto bb = (juce::uint8) juce::jlimit(0, 255, (int) (leftVal  * 255.0 + 0.5));
                const auto gg = (juce::uint8) juce::jlimit(0, 255,
                                    (int) (juce::jmin(leftVal, rightVal) * 255.0 + 0.5));
                bmp.setPixelColour(x, y, juce::Colour(rr, gg, bb));
            }

            if (progress && ((x - xStart) % 64 == 0))
                progress(0.60f + 0.38f * (float) (x - xStart) / (float) span);
            if (shouldAbort && shouldAbort())
            {
                score_free_spectrogram(&spec);
                if (stereo) score_free_spectrogram(&specR);
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
    if (stereo)
        logLines.add("Stereo: 2 spectrograms (left=red, right=blue)");
    else if (s.enableStereoMode != 0)
        logLines.add("Stereo ON but source is mono (" + juce::String(reader->numChannels)
                     + " ch) → greyscale. Load a stereo WAV for L/R.");

    score_free_spectrogram(&spec);
    if (stereo) score_free_spectrogram(&specR);

    if (progress) progress(1.0f);

    result.image       = img;
    result.ok          = true;
    result.stereo      = stereo;
    result.pixelWidth  = imageW;
    result.pixelHeight = imageH;
    result.spectroBand = juce::Rectangle<int>(xStart, yTop,
                                              juce::jmax(1, xEnd - xStart),
                                              juce::jmax(1, yBot - yTop));
    result.log         = logLines.joinIntoString("\n");
    return result;
}

//==============================================================================
namespace
{
    // PNG CRC-32 (ITU-T V.42, polynomial 0xEDB88320) over the chunk's type+data.
    juce::uint32 pngCrc32(const juce::uint8* data, size_t len)
    {
        juce::uint32 c = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i)
        {
            c ^= data[i];
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        return c ^ 0xFFFFFFFFu;
    }

    void appendBE32(juce::MemoryBlock& mb, juce::uint32 v)
    {
        const juce::uint8 b[4] = { (juce::uint8) (v >> 24), (juce::uint8) (v >> 16),
                                   (juce::uint8) (v >> 8),  (juce::uint8)  v };
        mb.append(b, 4);
    }

    // Insert a pHYs chunk (pixels-per-metre, both axes) right after IHDR so the
    // PNG carries a physical resolution. Returns the original bytes unchanged if
    // the stream isn't a PNG with a leading IHDR (defensive — never corrupts).
    juce::MemoryBlock pngWithDpi(const juce::MemoryBlock& src, double dpi)
    {
        const auto*  p = static_cast<const juce::uint8*> (src.getData());
        const size_t n = src.getSize();
        const size_t headEnd = 8 + 25;   // 8-byte signature + 25-byte IHDR chunk
        const juce::uint8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (n < headEnd || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0)
            return src;

        const juce::uint32 ppm = (juce::uint32) juce::roundToInt(dpi / 0.0254); // px/metre
        juce::uint8 typeData[4 + 9];
        memcpy(typeData, "pHYs", 4);
        for (int axis = 0; axis < 2; ++axis)   // ppuX then ppuY, both = ppm
        {
            typeData[4 + axis * 4 + 0] = (juce::uint8) (ppm >> 24);
            typeData[4 + axis * 4 + 1] = (juce::uint8) (ppm >> 16);
            typeData[4 + axis * 4 + 2] = (juce::uint8) (ppm >> 8);
            typeData[4 + axis * 4 + 3] = (juce::uint8)  ppm;
        }
        typeData[12] = 1;   // unit specifier: 1 = metre

        juce::MemoryBlock out;
        out.append(p, headEnd);                         // signature + IHDR
        appendBE32(out, 9);                             // pHYs data length
        out.append(typeData, sizeof(typeData));         // "pHYs" + 9 data bytes
        appendBE32(out, pngCrc32(typeData, sizeof(typeData)));
        out.append(p + headEnd, n - headEnd);           // IDAT … IEND
        return out;
    }

    // Patch the JFIF APP0 density fields in place (units = dpi, X = Y = dpi).
    // libjpeg writes a JFIF header with units = 0 (aspect ratio only); we rewrite
    // it so viewers know the true resolution. No-op if no JFIF marker is found.
    void jpegSetDpi(juce::MemoryBlock& mb, double dpi)
    {
        auto*        p = static_cast<juce::uint8*> (mb.getData());
        const size_t n = mb.getSize();
        for (size_t i = 0; i + 12 <= n && i < 64; ++i)
        {
            if (p[i] == 'J' && p[i+1] == 'F' && p[i+2] == 'I' && p[i+3] == 'F' && p[i+4] == 0)
            {
                const juce::uint16 d = (juce::uint16) juce::jlimit(1, 65535, juce::roundToInt(dpi));
                p[i + 7] = 1;                          // density units: 1 = dots/inch
                p[i + 8] = (juce::uint8) (d >> 8);     // X density (BE)
                p[i + 9] = (juce::uint8)  d;
                p[i + 10] = (juce::uint8) (d >> 8);    // Y density (BE)
                p[i + 11] = (juce::uint8)  d;
                return;
            }
        }
    }
}

bool exportImage(const juce::Image& img, const juce::File& dest, bool asPng, double dpi)
{
    if (! img.isValid())
        return false;

    // Encode to memory first so we can stamp the physical resolution before
    // touching disk (JUCE's PNG/JPEG writers don't expose DPI metadata).
    juce::MemoryOutputStream mem;
    bool ok;
    if (asPng)
    {
        juce::PNGImageFormat fmt;
        ok = fmt.writeImageToStream(img, mem);
    }
    else
    {
        juce::JPEGImageFormat fmt;
        fmt.setQuality(0.9f);
        ok = fmt.writeImageToStream(img, mem);
    }
    if (! ok)
        return false;

    juce::MemoryBlock bytes(mem.getData(), mem.getDataSize());
    if (dpi > 0.0)
    {
        if (asPng) bytes = pngWithDpi(bytes, dpi);
        else       jpegSetDpi(bytes, dpi);
    }

    dest.deleteFile();
    juce::FileOutputStream out(dest);
    if (out.failedToOpen())
        return false;
    if (! out.write(bytes.getData(), bytes.getSize()))
    {
        out.flush();
        dest.deleteFile();
        return false;
    }
    return true;
}

} // namespace scoregen
