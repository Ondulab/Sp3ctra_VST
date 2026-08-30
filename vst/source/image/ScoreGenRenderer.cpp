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

    // ── Constant-Q analysis plan (multi-resolution STFT) ─────────────────────
    //
    // The printed image is LOG in frequency: every octave gets the same
    // vertical space (432 rows/octave over the CIS sensor, 36 rows/semitone).
    // A single STFT window cannot serve that, because its resolution is a fixed
    // number of Hz while a semitone is a fixed RATIO: at 65 Hz a semitone spans
    // 3.9 Hz, at 16 kHz it spans 1016 Hz. One window is therefore always wrong
    // at both ends — too coarse in pitch at the bottom, needlessly fine (and so
    // needlessly smeared in time) at the top.
    //
    // The fix is to hold the number of signal CYCLES inside the window constant
    // instead of the number of Hz: a constant-Q filter bank, approximated here
    // by one window per octave, halving as the octave doubles. Pitch blur then
    // stays constant across the whole page, and the window shortens with pitch
    // so transients stay sharp exactly where the ear expects them to.
    struct LayerSpec
    {
        int    winSize = 0;
        double fCross  = 0.0;   // rows with centre freq >= fCross prefer this layer
    };

    // Cycles held inside the analysis window. The Blackman-Harris window this
    // engine applies has a -3 dB main lobe about 1.90 bins wide, so the pitch
    // blur works out at ~31.9 / kCyclesTarget semitones at an octave's lower
    // edge, and half that at its upper edge. 48 cycles => 2/3 .. 1/3 of a
    // semitone, everywhere. Raise it for finer pitch and longer windows, lower
    // it for sharper transients and coarser pitch.
    constexpr double kCyclesTarget = 48.0;

    // Floor on the window length, so the top octaves keep enough samples for
    // the 4-term window to mean anything.
    constexpr int kMinWindow = 128;

    // Candidate base windows for the ADAPTIVE choice, in seconds. Which one a
    // piece of material wants is a property of the material, not a setting, so
    // the renderer scores all of them against the signal and keeps the winner
    // (see score_choose_base_window_seconds). They are durations rather than
    // sample counts because a sample count is sample-rate dependent: the 4096
    // that spans 92.9 ms at 44.1 kHz spans only 42.7 ms at 96 kHz, silently
    // halving the pitch resolution of a high-rate file for no musical reason.
    //
    // Pitch blur they buy, at 65 / 130 / 260 Hz:
    //     46.4 ms -> 10.5 / 5.3  / 2.6  semitones   (keeps a drum attack)
    //     92.9 ms ->  5.3 / 2.6  / 1.3             (the legacy window)
    //      186 ms ->  2.6 / 1.3  / 0.66
    //      372 ms ->  1.3 / 0.66 / 0.33            (separates a chord)
    constexpr double kBaseWindowCandidates[] = { 0.0464, 0.0929, 0.1858, 0.3716 };
    constexpr int    kNumBaseWindowCandidates =
        (int) (sizeof(kBaseWindowCandidates) / sizeof(kBaseWindowCandidates[0]));

    // Fallback when the probe cannot run (signal shorter than any candidate):
    // the legacy 4096-sample window at 44.1 kHz.
    constexpr double kBaseWindowFallbackSeconds = 4096.0 / 44100.0;

    // Zero-padded FFT length for a layer. Kept a power of two (winSize itself
    // is not) so kiss_fftr never falls back to its slow path on a large prime
    // factor. 16x the window is plenty of frequency interpolation.
    inline int padSizeFor(int winSize)
    {
        int pad = 2;
        while (pad < winSize * 16 && pad < SCORE_FFT_EFFECTIVE_SIZE)
            pad *= 2;
        return juce::jmin(pad, SCORE_FFT_EFFECTIVE_SIZE);
    }

    // Builds the layer set, longest window first, from a base window the caller
    // has already chosen. The base layer owns the bottom octaves; each octave
    // above asks for the window that holds kCyclesTarget cycles at its lower
    // edge, so the windows halve as the octaves double. Every layer analyses
    // the WHOLE signal; the renderer picks one per image row.
    std::vector<LayerSpec> buildLayerPlan(int baseWin, int sampleRate,
                                          double minFreq, double maxFreq)
    {
        std::vector<LayerSpec> plan;
        plan.push_back({ juce::jmax(1, baseWin), 0.0 });
        if (minFreq <= 0.0 || maxFreq <= minFreq || sampleRate <= 0)
            return plan;               // degenerate band: nothing to lay out

        const int floorWin = juce::jmin(kMinWindow, plan.back().winSize);

        for (double fLo = minFreq * 2.0; fLo < maxFreq; fLo *= 2.0)
        {
            const double want = kCyclesTarget * (double) sampleRate / fLo;
            const int    win  = juce::jlimit(floorWin, plan.back().winSize,
                                             (int) std::lround(want));

            // Skip an octave whose ask is capped to (or barely under) the
            // window already in use — a near-duplicate layer costs a full
            // spectrogram and buys nothing.
            if ((double) win > 0.75 * (double) plan.back().winSize)
                continue;

            plan.push_back({ win, fLo });
        }
        return plan;
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
    if (s.pageFormat == 2)   // Selection sheet: the free region IS the window
        return (s.selectionSec > 0.0) ? s.selectionSec : 0.0;   // 0 = to end
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

    // ── Decode guard, before the signal buffers are allocated ────────────────
    // The spectrograms are priced later, once the analysis plan is known (it
    // depends on the signal), but the decoded audio is allocated first and has
    // to fit on its own. Budget scales with the machine: 25% of physical RAM,
    // clamped 2..8 GB.
    const double ramBytes = (double) juce::SystemStats::getMemorySizeInMegabytes() * 1.0e6;
    const double capBytes = juce::jlimit(2.0e9, 8.0e9, ramBytes * 0.25);
    {
        const double signalBytes = (double) framesToLoad * 8.0 * (stereo ? 2.0 : 1.0);
        if (signalBytes > capBytes)
            return fail("Selection too long to decode (~"
                        + juce::String(signalBytes / 1.0e9, 1) + " GB, cap "
                        + juce::String(capBytes / 1.0e9, 1)
                        + juce::String::fromUTF8(" GB) — shorten the selection"));
    }

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
    if (totalSamples < kMinWindow)
        return fail("Audio too short to analyse (" + juce::String(totalSamples)
                    + " samples)");

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

    // ── Analysis layer plan, chosen from the MATERIAL ────────────────────────
    // Long windows resolve pitch, short ones resolve time; which one this audio
    // wants is not a preference, it is a measurable property of the audio. The
    // probe scores each candidate by how compactly it represents the bottom of
    // the band and returns the winner: a held chord picks a long window (its
    // notes separate into lines), a drum picks a short one (its attacks stay
    // sharp). The probe runs on a decimated copy and costs a few milliseconds.
    double baseWindowSec = kBaseWindowFallbackSeconds;
    std::vector<double> windowScores((size_t) kNumBaseWindowCandidates, -1.0);
    {
        // Score over the span the base layer will own: the bottom four octaves.
        const double probeTop = juce::jmin(s.maxFreq, s.minFreq * 16.0);
        const double chosen = score_choose_base_window_seconds(
            signal.data(), totalSamples, sampleRate, s.minFreq, probeTop,
            kBaseWindowCandidates, kNumBaseWindowCandidates, windowScores.data());
        if (chosen > 0.0)
            baseWindowSec = chosen;
    }

    // What a plan costs: each layer stores num_windows x (padSize/2+1) doubles
    // per channel, zero-padded for fine frequency interpolation (~262 KB per
    // time column at full pad). A FULL sheet over a long file dwarfs the output
    // image by orders of magnitude — seen live at 68 GB.
    auto planBytes = [&](const std::vector<LayerSpec>& plan)
    {
        const double windows = (double) totalSamples
                                   / juce::jmax(1.0, (double) sampleRate / binsPerSecond)
                             + 2.0;
        double b = 0.0;
        for (const auto& L : plan)
            b += windows * (double) (padSizeFor(L.winSize) / 2 + 1) * (double) sizeof(double);
        return stereo ? b * 2.0 : b;                     // dataR per layer
    };

    std::vector<LayerSpec> layerPlan;
    {
        // Start from the measured window and step DOWN until the analysis fits
        // the budget. A longer base window multiplies the number of layers and
        // their padded widths, so the memory can run away on a long selection;
        // giving up some low-frequency pitch resolution is a far better outcome
        // than refusing to render at all.
        int idx = 0;
        for (int c = 0; c < kNumBaseWindowCandidates; ++c)
            if (kBaseWindowCandidates[c] <= baseWindowSec + 1e-9) idx = c;

        for (; idx >= 0; --idx)
        {
            const int win = juce::jlimit(kMinWindow,
                                juce::jmin(SCORE_FFT_EFFECTIVE_SIZE, totalSamples),
                                (int) std::lround(kBaseWindowCandidates[idx] * sampleRate));
            auto plan = buildLayerPlan(win, sampleRate, s.minFreq, s.maxFreq);
            if (planBytes(plan) <= capBytes || idx == 0)
            {
                layerPlan = std::move(plan);
                break;
            }
        }
    }

    // Frame centres are aligned to the LONGEST window (the plan is built
    // descending, so that is the first layer). It must not be shorter than any
    // layer: score_compute_spectrogram_ex offsets each frame by
    // (align - size)/2, which would run off the front of the signal if a layer
    // were longer than the reference.
    const int alignWin = layerPlan.front().winSize;
    if (totalSamples < alignWin)
        return fail("Audio too short for FFT window (" + juce::String(totalSamples)
                    + " < " + juce::String(alignWin) + " samples)");

    if (planBytes(layerPlan) > capBytes)
        return fail("Analysis too large (~" + juce::String(planBytes(layerPlan) / 1.0e9, 1)
                    + " GB, cap " + juce::String(capBytes / 1.0e9, 1)
                    + juce::String::fromUTF8(" GB) — shorten the selection, "
                      "lower the DPI or the writing speed, or disable "
                      "stereo/multi-res"));

    // ── STFT analysis layers (left/primary, plus right when stereo) ─────────
    //
    // One window per octave, halving as the octave doubles, so every band
    // holds the same number of signal cycles —
    // Gabor's time/frequency trade-off applied PER BAND instead of globally.
    // See buildLayerPlan() for the constant-Q reasoning. Lows keep the long
    // window (full harmonic resolution); highs get windows short enough that
    // transients stay sharp.
    // Every layer's frames are CENTER-ALIGNED to layer 0's grid and magnitudes
    // are normalised by each window's coherent gain, so the layers splice into
    // one consistent dB map. Encoder-only: the printed image plays back through
    // the unchanged instrument.
    struct SpecLayer
    {
        ScoreSpectrogramData data  {};
        ScoreSpectrogramData dataR {};   // stereo right channel (mono: unused)
        int    winSize = 0;
        int    padSize = 0;
        double freqRes = 0.0;
        double fCross  = 0.0;   // rows with centre freq ≥ fCross prefer this layer
    };

    constexpr double kBlend = 1.1224620483; // 2^(1/6): ±1/6 octave crossfade

    std::vector<SpecLayer> layers(layerPlan.size());
    auto freeLayers = [&layers]()
    {
        for (auto& L : layers)
        {
            score_free_spectrogram(&L.data);
            score_free_spectrogram(&L.dataR);
        }
    };

    for (size_t li = 0; li < layers.size(); ++li)
    {
        auto& L = layers[li];
        L.winSize = layerPlan[li].winSize;
        L.padSize = padSizeFor(L.winSize);
        L.freqRes = (double) sampleRate / (double) L.padSize;
        L.fCross  = layerPlan[li].fCross;

        int rc = score_compute_spectrogram_ex(signal.data(), totalSamples, sampleRate,
                                              L.winSize, L.padSize, alignWin,
                                              1, binsPerSecond,
                                              s.minFreq, s.maxFreq, &L.data);
        if (rc == 0 && stereo)
            rc = score_compute_spectrogram_ex(signalR.data(), totalSamples, sampleRate,
                                              L.winSize, L.padSize, alignWin,
                                              1, binsPerSecond,
                                              s.minFreq, s.maxFreq, &L.dataR);
        if (rc != 0)
        {
            freeLayers();
            return fail("Spectrogram computation failed (code " + juce::String(rc) + ")");
        }

        if (progress)
            progress(0.10f + 0.45f * (float) (li + 1) / (float) layers.size());
        if (shouldAbort && shouldAbort()) { freeLayers(); return fail("Aborted"); }
    }

    // Share ONE magnitude reference across every layer and both channels so
    // the dB/intensity mapping is consistent: across L-vs-R (a quieter channel
    // must stay quieter) AND across layers (a partial must keep its level when
    // it crosses a layer boundary).
    //
    // The reference must be the peak of what actually gets DRAWN. Every layer
    // analyses the whole spectrum, but a row only ever reads the layer that
    // owns its octave, so a layer's peak outside its own span is never printed.
    // Taking it as the reference would push max_dB above anything on the page:
    // the image comes out washed (nothing reaches black) and the bottom of the
    // dynamic-range window clips content that should have been visible. The
    // engine's own global_max is wider still — it spans DC to Nyquist,
    // including everything outside the printed frequency band.
    {
        auto spanMax = [](const ScoreSpectrogramData& d, double fLo, double fHi,
                          double freqRes) -> double
        {
            if (d.data == nullptr || d.num_windows <= 0 || freqRes <= 0.0)
                return 0.0;
            const int bLo = juce::jmax(d.index_min, (int) std::floor(fLo / freqRes));
            const int bHi = juce::jmin(d.index_max, (int) std::ceil (fHi / freqRes));
            if (bLo > bHi) return 0.0;
            double m = 0.0;
            for (int w = 0; w < d.num_windows; ++w)
            {
                const double* col = d.data + (size_t) w * d.num_bins;
                for (int b = bLo; b <= bHi; ++b)
                    if (col[b] > m) m = col[b];
            }
            return m;
        };

        double sharedMax = 0.0;
        for (size_t li = 0; li < layers.size(); ++li)
        {
            const auto& L = layers[li];
            // The span this layer owns, widened by the crossfade that reaches
            // 1/6 octave either side of a boundary.
            const double fLo = (li == 0) ? s.minFreq : L.fCross / kBlend;
            const double fHi = (li + 1 < layers.size()) ? layers[li + 1].fCross * kBlend
                                                        : s.maxFreq;
            sharedMax = juce::jmax(sharedMax, spanMax(L.data, fLo, fHi, L.freqRes));
            if (stereo)
                sharedMax = juce::jmax(sharedMax, spanMax(L.dataR, fLo, fHi, L.freqRes));
        }
        if (sharedMax <= 0.0)                       // degenerate: keep the old
            for (auto& L : layers)                  // reference rather than a
            {                                       // divide-by-zero black page
                sharedMax = juce::jmax(sharedMax, L.data.global_max);
                if (stereo) sharedMax = juce::jmax(sharedMax, L.dataR.global_max);
            }
        for (auto& L : layers)
        {
            L.data.global_max = sharedMax;
            if (stereo) L.dataR.global_max = sharedMax;
        }
    }

    for (auto& L : layers)
    {
        score_apply_image_processing(&L.data, s.dynamicRangeDB, s.gammaCorrection,
                                     s.enableDithering, s.contrastFactor,
                                     s.enableNoiseGate, s.noiseGateThreshold);
        if (stereo)
            score_apply_image_processing(&L.dataR, s.dynamicRangeDB, s.gammaCorrection,
                                         s.enableDithering, s.contrastFactor,
                                         s.enableNoiseGate, s.noiseGateThreshold);
    }

    ScoreSpectrogramData& spec = layers[0].data;   // reference layer (windows/bins)

    if (progress) progress(0.60f);

    // ── Layout (port of spectral_raster.c:556-707) ───────────────────────────
    // pageFormat 2 = Selection: one sheet stretched to hold the SELECTED
    // region at the writing speed (A4/A3 share the fixed-page geometry; the
    // helpers fall back to A4 width / 297 mm height for format 2).
    const double labelMargin = 150.0 * (dpi / 400.0);
    double pageW = pageWidthPx(s.pageFormat, dpi);
    const double pageH = pageHeightPx(s.pageFormat, dpi);
    if (s.pageFormat == 2 && s.writingSpeed > 0.0)
    {
        const double realDur = (double) totalSamples / (double) sampleRate;
        pageW = labelMargin + realDur * s.writingSpeed * (dpi / 2.54);
    }
    const int imageW = (int) pageW;
    const int imageH = (int) pageH;
    // Same ceiling as MIDI SCORE's FULL sheets (~1.5 GB transient RGB).
    if ((juce::int64) imageW * (juce::int64) imageH > (juce::int64) 500'000'000)
        return fail("Sheet too large (" + juce::String(imageW) + " x "
                    + juce::String(imageH)
                    + juce::String::fromUTF8(" px) — shorten the selection or"
                                             " lower the DPI / writing speed"));

    const double mmPx           = mmToPixels(dpi);
    const double bottomMarginPx = s.bottomMarginMM  * mmPx;
    const double spectroHeightPx= s.spectroHeightMM * mmPx;

    const double spectroLeft   = labelMargin;
    const double spectroWidth  = pageW - labelMargin;
    const double spectroBottom = pageH - bottomMarginPx;
    const double spectroTop    = spectroBottom - spectroHeightPx;

    const double freqRange = s.maxFreq - s.minFreq;

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

    const double drawnW = visibleWindows * windowWidth;
    int xStart = (int) std::floor(spectroLeft);
    int xEnd   = (int) std::ceil (spectroLeft + drawnW);
    int yTop   = (int) std::floor(spectroTop);
    int yBot   = (int) std::ceil (spectroBottom);
    xStart = juce::jmax(0, xStart);
    xEnd   = juce::jmin(imageW, xEnd);
    yTop   = juce::jmax(0, yTop);
    yBot   = juce::jmin(imageH, yBot);

    // ── Per-output-row layer choice + FFT bin cell [lo,hi] (LOG freq axis) ───
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
    //
    // Each row picks the layer whose window suits its centre
    // frequency; rows within ±1/6 octave of a layer crossover crossfade the two
    // layers' intensities so no seam is visible/audible at the boundary.
    struct RowMap
    {
        int   lA  = -1, lB = -1;     // layer indices (lB used when t > 0)
        float t   = 0.0f;            // 0 = pure lA … 1 = pure lB
        int   aLo = -1, aHi = -1;    // bin cell in layer lA
        int   bLo = -1, bHi = -1;    // bin cell in layer lB
    };

    const bool   logMap    = (s.minFreq > 0.0 && s.maxFreq > s.minFreq);
    const double freqRatio = logMap ? (s.maxFreq / s.minFreq) : 1.0;
    const int    rows      = (yBot > yTop) ? (yBot - yTop) : 0;
    std::vector<RowMap> rowMap((size_t) rows);

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

        auto binsFor = [&](int li, int& lo, int& hi) -> bool
        {
            const auto& L = layers[(size_t) li];
            int bl = (int) std::floor(fLo / L.freqRes);
            int bh = (int) std::ceil (fHi / L.freqRes);
            bl = juce::jmax(bl, L.data.index_min);
            bh = juce::jmin(juce::jmin(bh, L.data.index_max), L.data.num_bins - 1);
            if (bl > bh) return false;
            lo = bl; hi = bh;
            return true;
        };

        auto& rm = rowMap[(size_t) (y - yTop)];

        // Base layer: the last one whose octave has started at this row's
        // (geometric) centre frequency, i.e. the shortest window that still
        // holds the target cycle count there.
        const double fC = std::sqrt(fLo * fHi);
        int k = 0;
        for (size_t li = 1; li < layers.size(); ++li)
            if (fC >= layers[li].fCross) k = (int) li;

        rm.lA = rm.lB = k;
        rm.t  = 0.0f;
        if (k + 1 < (int) layers.size())            // approaching the next crossover
        {
            const double fx = layers[(size_t) (k + 1)].fCross;
            if (fC > fx / kBlend)
            {
                rm.lB = k + 1;
                rm.t  = (float) (0.5 * (std::log(fC) - std::log(fx / kBlend))
                                     / std::log(kBlend));
            }
        }
        if (k > 0)                                   // just past the previous one
        {
            const double fx = layers[(size_t) k].fCross;
            if (fC < fx * kBlend)
            {
                rm.lA = k - 1;
                rm.lB = k;
                rm.t  = (float) (0.5 + 0.5 * (std::log(fC) - std::log(fx))
                                           / std::log(kBlend));
            }
        }
        rm.t = juce::jlimit(0.0f, 1.0f, rm.t);

        if (! binsFor(rm.lA, rm.aLo, rm.aHi)) { rm.lA = -1; continue; }
        if (rm.lB != rm.lA && ! binsFor(rm.lB, rm.bLo, rm.bHi))
        { rm.lB = rm.lA; rm.t = 0.0f; }              // blend partner unusable → pure lA
    }

    {
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
        const int span = juce::jmax(1, xEnd - xStart);

        // Peak-hold over a cell: darkest (loudest) bin = MIN intensity
        // (0 = black = energy, 1 = white = silence).
        auto cellMin = [](const double* col, int lo, int hi) -> double
        {
            double v = col[lo];
            for (int b = lo + 1; b <= hi; ++b)
                if (col[b] < v) v = col[b];
            return v;
        };

        for (int x = xStart; x < xEnd; ++x)
        {
            int w = (int) ((x - spectroLeft) / windowWidth);
            w = juce::jlimit(0, visibleWindows - 1, w);

            for (int y = yTop; y < yBot; ++y)
            {
                const auto& rm = rowMap[(size_t) (y - yTop)];
                if (rm.lA < 0)
                    continue;                                  // row outside the band

                const auto& LA = layers[(size_t) rm.lA];
                const double* colA  = LA.data.data + (size_t) w * LA.data.num_bins;
                const double* colAR = stereo ? LA.dataR.data + (size_t) w * LA.data.num_bins
                                             : colA;

                double leftVal  = cellMin(colA,  rm.aLo, rm.aHi);
                double rightVal = stereo ? cellMin(colAR, rm.aLo, rm.aHi) : leftVal;

                if (rm.t > 0.0f && rm.lB != rm.lA)
                {
                    const auto& LB = layers[(size_t) rm.lB];
                    const double* colB  = LB.data.data + (size_t) w * LB.data.num_bins;
                    leftVal += rm.t * (cellMin(colB, rm.bLo, rm.bHi) - leftVal);
                    if (stereo)
                    {
                        const double* colBR = LB.dataR.data + (size_t) w * LB.data.num_bins;
                        rightVal += rm.t * (cellMin(colBR, rm.bLo, rm.bHi) - rightVal);
                    }
                    else
                        rightVal = leftVal;
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
                freeLayers();
                return fail("Aborted");
            }
        }
    }

    logLines.add("Page: " + juce::String(imageW) + " x " + juce::String(imageH)
                 + " px @ " + juce::String(dpi, 0) + " DPI");
    logLines.add("Sample rate: " + juce::String(sampleRate) + " Hz");
    // Per layer: the window, and what it buys at the layer's LOWER edge —
    // the cycle count (constant by construction) and the pitch blur that
    // follows from it. Both improve by 2x towards the layer's upper edge.
    // The base window was measured, not set: show the scores so a close
    // call (or a wrong-looking one) is visible rather than mysterious.
    juce::StringArray sc;
    for (int c = 0; c < kNumBaseWindowCandidates; ++c)
        sc.add(juce::String(kBaseWindowCandidates[c] * 1000.0, 0) + "ms="
               + (windowScores[(size_t) c] >= 0.0
                    ? juce::String(windowScores[(size_t) c], 4)
                    : juce::String("n/a")));
    logLines.add("Base window chosen from the material: "
                 + juce::String(1000.0 * alignWin / sampleRate, 1) + " ms");
    logLines.add("  compactness, lower = better: " + sc.joinIntoString("  "));
    logLines.add("Constant-Q layers (target " + juce::String(kCyclesTarget, 0)
                 + " cycles, values at each layer's lower edge):");
    for (size_t li = 0; li < layers.size(); ++li)
    {
        const auto&  L      = layers[li];
        const double fLo    = (L.fCross > 0.0) ? L.fCross : s.minFreq;
        const double cycles = (double) L.winSize * fLo / (double) sampleRate;
        // Blackman-Harris -3 dB main lobe ~1.90 bins; a semitone is 5.946%.
        const double semis  = (cycles > 1e-9) ? (1.90 / 0.05946) / cycles : 0.0;
        logLines.add("  " + juce::String(L.winSize) + " smp ("
                     + juce::String((double) L.winSize * 1000.0 / sampleRate, 1)
                     + " ms, pad " + juce::String(L.padSize) + ") from "
                     + juce::String(fLo, 0) + " Hz: "
                     + juce::String(cycles, 0) + " cycles, "
                     + juce::String(semis, 2) + " semitone blur");
    }

    // Columns the page spends per base analysis window. Far above 1 means
    // adjacent columns are near-identical: the writing speed is buying paper
    // resolution the analysis cannot fill, and the page holds less audio for
    // no gain in detail.
    logLines.add("Time: " + juce::String(1000.0 * alignWin / sampleRate, 1)
                 + " ms base window vs " + juce::String(1000.0 / binsPerSecond, 2)
                 + " ms/column = "
                 + juce::String(binsPerSecond * alignWin / sampleRate, 0)
                 + " columns per window");
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
                     + juce::String::fromUTF8(" ch) → greyscale. Load a stereo WAV for L/R."));

    freeLayers();

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

    // Insert a "Sp3ctraCal" tEXt chunk (band + frequency range) right after
    // IHDR so the sampler can reload the image with the score's EXACT log
    // frequency / band mapping. Same defensive contract as pngWithDpi.
    juce::MemoryBlock pngWithCalibration(const juce::MemoryBlock& src,
                                         const scoregen::SpectroCalibration& cal)
    {
        const auto*  p = static_cast<const juce::uint8*> (src.getData());
        const size_t n = src.getSize();
        const size_t headEnd = 8 + 25;   // 8-byte signature + 25-byte IHDR chunk
        const juce::uint8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (n < headEnd || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0)
            return src;

        // ASCII "key=value;" payload — self-describing and version-tagged.
        juce::String payload;
        payload << "v=1"
                << ";bx=" << cal.band.getX()      << ";by=" << cal.band.getY()
                << ";bw=" << cal.band.getWidth()  << ";bh=" << cal.band.getHeight()
                << ";lo=" << juce::String(cal.minHz, 6)
                << ";hi=" << juce::String(cal.maxHz, 6)
                << ";st=" << (cal.stereo ? 1 : 0);

        static const char kKeyword[] = "Sp3ctraCal";   // 10 chars, no NUL
        juce::MemoryBlock chunk;                        // "tEXt" + keyword + 0 + text
        chunk.append("tEXt", 4);
        chunk.append(kKeyword, sizeof(kKeyword) - 1);
        const char nul = 0;
        chunk.append(&nul, 1);
        const char* utf = payload.toRawUTF8();
        chunk.append(utf, std::strlen(utf));

        juce::MemoryBlock out;
        out.append(p, headEnd);                                         // signature + IHDR
        appendBE32(out, (juce::uint32) (chunk.getSize() - 4));          // data length (excl "tEXt")
        out.append(chunk.getData(), chunk.getSize());                   // "tEXt" + data
        appendBE32(out, pngCrc32((const juce::uint8*) chunk.getData(),
                                 chunk.getSize()));                     // CRC over type+data
        out.append(p + headEnd, n - headEnd);                          // pHYs / IDAT … IEND
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

bool exportImage(const juce::Image& img, const juce::File& dest, bool asPng, double dpi,
                 const SpectroCalibration* cal)
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
    // Calibration only rides PNG (lossless, chunk-based); JPEG export stays a
    // plain picture — the sampler falls back to its row/column scan for those.
    if (asPng && cal != nullptr && cal->valid)
        bytes = pngWithCalibration(bytes, *cal);

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

juce::MemoryBlock encodeCalibratedPng(const juce::Image& img,
                                      const SpectroCalibration& cal)
{
    if (! img.isValid() || ! cal.valid)
        return {};
    juce::MemoryOutputStream mem;
    juce::PNGImageFormat fmt;
    if (! fmt.writeImageToStream(img, mem))
        return {};
    juce::MemoryBlock bytes(mem.getData(), mem.getDataSize());
    return pngWithCalibration(bytes, cal);
}

SpectroCalibration readCalibration(const juce::File& pngFile)
{
    juce::MemoryBlock mb;
    if (! pngFile.loadFileAsData(mb))
        return {};
    return readCalibration(mb.getData(), mb.getSize());
}

SpectroCalibration readCalibration(const void* pngData, size_t numBytes)
{
    SpectroCalibration cal;

    const auto*  p = static_cast<const juce::uint8*> (pngData);
    const size_t n = numBytes;
    if (p == nullptr)
        return cal;
    const juce::uint8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 8 || memcmp(p, sig, 8) != 0)
        return cal;                                   // not a PNG

    static const char kKeyword[] = "Sp3ctraCal";
    const size_t kwLen = sizeof(kKeyword) - 1;

    // Walk the chunk list: [len:4 BE][type:4][data:len][crc:4].
    size_t pos = 8;
    while (pos + 8 <= n)
    {
        const juce::uint32 len = ((juce::uint32) p[pos]     << 24)
                               | ((juce::uint32) p[pos + 1] << 16)
                               | ((juce::uint32) p[pos + 2] <<  8)
                               |  (juce::uint32) p[pos + 3];
        const juce::uint8* type = p + pos + 4;
        const size_t dataPos = pos + 8;
        if (dataPos + (size_t) len + 4 > n)
            break;                                    // truncated / malformed

        if (memcmp(type, "tEXt", 4) == 0
            && len > kwLen + 1
            && memcmp(p + dataPos, kKeyword, kwLen) == 0
            && p[dataPos + kwLen] == 0)
        {
            const juce::String text(
                juce::CharPointer_UTF8((const char*) (p + dataPos + kwLen + 1)),
                (size_t) (len - kwLen - 1));
            int bx = 0, by = 0, bw = 0, bh = 0, st = 0;
            double lo = 0.0, hi = 0.0;
            for (const auto& tok : juce::StringArray::fromTokens(text, ";", ""))
            {
                const int eq = tok.indexOfChar('=');
                if (eq <= 0) continue;
                const juce::String k = tok.substring(0, eq);
                const juce::String v = tok.substring(eq + 1);
                if      (k == "bx") bx = v.getIntValue();
                else if (k == "by") by = v.getIntValue();
                else if (k == "bw") bw = v.getIntValue();
                else if (k == "bh") bh = v.getIntValue();
                else if (k == "lo") lo = v.getDoubleValue();
                else if (k == "hi") hi = v.getDoubleValue();
                else if (k == "st") st = v.getIntValue();
            }
            if (bw > 0 && bh > 0 && lo > 0.0 && hi > lo)
            {
                cal.band   = juce::Rectangle<int>(bx, by, bw, bh);
                cal.minHz  = lo;
                cal.maxHz  = hi;
                cal.stereo = (st != 0);
                cal.valid  = true;
            }
            return cal;
        }

        if (memcmp(type, "IEND", 4) == 0)
            break;
        pos = dataPos + (size_t) len + 4;             // advance past data + CRC
    }

    return cal;
}

} // namespace scoregen
