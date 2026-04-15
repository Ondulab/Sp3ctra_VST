#include "CisVisualizerComponent.h"
#include "PluginProcessor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" {
    #include "audio/buffers/audio_image_buffers.h"
    #include "config/config_instrument.h"
    #include "config/config_loader.h"
    #include "processing/image_pipeline_types.h"
    #include "synthesis/luxsynth/kissfft/kiss_fftr.h"
}

// Forward-declare C hooks defined in FrameSampler.cpp.
extern "C" int frame_sampler_is_playing(void);
extern "C" int frame_sampler_is_recording(void);

//==============================================================================
CisVisualizerComponent::CisVisualizerComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    startTimer(1000 / kTimerFps);
}

CisVisualizerComponent::~CisVisualizerComponent()
{
    stopTimer();
    // Release the cached KissFFT config (allocated on heap by kiss_fftr_alloc)
    if (fftCfg_)
    {
        kiss_fft_free(reinterpret_cast<kiss_fftr_cfg>(fftCfg_));
        fftCfg_ = nullptr;
    }
}

//==============================================================================
void CisVisualizerComponent::setBlobRegions(
    const std::vector<std::pair<float, float>>& regions)
{
    // UI thread only — simple assignment, no lock needed
    blobRegions = regions;
}

//==============================================================================
void CisVisualizerComponent::setActiveSource(VisualizerMode mode) noexcept
{
    activeSource_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

VisualizerMode CisVisualizerComponent::getActiveSource() const noexcept
{
    return static_cast<VisualizerMode>(activeSource_.load(std::memory_order_relaxed));
}

//==============================================================================
void CisVisualizerComponent::paint(juce::Graphics& g)
{
    // Guard: suspended during prepareToPlay to prevent CoreGraphics crash
    if (isSuspended.load())
    {
        g.fillAll(juce::Colour(0xff1a1a1a));
        return;
    }

    const int W = getWidth();
    const int H = getHeight();

    if (cisPixelsCount == 0)
    {
        g.fillAll(juce::Colour(0xff1a1a1a));
        g.setColour(juce::Colours::grey);
        g.drawText("Waiting for CIS data...", getLocalBounds(),
                   juce::Justification::centred);
        return;
    }

    // ── Active pipeline source (selected via pipeline node click) ─────────────
    const auto source = getActiveSource();

    // ── SPCTR_BLOB: dedicated coloured blob visualizer (LuxStral path) ───────
    if (source == VisualizerMode::SPCTR_BLOB)
    {
        paintSpctrBlobMode(g, W, H);
        paintSourceLabel(g, W, H);
        return;
    }

    // ── SYNTH_BLOB: dedicated coloured blob visualizer ────────────────────────
    // Intercept before the generic rendering path; has its own full renderer.
    if (source == VisualizerMode::SYNTH_BLOB)
    {
        paintSynthBlobMode(g, W, H);
        paintSourceLabel(g, W, H);
        return;
    }

    // ── FFT mode: dedicated spectrum renderer ────────────────────────────────
    if (source == VisualizerMode::SYNTH_FFT_COLOR)
    {
        paintFftColorMode(g, W, H);
        paintSourceLabel(g, W, H);
        return;
    }

    // ── COLOR sources always use the colour-temperature renderer ──────────────
    if (isColorSource(source))
    {
        paintColorTemperatureMode(g, W, H);
    }
    else
    {
        // ── Render style (Image / Waveform / Inverted) from Settings ─────────
        // Waveform modes only apply to sources that support display modes
        // (RAW, Live, Sampler, Gray).  BLOB sources always use image mode.
        const int renderMode = supportsDisplayModes(source)
            ? static_cast<int>(
                processor.getAPVTS().getRawParameterValue("visualizerMode")->load())
            : 0;

        // Source-level views use raw RGB; downstream views use processed gray
        const bool isSourceView = (source == VisualizerMode::RAW
                                || source == VisualizerMode::SAMPLER
                                || source == VisualizerMode::LIVE
                                || source == VisualizerMode::MIX);

        switch (renderMode)
        {
            case 1:  paintWaveformMode(g, W, H, false, !isSourceView); break;
            case 2:  paintWaveformMode(g, W, H, true,  !isSourceView); break;
            default:
            {
                if (isSourceView)
                    paintRawImageMode(g, W, H);
                else
                    paintImageMode(g, W, H);
                break;
            }
        }

        // ── Blob overlay — visible for BLOB sources or when IMAGE tab is active
        const bool isBlobSource = (source == VisualizerMode::SPCTR_BLOB
                                || source == VisualizerMode::SYNTH_BLOB);
        if ((blobOverlayVisible || isBlobSource) && renderMode == 0)
            paintBlobOverlay(g, W, H);
    }

    // ── Source label overlay (always shown) ───────────────────────────────────
    paintSourceLabel(g, W, H);
}

//==============================================================================
void CisVisualizerComponent::paintImageMode(
    juce::Graphics& g, int W, int H) const
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    // localDataGray already contains the fully-processed final image:
    //   RGB→gray, inversion, correct gamma (sampler or live) applied in
    //   updateCisData().  Display it directly — no additional transforms.
    if (localDataGray.empty()) return;

    for (int x = 0; x < W; ++x)
    {
        const uint8_t v = interpolateCisPixel(localDataGray.data(), x, W);
        g.setColour(juce::Colour(v, v, v));
        g.fillRect(x, 0, 1, H);
    }
}

//==============================================================================
void CisVisualizerComponent::paintWaveformMode(
    juce::Graphics& g, int W, int H, bool inverted, bool useGray) const
{
    g.fillAll(juce::Colours::white);
    const int centerY    = H / 2;
    const int halfHeight = H / 2;

    for (int x = 0; x < W; ++x)
    {
        if (useGray)
        {
            // Downstream views: use processed grayscale (with inversion + gamma)
            const uint8_t v = interpolateCisPixel(localDataGray.data(), x, W);
            const int barH = inverted
                           ? ((255 - v) * halfHeight) / 255
                           : (v * halfHeight) / 255;
            if (barH > 0)
            {
                g.setColour(juce::Colour(v, v, v));
                g.fillRect(x, centerY - barH, 1, barH * 2);
            }
        }
        else
        {
            // Source views: use raw RGB
            const uint8_t r  = interpolateCisPixel(localDataR.data(), x, W);
            const uint8_t gr = interpolateCisPixel(localDataG.data(), x, W);
            const uint8_t b  = interpolateCisPixel(localDataB.data(), x, W);
            const int maxCh  = std::max({r, gr, b});

            const int barH = inverted
                           ? ((255 - maxCh) * halfHeight) / 255
                           : (maxCh * halfHeight) / 255;
            if (barH > 0)
            {
                g.setColour(juce::Colour(r, gr, b));
                g.fillRect(x, centerY - barH, 1, barH * 2);
            }
        }
    }
}

//==============================================================================
void CisVisualizerComponent::paintBlobOverlay(
    juce::Graphics& g, int W, int H) const
{
    if (blobRegions.empty()) return;

    // Semi-transparent orange bounding boxes
    g.setColour(juce::Colour(0xb0ff8800));

    for (const auto& [x0n, x1n] : blobRegions)
    {
        const int x0 = juce::roundToInt(x0n * (float)(W - 1));
        const int x1 = juce::roundToInt(x1n * (float)(W - 1));
        const int bw = juce::jmax(1, x1 - x0);

        // Outer rectangle
        g.drawRect(x0, 0, bw, H, 1);

        // Top / bottom colour stripe (1-px thick, a bit darker)
        g.setColour(juce::Colour(0x60ff8800));
        g.fillRect(x0, 0,   bw, 2);
        g.fillRect(x0, H-2, bw, 2);
        g.setColour(juce::Colour(0xb0ff8800));
    }

    // Blob count label (top-right)
    g.setColour(juce::Colour(0xffff8800));
    g.setFont(juce::FontOptions(9.f));
    g.drawText(juce::String(blobRegions.size()) + " blobs",
               getLocalBounds().reduced(3).removeFromRight(60).removeFromTop(14),
               juce::Justification::centredRight, false);
}

//==============================================================================
void CisVisualizerComponent::paintRawImageMode(
    juce::Graphics& g, int W, int H) const
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    // RAW mode: display the full-colour RGB data from AudioImageBuffers
    // (no grayscale conversion, no inversion, no gamma).
    if (localDataR.empty()) return;

    for (int x = 0; x < W; ++x)
    {
        const uint8_t r  = interpolateCisPixel(localDataR.data(), x, W);
        const uint8_t gr = interpolateCisPixel(localDataG.data(), x, W);
        const uint8_t b  = interpolateCisPixel(localDataB.data(), x, W);
        g.setColour(juce::Colour(r, gr, b));
        g.fillRect(x, 0, 1, H);
    }
}

//==============================================================================
void CisVisualizerComponent::paintSourceLabel(
    juce::Graphics& g, int W, int H) const
{
    const auto source = getActiveSource();
    const char* label = visualizerModeLabel(source);

    // Semi-transparent pill badge — top-left corner
    const float pillW = 8.f + juce::Font(juce::FontOptions(10.f)).getStringWidthFloat(label) + 8.f;
    constexpr float pillH = 16.f;
    constexpr float pillX = 4.f;
    constexpr float pillY = 4.f;

    // Accent colour based on pipeline family
    juce::Colour accent;
    switch (source)
    {
        case VisualizerMode::RAW:
        case VisualizerMode::LIVE:
        case VisualizerMode::SAMPLER:
        case VisualizerMode::MIX:
            accent = juce::Colour(0xffa87ae0); // Sources — purple
            break;
        case VisualizerMode::SPCTR_GRAY:
            accent = juce::Colour(0xff6bb8e0); break;
        case VisualizerMode::SPCTR_COLOR:
            accent = juce::Colour(0xff4ae0c8); break;
        case VisualizerMode::SPCTR_BLOB:
            accent = juce::Colour(0xff8888e0); break;
        case VisualizerMode::SYNTH_GRAY:
            accent = juce::Colour(0xffe0a84a); break;
        case VisualizerMode::SYNTH_COLOR:
            accent = juce::Colour(0xffe0c864); break;
        case VisualizerMode::SYNTH_BLOB:
            accent = juce::Colour(0xffd07040); break;
        case VisualizerMode::SYNTH_FFT_COLOR:
            accent = juce::Colour(0xffcc88cc); break;
        default:
            accent = juce::Colour(0xffe08844); break;
    }

    g.setColour(accent.withAlpha(0.55f));
    g.fillRoundedRectangle(pillX, pillY, pillW, pillH, 3.f);

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(10.f));
    g.drawText(label,
               juce::Rectangle<float>(pillX, pillY, pillW, pillH),
               juce::Justification::centred, false);
}

//==============================================================================
void CisVisualizerComponent::resized() {}

//==============================================================================
void CisVisualizerComponent::timerCallback()
{
    updateCisData();
    repaint();
}

//==============================================================================
void CisVisualizerComponent::suspend()
{
    isSuspended.store(true);
    stopTimer();
}

void CisVisualizerComponent::resume()
{
    isSuspended.store(false);
    startTimer(1000 / kTimerFps);
}

//==============================================================================
void CisVisualizerComponent::updateCisData()
{
    auto* core = processor.getSp3ctraCore();
    if (!core || !core->isInitialized())
    {
        cisPixelsCount = 0;
        return;
    }

    auto* buffers = core->getAudioImageBuffers();
    if (!buffers || !buffers->initialized)
    {
        cisPixelsCount = 0;
        return;
    }

    // ── Determine sensor pixel count ─────────────────────────────────────────
    extern sp3ctra_config_t g_sp3ctra_config;
    const int newCount = (g_sp3ctra_config.sensor_dpi == 400)
                         ? CIS_400DPI_PIXELS_NB
                         : CIS_200DPI_PIXELS_NB;

    // ── Ensure local buffers are sized ───────────────────────────────────────
    if (cisPixelsCount != newCount)
    {
        cisPixelsCount = newCount;
        localDataR.resize(cisPixelsCount, 255);
        localDataG.resize(cisPixelsCount, 255);
        localDataB.resize(cisPixelsCount, 255);
        localDataGray.resize(cisPixelsCount, 255);
    }

    // ── Read transport states ─────────────────────────────────────────────────
    const int rawLiveFreeze = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("imageFreezeMode")->load());

    // ── RAW upstream gate (mirror image_preprocessor.c logic) ─────────────────
    // RAW freeze overrides downstream streams when it is more restrictive.
    const int rawFreezeMode = g_sp3ctra_config.raw_freeze_mode;
    const int liveFreezeMode = (rawFreezeMode > rawLiveFreeze) ? rawFreezeMode : rawLiveFreeze;

    // ── FramePlayerThread writes to AudioImageBuffers only when a slot is
    // actively playing AND the sampler transport is not STOP.
    const int  rawSmpFreeze  = g_sp3ctra_config.sampler_freeze_mode;
    const int  smpFreezeMode = (rawFreezeMode > rawSmpFreeze) ? rawFreezeMode : rawSmpFreeze;
    const bool samplerWriting = (frame_sampler_is_playing() != 0)
                                && (smpFreezeMode != 2);

    // ── Active visualizer source ──────────────────────────────────────────────
    const auto vizSource = getActiveSource();

    // ── Source-specific freeze gates ──────────────────────────────────────────
    // Each visualizer source has its own freeze semantics:
    //   RAW  : only rawFreezeMode (upstream of everything)
    //   LIVE : only liveFreezeMode (independent of sampler)
    //   MIX  : liveFreezeMode AND samplerWriting (the actual audio output)
    //   Others: same as MIX
    if (vizSource == VisualizerMode::RAW)
    {
        if (rawFreezeMode == 2) // RAW STOP → white
        {
            std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
            std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
            std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
            std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
            if (auto* fs = processor.getFrameSampler())
                fs->setFinalGrayBuffer(localDataGray);
            return;
        }
        if (rawFreezeMode == 1) return; // RAW HOLD → freeze display
    }
    else if (vizSource == VisualizerMode::LIVE)
    {
        // LIVE freeze gates: only respect liveFreezeMode, ignore sampler
        if (liveFreezeMode == 2)
        {
            std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
            std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
            std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
            std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
            if (auto* fs = processor.getFrameSampler())
                fs->setFinalGrayBuffer(localDataGray);
            return;
        }
        if (liveFreezeMode == 1) return;
    }
    else if (vizSource == VisualizerMode::SAMPLER)
    {
        // SAMPLER freeze: only the sampler's own transport (NOT propagated
        // through RAW).  RAW STOP must not blank the sampler.
        //
        // FIX(routing): During recording, the live scanner data IS what is
        // being captured — the Transport UI state (STOP/HOLD) must not blank
        // or freeze the display.  Bypass all freeze gates while recording so
        // that visual and audio always reflect the same incoming stream.
        const bool isRecording = (frame_sampler_is_recording() != 0);
        if (!isRecording)
        {
            if (rawSmpFreeze == 2) // Sampler STOP → white
            {
                std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
                std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
                std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
                std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                if (auto* fs = processor.getFrameSampler())
                    fs->setFinalGrayBuffer(localDataGray);
                return;
            }
            if (rawSmpFreeze == 1) return; // Sampler HOLD → freeze
        }
        // isRecording == true: fall through and display the live scanner data
    }
    else
    {
        // Downstream views (SPCTR_*, SYNTH_*): freeze logic must match their
        // configured source (LIVE, SAMPLER, or MIX) — not the global samplerWriting
        // flag.  Using samplerWriting here caused SPCTR_GRAY (Source=LIVE) to
        // bypass the live freeze gate and switch to sampler_gamma whenever the
        // sampler started playing, making the display look as if it showed sampler
        // data.
        const bool isSpctrLocal = (vizSource == VisualizerMode::SPCTR_GRAY
                                || vizSource == VisualizerMode::SPCTR_COLOR
                                || vizSource == VisualizerMode::SPCTR_BLOB);
        const int downstreamSrcType = isSpctrLocal
                                      ? g_sp3ctra_config.luxstral_source_type
                                      : g_sp3ctra_config.luxsynth_source_type;

        if (downstreamSrcType == IMAGE_SOURCE_LIVE)
        {
            // Source=LIVE: honour live freeze, ignore sampler state entirely.
            if (liveFreezeMode == 2)
            {
                std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
                std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
                std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
                std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                if (auto* fs = processor.getFrameSampler())
                    fs->setFinalGrayBuffer(localDataGray);
                return;
            }
            if (liveFreezeMode == 1) return;
        }
        else if (downstreamSrcType == IMAGE_SOURCE_SAMPLER)
        {
            // Source=SAMPLER: honour sampler freeze, bypass during recording.
            const bool isRecording = (frame_sampler_is_recording() != 0);
            if (!isRecording)
            {
                if (rawSmpFreeze == 2)
                {
                    std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
                    std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
                    std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
                    std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                    if (auto* fs = processor.getFrameSampler())
                        fs->setFinalGrayBuffer(localDataGray);
                    return;
                }
                if (rawSmpFreeze == 1) return;
            }
        }
        else // IMAGE_SOURCE_MIX
        {
            // Source=MIX: combined logic — sampler can override the live freeze.
            if (liveFreezeMode == 2 && !samplerWriting)
            {
                std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
                std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
                std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
                std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                if (auto* fs = processor.getFrameSampler())
                    fs->setFinalGrayBuffer(localDataGray);
                return;
            }
            if (liveFreezeMode == 1 && !samplerWriting)
            {
                auto* fs_hold = processor.getFrameSampler();
                if (fs_hold && fs_hold->isSeqSilentStepActive())
                {
                    std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                    fs_hold->setFinalGrayBuffer(localDataGray);
                }
                return;
            }
        }
    }

    // ── Read from appropriate buffer ─────────────────────────────────────────
    // Source-level views read their dedicated buffer.
    // Downstream views (SPCTR_*, SYNTH_*) respect the per-path source selector
    //   so that changing the Source combo in the UI is reflected in the visualizer.
    uint8_t *pR, *pG, *pB;
    if (vizSource == VisualizerMode::RAW || vizSource == VisualizerMode::LIVE)
    {
        audio_image_buffers_get_raw_pointers(buffers, &pR, &pG, &pB);
    }
    else if (vizSource == VisualizerMode::SAMPLER)
    {
        audio_image_buffers_get_sampler_pointers(buffers, &pR, &pG, &pB);
    }
    else if (vizSource == VisualizerMode::MIX)
    {
        audio_image_buffers_get_read_pointers(buffers, &pR, &pG, &pB);
    }
    else
    {
        // Downstream views: route according to per-path source selector
        const bool isSpctr = (vizSource == VisualizerMode::SPCTR_GRAY
                           || vizSource == VisualizerMode::SPCTR_COLOR
                           || vizSource == VisualizerMode::SPCTR_BLOB);
        const int srcType = isSpctr ? g_sp3ctra_config.luxstral_source_type
                                    : g_sp3ctra_config.luxsynth_source_type;

        if (srcType == IMAGE_SOURCE_LIVE)
            audio_image_buffers_get_raw_pointers(buffers, &pR, &pG, &pB);
        else if (srcType == IMAGE_SOURCE_SAMPLER)
            audio_image_buffers_get_sampler_pointers(buffers, &pR, &pG, &pB);
        else
            audio_image_buffers_get_read_pointers(buffers, &pR, &pG, &pB);
    }
    std::memcpy(localDataR.data(), pR, cisPixelsCount);
    std::memcpy(localDataG.data(), pG, cisPixelsCount);
    std::memcpy(localDataB.data(), pB, cisPixelsCount);

    // ── Apply live opacity ───────────────────────────────────────────────────
    // Opacity controls affect ONLY the MIX bus (the blended output).
    // RAW, LIVE, and SAMPLER show their pure data without opacity adjustments.
    // For MIX: apply live opacity only when sampler is not writing
    //   (when sampler writes, opacities are already baked into the bus).
    const bool applyLiveOpacity =
        (vizSource == VisualizerMode::MIX
         && !samplerWriting && liveFreezeMode == 0);

    if (applyLiveOpacity)
    {
        const float liveOp = processor.getAPVTS()
                                 .getRawParameterValue("imageLiveOpacity")->load();
        if (liveOp < 0.999f)
        {
            const float inv = 1.0f - liveOp;
            for (int i = 0; i < cisPixelsCount; ++i)
            {
                localDataR[i] = static_cast<uint8_t>(localDataR[i] * liveOp + 255.f * inv);
                localDataG[i] = static_cast<uint8_t>(localDataG[i] * liveOp + 255.f * inv);
                localDataB[i] = static_cast<uint8_t>(localDataB[i] * liveOp + 255.f * inv);
            }
        }
    }

    // ── Compute final processed grayscale — single source of truth ───────────
    // Source-level modes (SAMPLER, LIVE, MIX) show the raw RGB → grayscale
    // conversion WITHOUT inversion, DC blocking, or gamma.
    //
    // Downstream modes (SPCTR_*, SYNTH_*) mirror the actual pipeline stages:
    //   STEP 1 — RGB → grayscale (Rec. 601 weights)
    //   STEP 2 — optional inversion (Negative)
    //   STEP 3 — optional DC blocking (AC removal — subtract per-line mean)
    //   STEP 4 — optional gamma
    {
        const bool isSourceView = (vizSource == VisualizerMode::SAMPLER
                                || vizSource == VisualizerMode::LIVE
                                || vizSource == VisualizerMode::MIX);
        const bool isSpctrView = (vizSource == VisualizerMode::SPCTR_GRAY
                               || vizSource == VisualizerMode::SPCTR_COLOR
                               || vizSource == VisualizerMode::SPCTR_BLOB);

        /* Per-path flags */
        const int doInvert = isSourceView ? 0
                           : (isSpctrView ? g_sp3ctra_config.luxstral_inversion
                                          : g_sp3ctra_config.luxsynth_inversion);
        const int doDcBlock = isSourceView ? 0
                            : (isSpctrView ? g_sp3ctra_config.luxstral_ac_removal
                                           : g_sp3ctra_config.luxsynth_ac_removal);

        /* Gamma: per-path.
         *  SPCTR_* (LUXSTRAL): switch between live and sampler gamma based on what
         *    the view is actually showing — NOT just samplerWriting.  When Source=LIVE,
         *    the sampler state must never influence the gamma (fixes visual contamination).
         *  SYNTH_*: no gamma (original behaviour). */
        float gammaVal;
        int   gammaOn;
        if (isSourceView) {
            gammaVal = 0.0f;
            gammaOn  = 0;
        } else if (isSpctrView) {
            // FIX(routing): Use sampler_gamma ONLY when the configured source actually
            // carries sampler data.  Source=LIVE must always use the live (additive)
            // gamma regardless of whether the sampler is playing.
            const bool useSamplerGamma = samplerWriting
                && (g_sp3ctra_config.luxstral_source_type != IMAGE_SOURCE_LIVE);
            gammaVal = useSamplerGamma ? g_sp3ctra_config.sampler_gamma
                                       : g_sp3ctra_config.additive_gamma_value;
            gammaOn  = useSamplerGamma ? (gammaVal > 0.0f ? 1 : 0)
                                       : g_sp3ctra_config.additive_enable_non_linear_mapping;
        } else {
            /* SYNTH_* — no gamma by design */
            gammaVal = 0.0f;
            gammaOn  = 0;
        }

        localDataGray.resize(cisPixelsCount);

        /* Intermediate float buffer for multi-pass processing */
        thread_local std::vector<float> grayF;
        grayF.resize(static_cast<size_t>(cisPixelsCount));

        /* ── Pass 1: grayscale + inversion ──────────────────────────────── */
        for (int i = 0; i < cisPixelsCount; ++i)
        {
            float gray = (0.299f * static_cast<float>(localDataR[i])
                        + 0.587f * static_cast<float>(localDataG[i])
                        + 0.114f * static_cast<float>(localDataB[i])) / 255.0f;
            if (gray < 0.0f) gray = 0.0f;
            if (gray > 1.0f) gray = 1.0f;

            // STEP 2: inversion (Negative)
            if (doInvert) gray = 1.0f - gray;

            grayF[static_cast<size_t>(i)] = gray;
        }

        /* ── Pass 2: DC blocking (subtract mean, clamp to [0,1]) ──────── */
        if (doDcBlock && cisPixelsCount > 0)
        {
            float sum = 0.0f;
            for (int i = 0; i < cisPixelsCount; ++i)
                sum += grayF[static_cast<size_t>(i)];
            float mean = sum / static_cast<float>(cisPixelsCount);
            for (int i = 0; i < cisPixelsCount; ++i)
            {
                float v = grayF[static_cast<size_t>(i)] - mean;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                grayF[static_cast<size_t>(i)] = v;
            }
        }

        /* ── Pass 3: gamma + final quantisation to uint8 ──────────────── */
        for (int i = 0; i < cisPixelsCount; ++i)
        {
            float gray = grayF[static_cast<size_t>(i)];

            // STEP 4: gamma (photo convention: pow(x, 1/gamma))
            //   gamma > 1 → brightens midtones
            //   gamma < 1 → darkens  midtones
            if (gammaOn && gammaVal > 0.0f)
                gray = std::pow(gray, 1.0f / gammaVal);

            if (gray < 0.0f) gray = 0.0f;
            if (gray > 1.0f) gray = 1.0f;

            localDataGray[static_cast<size_t>(i)] =
                static_cast<uint8_t>(gray * 255.0f + 0.5f);
        }

        // ── Sequencer STEP_EMPTY: override computed result with full white ────
        // When the sequencer triggers a silence step, the CIS passthrough is
        // disabled (passthroughEnabled=false) but AudioImageBuffers still holds
        // the last frame.  We must force white here so that both the display
        // and BlobVisualizerComponent see silence.
        auto* fs_ = processor.getFrameSampler();
        if (fs_ && fs_->isSeqSilentStepActive())
            std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});

        // ── Publish the mix-final gray buffer ─────────────────────────────────
        // BlobVisualizerComponent reads this via getFinalGrayBuffer() so that
        // it always operates on the same image that is displayed to the user.
        if (fs_)
            fs_->setFinalGrayBuffer(localDataGray);
    }
}

//==============================================================================
void CisVisualizerComponent::paintColorTemperatureMode(
    juce::Graphics& g, int W, int H) const
{
    g.fillAll(juce::Colour(0xff1a1a1a));

    if (localDataR.empty() || W < 2 || H < 2) return;

    // ── Color temperature waveform ───────────────────────────────────────────
    //
    // One continuous curve: Y = color_temperature(pixel)
    //   temperature = (R − B) / 255   →  range [-1, +1]
    //
    //   +1 (warm) = top of display   → red area above centre
    //    0        = centre line       → gray / white / black surfaces
    //   −1 (cold) = bottom of display → blue area below centre
    //
    // Every pixel is drawn — NO skipping.  Gray/white/black produce R ≈ B
    // so temp ≈ 0 → flat line at centre → no visible area.

    const int   centerY = H / 2;
    const float halfH   = static_cast<float>(H) / 2.0f;

    // ── Spatial smoothing (box average ±8 px) ────────────────────────────────
    // Eliminates per-photosite fixed-pattern noise on R/B channels.
    constexpr int kSmoothRadius = 8;

    thread_local std::vector<float> rawR, rawB, tempCurve;
    rawR.resize(static_cast<size_t>(W));
    rawB.resize(static_cast<size_t>(W));
    tempCurve.resize(static_cast<size_t>(W));

    // Sample raw CIS R and B at display resolution
    for (int x = 0; x < W; ++x)
    {
        rawR[static_cast<size_t>(x)] = static_cast<float>(
            interpolateCisPixel(localDataR.data(), x, W));
        rawB[static_cast<size_t>(x)] = static_cast<float>(
            interpolateCisPixel(localDataB.data(), x, W));
    }

    // Compute smoothed temperature curve (before DC removal)
    constexpr float kTempGain = 8.0f;
    for (int x = 0; x < W; ++x)
    {
        const int lo = std::max(0, x - kSmoothRadius);
        const int hi = std::min(W - 1, x + kSmoothRadius);
        const float n = static_cast<float>(hi - lo + 1);
        float sumR = 0.0f, sumB = 0.0f;
        for (int i = lo; i <= hi; ++i)
        {
            sumR += rawR[static_cast<size_t>(i)];
            sumB += rawB[static_cast<size_t>(i)];
        }
        const float avgR = sumR / n;
        const float avgB = sumB / n;
        // Raw temperature before gain — stored unclamped for DC removal
        tempCurve[static_cast<size_t>(x)] = (avgR - avgB) / 255.0f;
    }

    // ── Auto-zero: subtract the global mean (R-B) ───────────────────────────
    // The CIS sensor has a fixed warm bias (R > B globally).  Subtracting
    // the per-frame mean centres the curve around 0 so that only local
    // colour temperature *variations* are shown.
    {
        float meanTemp = 0.0f;
        for (int x = 0; x < W; ++x)
            meanTemp += tempCurve[static_cast<size_t>(x)];
        meanTemp /= static_cast<float>(W);

        for (int x = 0; x < W; ++x)
        {
            float temp = (tempCurve[static_cast<size_t>(x)] - meanTemp) * kTempGain;
            if (temp < -1.0f) temp = -1.0f;
            if (temp >  1.0f) temp =  1.0f;
            tempCurve[static_cast<size_t>(x)] = temp;
        }
    }

    // ── Draw filled area between centre and curve ────────────────────────────
    const juce::Colour warmCol(0xffcc4444); // red
    const juce::Colour coldCol(0xff4466cc); // blue

    for (int x = 0; x < W; ++x)
    {
        const float temp = tempCurve[static_cast<size_t>(x)];

        // Pixel height from centre (can be fractional, round to int)
        const int barH = static_cast<int>(std::abs(temp) * halfH + 0.5f);

        if (barH < 1) continue; // temp ≈ 0 → nothing to draw (flat line)

        if (temp > 0.0f)
        {
            // Warm → fill red area from centre upward
            g.setColour(warmCol);
            g.fillRect(x, centerY - barH, 1, barH);
        }
        else
        {
            // Cold → fill blue area from centre downward
            g.setColour(coldCol);
            g.fillRect(x, centerY, 1, barH);
        }
    }

    // ── Centre line (reference) ──────────────────────────────────────────────
    g.setColour(juce::Colour(0x50ffffff));
    g.fillRect(0, centerY, W, 1);
}

//==============================================================================
bool CisVisualizerComponent::isColorSource(VisualizerMode m) const noexcept
{
    // Note: SYNTH_FFT_COLOR is intercepted before this call in paint() and
    // handled by its own dedicated renderer — do NOT include it here.
    return m == VisualizerMode::SPCTR_COLOR
        || m == VisualizerMode::SYNTH_COLOR;
}

//==============================================================================
bool CisVisualizerComponent::supportsDisplayModes(VisualizerMode m) const noexcept
{
    // Display mode switching (Image / Waveform / Inverted) is only relevant
    // for RAW data, Live, Sampler, Mix, and grayscale pipeline views.
    // COLOR, BLOB, and FFT modes have their own dedicated renderers and are
    // intercepted in paint() before this function is ever consulted.
    switch (m)
    {
        case VisualizerMode::RAW:
        case VisualizerMode::LIVE:
        case VisualizerMode::SAMPLER:
        case VisualizerMode::MIX:
        case VisualizerMode::SPCTR_GRAY:
        case VisualizerMode::SYNTH_GRAY:
            return true;
        default:
            return false;
    }
}

//==============================================================================
void CisVisualizerComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        showDisplayModeMenu();
        return;
    }

    // Default left-click behaviour
    juce::Component::mouseDown(event);
}

//==============================================================================
void CisVisualizerComponent::showDisplayModeMenu()
{
    const auto source = getActiveSource();

    juce::PopupMenu menu;

    if (supportsDisplayModes(source))
    {
        const int currentMode = static_cast<int>(
            processor.getAPVTS().getRawParameterValue("visualizerMode")->load());

        menu.addItem(1, "Image",
                     true, currentMode == 0);
        menu.addItem(2, "Waveform",
                     true, currentMode == 1);
        menu.addItem(3, "Inverted Waveform",
                     true, currentMode == 2);
    }
    else
    {
        // COLOR / BLOB sources — no alternative display modes available
        menu.addItem(-1, "Color Temperature (fixed)", false, true);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
        [this](int result)
        {
            if (result >= 1 && result <= 3)
            {
                if (auto* param = processor.getAPVTS().getParameter("visualizerMode"))
                {
                    // Parameter range is [0, 2], ComboBox items map 1→0, 2→1, 3→2
                    const float normalised = param->convertTo0to1(
                        static_cast<float>(result - 1));
                    param->setValueNotifyingHost(normalised);
                }
            }
        });
}

//==============================================================================
uint8_t CisVisualizerComponent::interpolateCisPixel(
    const uint8_t* buffer, int displayX, int displayWidth) const
{
    if (cisPixelsCount == 0 || displayWidth == 0) return 0;

    const float pos  = static_cast<float>(displayX) * (cisPixelsCount - 1)
                     / static_cast<float>(displayWidth - 1);
    const int   idx  = static_cast<int>(pos);
    const float frac = pos - idx;

    if (idx + 1 < cisPixelsCount)
        return static_cast<uint8_t>(buffer[idx] * (1.f - frac) + buffer[idx+1] * frac);

    return buffer[idx];
}

//==============================================================================
// SYNTH_BLOB — blob detection (color + continuity, max 88 blobs)
//==============================================================================
void CisVisualizerComponent::detectSynthBlobs()
{
    synthBlobs_.clear();

    if (localDataGray.empty() || cisPixelsCount == 0
        || localDataR.empty() || localDataB.empty())
        return;

    extern sp3ctra_config_t g_sp3ctra_config;
    // Use LuxSynth-dedicated blob params — fully isolated from StrokeForge/LuxStral.
    // Configured via lxBlob* APVTS params in the LUXSYNTH tab → BLOB DETECTION section.
    const float threshold     = g_sp3ctra_config.luxsynth_blob_threshold;
    const int   minWidth      = juce::jmax(1, g_sp3ctra_config.luxsynth_blob_min_width);
    const int   mergeGap      = juce::jmax(0, g_sp3ctra_config.luxsynth_blob_merge_gap);
    // Color Split parameter [0..1]:
    //   0% → no color-based split (color completely ignored, pure gap merge)
    //   100% → maximum split (any color divergence breaks a blob, including
    //           within continuous active regions — independent of Merge Gap)
    // Internally converted to a merge threshold:
    //   colorMergeThr = 1 - colorSplitParam
    //   dist > colorMergeThr → split
    const float colorSplitParam = juce::jlimit(0.0f, 1.0f,
                                      g_sp3ctra_config.luxsynth_blob_color_split);
    const float colorMergeThr   = 1.0f - colorSplitParam;

    // ── Pre-compute locally smoothed normalised RGB per CIS pixel ─────────────
    // Box average ±kSmoothRadius represents the "local color identity" of each
    // pixel, independent of single-photosite noise. Used to decide whether two
    // active regions separated by an inactive gap should be merged (same color
    // neighbourhood) or kept separate (different color neighbourhood).
    constexpr int kSmoothRadius = 8;

    thread_local std::vector<float> smR, smG, smB;
    smR.resize(static_cast<size_t>(cisPixelsCount));
    smG.resize(static_cast<size_t>(cisPixelsCount));
    smB.resize(static_cast<size_t>(cisPixelsCount));

    for (int i = 0; i < cisPixelsCount; ++i)
    {
        const int   lo = std::max(0, i - kSmoothRadius);
        const int   hi = std::min(cisPixelsCount - 1, i + kSmoothRadius);
        const float n  = static_cast<float>(hi - lo + 1);
        float sr = 0.f, sg = 0.f, sb = 0.f;
        for (int k = lo; k <= hi; ++k)
        {
            sr += static_cast<float>(localDataR[k]);
            sg += static_cast<float>(localDataG[k]);
            sb += static_cast<float>(localDataB[k]);
        }
        smR[static_cast<size_t>(i)] = sr / (n * 255.f);
        smG[static_cast<size_t>(i)] = sg / (n * 255.f);
        smB[static_cast<size_t>(i)] = sb / (n * 255.f);
    }

    // ── 1-D scan — gap-based merge with colorimetric proximity check ──────────
    //
    // Logic:
    //  - Active pixel (act ≥ threshold): extend or start blob
    //  - Inactive pixel: count gap; if gap > mergeGap → close blob
    //  - On gap resume (active after inactive, gap ≤ mergeGap):
    //      compute Euclidean RGB distance between new pixel's local color and
    //      running blob's accumulated mean local color.
    //      dist ≤ colorMergeThr → merge (continue blob)
    //      dist >  colorMergeThr → don't merge (close and start new blob)
    //
    bool  inBlob    = false;
    int   blobStart = 0;
    int   gapCount  = 0;
    float blobPeak  = 0.f;
    float blobSum   = 0.f;
    int   blobLen   = 0;
    float blobRSum  = 0.f, blobGSum = 0.f, blobBSum = 0.f;

    auto startNewBlob = [&](int px)
    {
        blobStart = px;
        inBlob    = true;
        blobPeak  = localDataGray[px] / 255.f;
        blobSum   = blobPeak;
        blobLen   = 1;
        blobRSum  = smR[static_cast<size_t>(px)];
        blobGSum  = smG[static_cast<size_t>(px)];
        blobBSum  = smB[static_cast<size_t>(px)];
        gapCount  = 0;
    };

    auto finishBlob = [&](int endPx)
    {
        const int width = endPx - blobStart;
        if (width >= minWidth && blobLen > 0
            && static_cast<int>(synthBlobs_.size()) < kMaxSynthBlobs)
        {
            SynthBlob b;
            b.startPx       = blobStart;
            b.endPx         = endPx;
            b.peakIntensity = blobPeak;
            b.avgIntensity  = blobSum / static_cast<float>(blobLen);
            const float mr  = juce::jlimit(0.f, 1.f, blobRSum / static_cast<float>(blobLen));
            const float mg  = juce::jlimit(0.f, 1.f, blobGSum / static_cast<float>(blobLen));
            const float mb  = juce::jlimit(0.f, 1.f, blobBSum / static_cast<float>(blobLen));
            b.avgColorTemp  = mr - mb; // warm/cool approximation for tooltip
            b.avgLocalColor = juce::Colour(static_cast<uint8_t>(mr * 255.f),
                                           static_cast<uint8_t>(mg * 255.f),
                                           static_cast<uint8_t>(mb * 255.f));
            b.color         = juce::Colours::white; // hue assigned after scan
            synthBlobs_.push_back(b);
        }
        inBlob   = false;
        gapCount = 0;
        blobPeak = blobSum = 0.f;
        blobLen  = 0;
        blobRSum = blobGSum = blobBSum = 0.f;
    };

    for (int i = 0; i < cisPixelsCount; ++i)
    {
        const float act    = localDataGray[i] / 255.f;
        const bool  active = (act >= threshold);

        if (active)
        {
            if (!inBlob)
            {
                startNewBlob(i);
            }
            else if (gapCount > 0)
            {
                // ── Gap resume: color proximity check ────────────────────────
                // gapCount ≤ mergeGap (otherwise blob was already closed).
                // Decide whether to merge based on colorimetric distance.
                const float meanR = blobRSum / static_cast<float>(blobLen);
                const float meanG = blobGSum / static_cast<float>(blobLen);
                const float meanB = blobBSum / static_cast<float>(blobLen);
                const float dr    = smR[static_cast<size_t>(i)] - meanR;
                const float dg    = smG[static_cast<size_t>(i)] - meanG;
                const float db    = smB[static_cast<size_t>(i)] - meanB;
                // Euclidean distance in normalised RGB, range [0..sqrt(3)] → /sqrt(3) → [0..1]
                const float dist  = std::sqrt(dr*dr + dg*dg + db*db) * 0.5774f;

                if (dist > colorMergeThr
                    && static_cast<int>(synthBlobs_.size()) < kMaxSynthBlobs - 1)
                {
                    // Colors too different → close running blob, start a new one
                    finishBlob(i - gapCount);
                    startNewBlob(i);
                }
                else
                {
                    // Colors close → merge: continue extending the blob
                    if (act > blobPeak) blobPeak = act;
                    blobSum  += act;
                    blobLen++;
                    blobRSum += smR[static_cast<size_t>(i)];
                    blobGSum += smG[static_cast<size_t>(i)];
                    blobBSum += smB[static_cast<size_t>(i)];
                    gapCount  = 0;
                }
            }
            else
            {
                // ── Within continuous active region: color split check ────────
                // Fires even with no gap — independent of Merge Gap.
                // Only active when colorSplitParam > 0 (i.e. user wants splitting).
                // At 0% the threshold = 1.0 so dist never exceeds it → no split.
                if (colorSplitParam > 0.001f && blobLen > 0)
                {
                    const float meanR = blobRSum / static_cast<float>(blobLen);
                    const float meanG = blobGSum / static_cast<float>(blobLen);
                    const float meanB = blobBSum / static_cast<float>(blobLen);
                    const float dr    = smR[static_cast<size_t>(i)] - meanR;
                    const float dg    = smG[static_cast<size_t>(i)] - meanG;
                    const float db    = smB[static_cast<size_t>(i)] - meanB;
                    const float dist  = std::sqrt(dr*dr + dg*dg + db*db) * 0.5774f;

                    if (dist > colorMergeThr
                        && static_cast<int>(synthBlobs_.size()) < kMaxSynthBlobs - 1)
                    {
                        // Color divergence → close current blob, start new one at i.
                        // startNewBlob already initialises blobLen=1 with pixel i.
                        finishBlob(i);
                        startNewBlob(i);
                        continue; // pixel i already consumed by startNewBlob
                    }
                }
                // Extend current blob
                if (act > blobPeak) blobPeak = act;
                blobSum  += act;
                blobLen++;
                blobRSum += smR[static_cast<size_t>(i)];
                blobGSum += smG[static_cast<size_t>(i)];
                blobBSum += smB[static_cast<size_t>(i)];
            }
        }
        else // inactive pixel
        {
            if (inBlob)
            {
                ++gapCount;
                if (gapCount > mergeGap)
                    finishBlob(i - gapCount);
            }
        }
    }

    if (inBlob)
        finishBlob(cisPixelsCount - gapCount);

    // ── Assign unique hues — evenly spaced on HSV wheel ───────────────────────
    const int nb = static_cast<int>(synthBlobs_.size());
    for (int b = 0; b < nb; ++b)
    {
        const float hue = (nb > 1) ? static_cast<float>(b) / static_cast<float>(nb)
                                   : 0.0f;
        synthBlobs_[b].color = juce::Colour::fromHSV(hue, 0.85f, 0.90f, 1.0f);
    }
}

//==============================================================================
// SYNTH_BLOB — full coloured visualizer
//==============================================================================
void CisVisualizerComponent::paintSynthBlobMode(juce::Graphics& g, int W, int H)
{
    // ── Background ────────────────────────────────────────────────────────────
    g.fillAll(juce::Colour(0xff080808));

    if (cisPixelsCount == 0 || localDataGray.empty())
        return;

    // ── Run blob detection (30 fps, O(N) 1-D scan — fast) ────────────────────
    detectSynthBlobs();

    // ── Build pixel-to-blob-index lookup (thread-local to avoid alloc) ────────
    thread_local std::vector<int> pixelBlobIdx;
    pixelBlobIdx.assign(cisPixelsCount, -1);
    for (int b = 0; b < static_cast<int>(synthBlobs_.size()); ++b)
    {
        const auto& blob = synthBlobs_[b];
        for (int i = blob.startPx; i < blob.endPx && i < cisPixelsCount; ++i)
            pixelBlobIdx[i] = b;
    }

    // ── Render column by column ───────────────────────────────────────────────
    // For each display column we find the corresponding CIS pixel and decide
    // whether it belongs to a blob or is background.
    //
    // Blob columns: bottom-anchored waveform bar (height ∝ activity), coloured.
    // Background  : very dim grayscale texture (shows the raw signal level).
    for (int x = 0; x < W; ++x)
    {
        const float pos = static_cast<float>(x) / static_cast<float>(juce::jmax(1, W - 1));
        const int   ci  = juce::jlimit(0, cisPixelsCount - 1,
                              static_cast<int>(pos * static_cast<float>(cisPixelsCount - 1) + 0.5f));

        const float act  = localDataGray[ci] / 255.0f;
        const int   bIdx = pixelBlobIdx[ci];

        if (bIdx < 0)
        {
            // Background — very dim grayscale texture
            const uint8_t v = static_cast<uint8_t>(act * 22.0f);
            g.setColour(juce::Colour(v, v, v));
            g.fillRect(x, 0, 1, H);
        }
        else
        {
            const auto& blob = synthBlobs_[bIdx];

            // Bottom-anchored waveform bar
            const int barH = juce::jmax(1, static_cast<int>(act * static_cast<float>(H)));

            // Upper (inactive) region — dim blob colour
            g.setColour(blob.color.withAlpha(0.10f));
            g.fillRect(x, 0, 1, H - barH);

            // Lower (active) waveform — bright blob colour
            const float alpha = 0.40f + 0.60f * act;
            g.setColour(blob.color.withAlpha(alpha));
            g.fillRect(x, H - barH, 1, barH);
        }
    }

    // ── Draw blob outlines, peak markers, and labels ──────────────────────────
    const float cisScale = static_cast<float>(W - 1)
                         / static_cast<float>(juce::jmax(1, cisPixelsCount - 1));

    for (int b = 0; b < static_cast<int>(synthBlobs_.size()); ++b)
    {
        const auto& blob = synthBlobs_[b];
        const int x0 = static_cast<int>(static_cast<float>(blob.startPx)  * cisScale);
        const int x1 = static_cast<int>(static_cast<float>(blob.endPx - 1) * cisScale);
        const int bw = juce::jmax(1, x1 - x0 + 1);

        // Blob bounding-box outline
        g.setColour(blob.color.withAlpha(0.85f));
        g.drawRect(x0, 0, bw, H, 1);

        // Peak-intensity horizontal line (bottom-anchored)
        {
            const int peakH  = juce::jmax(2, static_cast<int>(blob.peakIntensity * static_cast<float>(H)));
            const int lineY  = H - peakH;
            g.setColour(blob.color.brighter(0.4f));
            g.fillRect(x0, lineY, bw, 2);
        }

        // ── Width indicator bar (bottom of component, 3 px) ──────────────────
        // Fills the full width of the blob at the very bottom — quick visual
        // for comparing relative blob widths.
        g.setColour(blob.color.withAlpha(0.60f));
        g.fillRect(x0, H - 3, bw, 3);
    }

    // ── Summary badge: "N/88 blobs" + color key ───────────────────────────────
    {
        const int nb = static_cast<int>(synthBlobs_.size());
        const juce::String badge =
            juce::String(nb) + "/" + juce::String(kMaxSynthBlobs) + " blobs";

        constexpr float bw = 100.f, bh = 16.f;
        const float bx = static_cast<float>(W) - bw - 4.f;
        constexpr float by = 4.f;

        g.setColour(juce::Colour(0xb0000000));
        g.fillRoundedRectangle(bx, by, bw, bh, 3.f);
        g.setColour(juce::Colour(0xffd07040)); // SYNTH_BLOB accent
        g.drawRoundedRectangle(bx, by, bw, bh, 3.f, 1.f);

        g.setColour(juce::Colours::white.withAlpha(0.90f));
        g.setFont(juce::FontOptions(9.f));
        g.drawText(badge,
                   static_cast<int>(bx), static_cast<int>(by),
                   static_cast<int>(bw), static_cast<int>(bh),
                   juce::Justification::centred, false);
    }

    // ── Hover tooltip — drawn last so it always appears on top ────────────────
    // Shown when the mouse is over a blob in SYNTH_BLOB mode.
    // hoverBlobIdx_ is kept up-to-date by mouseMove() / mouseExit().
    if (hoverBlobIdx_ >= 0 && hoverBlobIdx_ < static_cast<int>(synthBlobs_.size()))
    {
        const auto& blob = synthBlobs_[hoverBlobIdx_];

        // ── Build info lines ────────────────────────────────────────────────
        const juce::String line1 =
            "Blob #" + juce::String(hoverBlobIdx_ + 1);
        const juce::String line2 =
            "Width:  " + juce::String(blob.endPx - blob.startPx) + " px";
        const juce::String line3 =
            "Peak:   " + juce::String(static_cast<int>(blob.peakIntensity * 100.f)) + "%"
            + "  avg: " + juce::String(static_cast<int>(blob.avgIntensity * 100.f)) + "%";
        const juce::String tempStr =
            (blob.avgColorTemp >  0.08f) ? "Warm" :
            (blob.avgColorTemp < -0.08f) ? "Cool" : "Neutral";
        const juce::String line4 = "Temp:   " + tempStr;

        // ── Tooltip box geometry ─────────────────────────────────────────────
        constexpr float kTW = 148.f, kTH = 70.f, kTR = 4.f;
        float tx = static_cast<float>(hoverPos_.x) + 14.f;
        float ty = static_cast<float>(hoverPos_.y) - kTH * 0.5f;

        // Clamp inside component bounds
        if (tx + kTW > static_cast<float>(W)) tx = static_cast<float>(hoverPos_.x) - kTW - 10.f;
        if (ty < 2.f)                          ty = 2.f;
        if (ty + kTH > static_cast<float>(H)) ty = static_cast<float>(H) - kTH - 2.f;

        // Background + border
        g.setColour(juce::Colour(0xee0d0d0d));
        g.fillRoundedRectangle(tx, ty, kTW, kTH, kTR);
        g.setColour(blob.color.withAlpha(0.90f));
        g.drawRoundedRectangle(tx, ty, kTW, kTH, kTR, 1.2f);

        // ── Text ─────────────────────────────────────────────────────────────
        const auto ti = [&](int lineIdx) {
            return static_cast<int>(ty + 4.f + static_cast<float>(lineIdx) * 16.f);
        };
        const int lw = static_cast<int>(kTW) - 8;
        const int lx = static_cast<int>(tx) + 4;

        g.setFont(juce::FontOptions(9.5f));
        g.setColour(blob.color.brighter(0.25f));
        g.drawText(line1, lx, ti(0), lw, 14, juce::Justification::centredLeft, false);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(8.5f));
        g.drawText(line2, lx, ti(1), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(line3, lx, ti(2), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(line4, lx, ti(3), lw, 13, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
// SPCTR_BLOB — blob detection (LuxStral path, gap-based, no color split)
// Uses StrokeForge configuration parameters, isolated from LuxSynth/SYNTH_BLOB.
//==============================================================================
void CisVisualizerComponent::detectSpctrBlobs()
{
    spctrBlobs_.clear();

    if (localDataGray.empty() || cisPixelsCount == 0) return;

    extern sp3ctra_config_t g_sp3ctra_config;
    // Use StrokeForge-dedicated blob params — fully isolated from LuxSynth path.
    // Read from spctrBlob* APVTS params — identical ranges as lxBlob* (IMAGE LUXSYNTH).
    // These are UI thread reads; detectSpctrBlobs() is called from paint() (message thread).
    auto& apvts_ = processor.getAPVTS();
    const float threshold = apvts_.getRawParameterValue("spctrBlobThreshold")->load();
    const int   minWidth  = juce::jmax(1, static_cast<int>(
                                apvts_.getRawParameterValue("spctrBlobMinWidth")->load()));
    const int   mergeGap  = juce::jmax(0, static_cast<int>(
                                apvts_.getRawParameterValue("spctrBlobMergeGap")->load()));
    // Internally converted to a merge threshold (same formula as detectSynthBlobs):
    //   colorMergeThr = 1 - colorSplitParam
    //   dist > colorMergeThr → split
    // Note: spctrBlobColorSplit ≥ 0 → colorMergeThr ≤ 1.0.
    // In grayscale mode (LuxStral pipeline) dist ≡ 0 so split is a no-op;
    // the parameter is active when the upstream source carries real colour.
    const float colorSplitParam = juce::jlimit(0.0f, 1.0f,
                                      apvts_.getRawParameterValue("spctrBlobColorSplit")->load());
    const float colorMergeThr   = 1.0f - colorSplitParam;

    // ── Pre-compute locally smoothed RGB (used for avgLocalColor in tooltip) ───
    constexpr int kSmoothRadius = 8;
    thread_local std::vector<float> smR, smG, smB;
    smR.resize(static_cast<size_t>(cisPixelsCount));
    smG.resize(static_cast<size_t>(cisPixelsCount));
    smB.resize(static_cast<size_t>(cisPixelsCount));

    for (int i = 0; i < cisPixelsCount; ++i)
    {
        const int   lo = std::max(0, i - kSmoothRadius);
        const int   hi = std::min(cisPixelsCount - 1, i + kSmoothRadius);
        const float n  = static_cast<float>(hi - lo + 1);
        float sr = 0.f, sg = 0.f, sb = 0.f;
        for (int k = lo; k <= hi; ++k)
        {
            sr += static_cast<float>(localDataR[k]);
            sg += static_cast<float>(localDataG[k]);
            sb += static_cast<float>(localDataB[k]);
        }
        smR[static_cast<size_t>(i)] = sr / (n * 255.f);
        smG[static_cast<size_t>(i)] = sg / (n * 255.f);
        smB[static_cast<size_t>(i)] = sb / (n * 255.f);
    }

    // ── 1-D scan — gap + color-split merge (same algorithm as detectSynthBlobs) ──
    bool  inBlob   = false;
    int   blobStart = 0;
    int   gapCount  = 0;
    float blobPeak  = 0.f;
    float blobSum   = 0.f;
    int   blobLen   = 0;
    float blobRSum  = 0.f, blobGSum = 0.f, blobBSum = 0.f;

    auto startNew = [&](int px)
    {
        blobStart = px;   inBlob   = true;
        blobPeak  = localDataGray[px] / 255.f;
        blobSum   = blobPeak;  blobLen  = 1;
        blobRSum  = smR[static_cast<size_t>(px)];
        blobGSum  = smG[static_cast<size_t>(px)];
        blobBSum  = smB[static_cast<size_t>(px)];
        gapCount  = 0;
    };

    auto finishBlob = [&](int endPx)
    {
        const int width = endPx - blobStart;
        if (width >= minWidth && blobLen > 0
            && static_cast<int>(spctrBlobs_.size()) < kMaxSynthBlobs)
        {
            SynthBlob b;
            b.startPx       = blobStart;
            b.endPx         = endPx;
            b.peakIntensity = blobPeak;
            b.avgIntensity  = blobSum / static_cast<float>(blobLen);
            const float mr  = juce::jlimit(0.f, 1.f, blobRSum / static_cast<float>(blobLen));
            const float mg  = juce::jlimit(0.f, 1.f, blobGSum / static_cast<float>(blobLen));
            const float mb  = juce::jlimit(0.f, 1.f, blobBSum / static_cast<float>(blobLen));
            b.avgColorTemp  = mr - mb;
            b.avgLocalColor = juce::Colour(static_cast<uint8_t>(mr * 255.f),
                                           static_cast<uint8_t>(mg * 255.f),
                                           static_cast<uint8_t>(mb * 255.f));
            b.color         = juce::Colours::white; // hue assigned after scan
            spctrBlobs_.push_back(b);
        }
        inBlob   = false;  gapCount = 0;
        blobPeak = blobSum = 0.f;  blobLen = 0;
        blobRSum = blobGSum = blobBSum = 0.f;
    };

    for (int i = 0; i < cisPixelsCount; ++i)
    {
        const float act    = localDataGray[i] / 255.f;
        const bool  active = (act >= threshold);

        if (active)
        {
            if (!inBlob)
            {
                startNew(i);
            }
            else if (gapCount > 0)
            {
                // ── Gap resume: color proximity check (identical to detectSynthBlobs) ──
                // Compute Euclidean RGB distance between current pixel's local color
                // and the running blob's accumulated mean local color.
                // In grayscale mode smR≡smG≡smB → dist ≡ 0 → always merges.
                const float meanR = blobRSum / static_cast<float>(blobLen);
                const float meanG = blobGSum / static_cast<float>(blobLen);
                const float meanB = blobBSum / static_cast<float>(blobLen);
                const float dr    = smR[static_cast<size_t>(i)] - meanR;
                const float dg    = smG[static_cast<size_t>(i)] - meanG;
                const float db    = smB[static_cast<size_t>(i)] - meanB;
                const float dist  = std::sqrt(dr*dr + dg*dg + db*db) * 0.5774f;

                if (dist > colorMergeThr
                    && static_cast<int>(spctrBlobs_.size()) < kMaxSynthBlobs - 1)
                {
                    // Colors diverge → close current blob, start fresh from i
                    finishBlob(i - gapCount);
                    startNew(i);
                }
                else
                {
                    // Colors close → merge: continue extending the blob
                    if (act > blobPeak) blobPeak = act;
                    blobSum  += act;  blobLen++;
                    blobRSum += smR[static_cast<size_t>(i)];
                    blobGSum += smG[static_cast<size_t>(i)];
                    blobBSum += smB[static_cast<size_t>(i)];
                    gapCount  = 0;
                }
            }
            else
            {
                // Extend current blob
                if (act > blobPeak) blobPeak = act;
                blobSum  += act;  blobLen++;
                blobRSum += smR[static_cast<size_t>(i)];
                blobGSum += smG[static_cast<size_t>(i)];
                blobBSum += smB[static_cast<size_t>(i)];
            }
        }
        else
        {
            if (inBlob)
            {
                ++gapCount;
                if (gapCount > mergeGap)
                    finishBlob(i - gapCount);
            }
        }
    }
    if (inBlob) finishBlob(cisPixelsCount - gapCount);

    // ── Assign unique hues — evenly spaced on HSV wheel ──────────────────────
    const int nb = static_cast<int>(spctrBlobs_.size());
    for (int b = 0; b < nb; ++b)
    {
        const float hue = (nb > 1) ? static_cast<float>(b) / static_cast<float>(nb)
                                   : 0.0f;
        spctrBlobs_[b].color = juce::Colour::fromHSV(hue, 0.85f, 0.90f, 1.0f);
    }
}

//==============================================================================
// SPCTR_BLOB — full coloured visualizer (mirrors paintSynthBlobMode)
// Reads from spctrBlobs_; uses LuxStral accent colour (0xff8888e0).
//==============================================================================
void CisVisualizerComponent::paintSpctrBlobMode(juce::Graphics& g, int W, int H)
{
    g.fillAll(juce::Colour(0xff080808));
    if (cisPixelsCount == 0 || localDataGray.empty()) return;

    detectSpctrBlobs();

    thread_local std::vector<int> pixelBlobIdx;
    pixelBlobIdx.assign(cisPixelsCount, -1);
    for (int b = 0; b < static_cast<int>(spctrBlobs_.size()); ++b)
    {
        const auto& blob = spctrBlobs_[b];
        for (int i = blob.startPx; i < blob.endPx && i < cisPixelsCount; ++i)
            pixelBlobIdx[i] = b;
    }

    for (int x = 0; x < W; ++x)
    {
        const float pos = static_cast<float>(x) / static_cast<float>(juce::jmax(1, W - 1));
        const int   ci  = juce::jlimit(0, cisPixelsCount - 1,
                              static_cast<int>(pos * static_cast<float>(cisPixelsCount - 1) + 0.5f));
        const float act  = localDataGray[ci] / 255.0f;
        const int   bIdx = pixelBlobIdx[ci];

        if (bIdx < 0)
        {
            const uint8_t v = static_cast<uint8_t>(act * 22.0f);
            g.setColour(juce::Colour(v, v, v));
            g.fillRect(x, 0, 1, H);
        }
        else
        {
            const auto& blob = spctrBlobs_[bIdx];
            const int barH = juce::jmax(1, static_cast<int>(act * static_cast<float>(H)));
            g.setColour(blob.color.withAlpha(0.10f));
            g.fillRect(x, 0, 1, H - barH);
            g.setColour(blob.color.withAlpha(0.40f + 0.60f * act));
            g.fillRect(x, H - barH, 1, barH);
        }
    }

    const float cisScale = static_cast<float>(W - 1)
                         / static_cast<float>(juce::jmax(1, cisPixelsCount - 1));

    for (int b = 0; b < static_cast<int>(spctrBlobs_.size()); ++b)
    {
        const auto& blob = spctrBlobs_[b];
        const int x0 = static_cast<int>(static_cast<float>(blob.startPx)  * cisScale);
        const int x1 = static_cast<int>(static_cast<float>(blob.endPx - 1) * cisScale);
        const int bw = juce::jmax(1, x1 - x0 + 1);

        g.setColour(blob.color.withAlpha(0.85f));
        g.drawRect(x0, 0, bw, H, 1);

        {
            const int peakH = juce::jmax(2, static_cast<int>(blob.peakIntensity * static_cast<float>(H)));
            g.setColour(blob.color.brighter(0.4f));
            g.fillRect(x0, H - peakH, bw, 2);
        }

        g.setColour(blob.color.withAlpha(0.60f));
        g.fillRect(x0, H - 3, bw, 3);
    }

    // ── Summary badge: "N/88 blobs" — LuxStral accent ────────────────────────
    {
        const int nb = static_cast<int>(spctrBlobs_.size());
        const juce::String badge =
            juce::String(nb) + "/" + juce::String(kMaxSynthBlobs) + " blobs";

        constexpr float bw = 100.f, bh = 16.f;
        const float bx = static_cast<float>(W) - bw - 4.f;
        constexpr float by = 4.f;

        g.setColour(juce::Colour(0xb0000000));
        g.fillRoundedRectangle(bx, by, bw, bh, 3.f);
        g.setColour(juce::Colour(0xff8888e0)); // SPCTR_BLOB accent
        g.drawRoundedRectangle(bx, by, bw, bh, 3.f, 1.f);
        g.setColour(juce::Colours::white.withAlpha(0.90f));
        g.setFont(juce::FontOptions(9.f));
        g.drawText(badge,
                   static_cast<int>(bx), static_cast<int>(by),
                   static_cast<int>(bw), static_cast<int>(bh),
                   juce::Justification::centred, false);
    }

    // ── Hover tooltip ─────────────────────────────────────────────────────────
    if (hoverBlobIdx_ >= 0 && hoverBlobIdx_ < static_cast<int>(spctrBlobs_.size()))
    {
        const auto& blob = spctrBlobs_[hoverBlobIdx_];

        const juce::String line1 = "Blob #" + juce::String(hoverBlobIdx_ + 1);
        const juce::String line2 = "Width:  " + juce::String(blob.endPx - blob.startPx) + " px";
        const juce::String line3 =
            "Peak:   " + juce::String(static_cast<int>(blob.peakIntensity * 100.f)) + "%"
            + "  avg: " + juce::String(static_cast<int>(blob.avgIntensity * 100.f)) + "%";
        const juce::String tempStr =
            (blob.avgColorTemp >  0.08f) ? "Warm" :
            (blob.avgColorTemp < -0.08f) ? "Cool" : "Neutral";
        const juce::String line4 = "Temp:   " + tempStr;

        constexpr float kTW = 148.f, kTH = 70.f, kTR = 4.f;
        float tx = static_cast<float>(hoverPos_.x) + 14.f;
        float ty = static_cast<float>(hoverPos_.y) - kTH * 0.5f;
        if (tx + kTW > static_cast<float>(W)) tx = static_cast<float>(hoverPos_.x) - kTW - 10.f;
        if (ty < 2.f)                          ty = 2.f;
        if (ty + kTH > static_cast<float>(H)) ty = static_cast<float>(H) - kTH - 2.f;

        g.setColour(juce::Colour(0xee0d0d0d));
        g.fillRoundedRectangle(tx, ty, kTW, kTH, kTR);
        g.setColour(blob.color.withAlpha(0.90f));
        g.drawRoundedRectangle(tx, ty, kTW, kTH, kTR, 1.2f);

        const auto ti = [&](int li) { return static_cast<int>(ty + 4.f + static_cast<float>(li) * 16.f); };
        const int lw = static_cast<int>(kTW) - 8;
        const int lx = static_cast<int>(tx) + 4;

        g.setFont(juce::FontOptions(9.5f));
        g.setColour(blob.color.brighter(0.25f));
        g.drawText(line1, lx, ti(0), lw, 14, juce::Justification::centredLeft, false);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(8.5f));
        g.drawText(line2, lx, ti(1), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(line3, lx, ti(2), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(line4, lx, ti(3), lw, 13, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
void CisVisualizerComponent::mouseMove(const juce::MouseEvent& event)
{
    const auto ms = getActiveSource();
    const bool isSpctr = (ms == VisualizerMode::SPCTR_BLOB);
    if (ms != VisualizerMode::SYNTH_BLOB && !isSpctr)
    {
        hoverBlobIdx_ = -1;
        return;
    }

    hoverPos_ = event.getPosition();

    const int W = getWidth();
    const auto& blobs = isSpctr ? spctrBlobs_ : synthBlobs_;
    if (W <= 1 || cisPixelsCount <= 0 || blobs.empty())
    {
        hoverBlobIdx_ = -1;
        return;
    }

    // ── Magnetic snap ─────────────────────────────────────────────────────────
    const float cisPerDisplayPx = static_cast<float>(cisPixelsCount - 1)
                                 / static_cast<float>(W - 1);
    const float cursorCi = static_cast<float>(hoverPos_.x) * cisPerDisplayPx;

    constexpr float kSnapDisplayPx = 20.f;
    const float snapRadiusCi = kSnapDisplayPx * cisPerDisplayPx;

    int   bestIdx  = -1;
    float bestDist = snapRadiusCi;

    for (int b = 0; b < static_cast<int>(blobs.size()); ++b)
    {
        const auto& blob = blobs[b];
        float distCi = 0.f;
        if (cursorCi < static_cast<float>(blob.startPx))
            distCi = static_cast<float>(blob.startPx) - cursorCi;
        else if (cursorCi >= static_cast<float>(blob.endPx))
            distCi = cursorCi - static_cast<float>(blob.endPx - 1);
        if (distCi <= bestDist) { bestDist = distCi; bestIdx = b; }
    }

    hoverBlobIdx_ = bestIdx;
}

//==============================================================================
void CisVisualizerComponent::mouseExit(const juce::MouseEvent&)
{
    hoverBlobIdx_ = -1;
}

//==============================================================================
// FFT — computeFftMagnitudes
// Computes a Hann-windowed real FFT on localDataGray using KissFFT.
// Results are stored in fftMagnitudesSmoothed_ with exponential smoothing.
// The KissFFT config is cached in fftCfg_ and reallocated only when
// cisPixelsCount changes.  All work is O(N log N) on the UI thread at 30 fps.
//==============================================================================
void CisVisualizerComponent::computeFftMagnitudes()
{
    if (localDataGray.empty() || cisPixelsCount == 0)
    {
        fftMagnitudes_.clear();
        fftMagnitudesSmoothed_.clear();
        fftHarmonicity_.clear();
        fftNumHarmonics_ = 0;
        return;
    }

    // ── Read quality / smoothing parameters from APVTS ────────────────────────
    // lxFftBins choice: 0=32, 1=64, 2=128, 3=256 harmonics.
    // This is the number of oscillators that will be fed to the LuxSynth engine.
    static const int kBinsChoices[] = { 32, 64, 128, 256 };
    const int binsChoice = juce::jlimit(0, 3, static_cast<int>(
        processor.getAPVTS().getRawParameterValue("lxFftBins")->load()));
    const int nHarmonics = kBinsChoices[binsChoice];

    // lxFftSmoothing [0..1]:
    //   0 = very fast / reactive : alpha_attack≈0.80, alpha_release≈0.50
    //   1 = very slow / smooth   : alpha_attack≈0.05, alpha_release≈0.02
    const float sm = juce::jlimit(0.0f, 1.0f,
        processor.getAPVTS().getRawParameterValue("lxFftSmoothing")->load());
    const float alphaAttack  = 0.80f - sm * 0.75f;  // [0.05 .. 0.80]
    const float alphaRelease = 0.50f - sm * 0.48f;  // [0.02 .. 0.50]

    const int N     = cisPixelsCount;
    const int nBins = N / 2 + 1;
    // Cap harmonics to what the FFT can actually provide
    const int nDisplay = juce::jmin(nHarmonics, nBins - 1);
    fftNumHarmonics_ = nDisplay;

    // ── Reallocate KissFFT config when the signal size changes ────────────────
    if (fftSize_ != N)
    {
        if (fftCfg_)
        {
            kiss_fft_free(reinterpret_cast<kiss_fftr_cfg>(fftCfg_));
            fftCfg_ = nullptr;
        }
        fftCfg_  = static_cast<void*>(kiss_fftr_alloc(N, 0, nullptr, nullptr));
        fftSize_ = N;
        fftMagnitudes_.assign(static_cast<size_t>(nBins), 0.0f);
        fftMagnitudesSmoothed_.assign(static_cast<size_t>(nBins), 0.0f);
        fftHarmonicity_.assign(static_cast<size_t>(nBins), 0.5f);
    }
    // Grow buffers if bins choice changes
    if (fftHarmonicity_.size() < static_cast<size_t>(nBins))
        fftHarmonicity_.assign(static_cast<size_t>(nBins), 0.5f);

    auto* cfg = reinterpret_cast<kiss_fftr_cfg>(fftCfg_);
    if (!cfg) return;

    thread_local std::vector<kiss_fft_scalar> inBuf;
    thread_local std::vector<kiss_fft_cpx>    outBuf;
    inBuf.resize(static_cast<size_t>(N));
    outBuf.resize(static_cast<size_t>(nBins));

    // ── Hann window ───────────────────────────────────────────────────────────
    const float kTwoPiOverN =
        2.0f * static_cast<float>(M_PI) / static_cast<float>(juce::jmax(1, N - 1));
    for (int i = 0; i < N; ++i)
    {
        const float hann = 0.5f * (1.0f - std::cos(kTwoPiOverN * static_cast<float>(i)));
        inBuf[static_cast<size_t>(i)] =
            (static_cast<float>(localDataGray[static_cast<size_t>(i)]) / 255.0f) * hann;
    }

    kiss_fftr(cfg, inBuf.data(), outBuf.data());

    // ── Magnitudes — suppress DC ──────────────────────────────────────────────
    fftMagnitudes_[0] = 0.0f;
    float maxMag = 1e-12f;
    for (int k = 1; k <= nDisplay; ++k)
    {
        const float re  = outBuf[static_cast<size_t>(k)].r;
        const float im  = outBuf[static_cast<size_t>(k)].i;
        const float mag = std::sqrt(re * re + im * im);
        fftMagnitudes_[static_cast<size_t>(k)] = mag;
        if (mag > maxMag) maxMag = mag;
    }

    // ── Peak-normalise (first nDisplay bins only) ─────────────────────────────
    const float invMax = 1.0f / maxMag;
    for (int k = 1; k <= nDisplay; ++k)
        fftMagnitudes_[static_cast<size_t>(k)] *= invMax;

    // ── Temporal smoothing (parametric attack / release) ──────────────────────
    for (int k = 0; k <= nDisplay; ++k)
    {
        const float cur  = fftMagnitudes_[static_cast<size_t>(k)];
        const float prev = fftMagnitudesSmoothed_[static_cast<size_t>(k)];
        const float a    = (cur >= prev) ? alphaAttack : alphaRelease;
        fftMagnitudesSmoothed_[static_cast<size_t>(k)] =
            a * cur + (1.0f - a) * prev;
    }

    // ── Per-bin harmonicity from CIS colour temperature ───────────────────────
    // The LuxSynth engine uses harmonicity[k] to decide whether oscillator k
    // behaves as a harmonic (warm: R>B) or inharmonic (cool: B>R) partial.
    //
    // Mapping: divide the CIS scan into nDisplay equal sections.
    // Bin k ↔ section k of the scan (k spatial cycles across the full line).
    // Average (R-B)/255 in that section → temperature → harmonicity [0..1].
    // Light temporal smoothing (τ ≈ 10 frames at 30 fps) avoids flicker.
    if (!localDataR.empty() && !localDataB.empty())
    {
        // ── Subtract global CIS sensor R-B bias ───────────────────────────────
        // The CIS sensor has a fixed warm bias (R > B globally on white surfaces).
        // Subtracting the per-frame global mean centres harmonicity around 0.5 so
        // that only *local* colour variations drive the per-bin values.
        float globalR = 0.0f, globalB = 0.0f;
        for (int i = 0; i < cisPixelsCount; ++i)
        {
            globalR += static_cast<float>(localDataR[static_cast<size_t>(i)]);
            globalB += static_cast<float>(localDataB[static_cast<size_t>(i)]);
        }
        const float globalBias = (globalR - globalB)
                                 / (static_cast<float>(cisPixelsCount) * 255.0f);

        // Amplification: the per-bin R-B delta is typically small after bias removal;
        // multiply by kHarmGain so that even moderate colour variations push the
        // harmonicity toward 0 or 1 rather than hovering near 0.5.
        constexpr float kHarmGain = 4.0f;

        const int regionW = juce::jmax(1, cisPixelsCount / juce::jmax(1, nDisplay));
        for (int k = 1; k <= nDisplay; ++k)
        {
            const int posStart = juce::jlimit(0, cisPixelsCount - 1, (k - 1) * regionW);
            const int posEnd   = juce::jlimit(posStart + 1, cisPixelsCount, k * regionW);
            float sumR = 0.0f, sumB = 0.0f;
            for (int i = posStart; i < posEnd; ++i)
            {
                sumR += static_cast<float>(localDataR[static_cast<size_t>(i)]);
                sumB += static_cast<float>(localDataB[static_cast<size_t>(i)]);
            }
            const float n        = static_cast<float>(posEnd - posStart);
            // Bias-corrected, amplified temperature [-1..1]
            const float tempRaw  = (sumR - sumB) / (n * 255.0f) - globalBias;
            const float tempAmp  = juce::jlimit(-1.0f, 1.0f, tempRaw * kHarmGain);
            const float newH     = (tempAmp + 1.0f) * 0.5f;  // [0..1]
            // Faster smoothing (τ ≈ 3 frames) so the cursor reacts quickly
            fftHarmonicity_[static_cast<size_t>(k)] =
                0.40f * newH + 0.60f * fftHarmonicity_[static_cast<size_t>(k)];
        }
    }
}

//==============================================================================
// FFT COLOR — per-bin harmonicity visualizer (LuxSynth synthesis data)
//
// Each bar represents one LuxSynth oscillator (harmonic partial).
// Height  = FFT magnitude (oscillator amplitude in synthesis engine).
// Hue     = harmonicity derived from CIS colour temperature:
//             warm (R>B) → orange/red   → harmonic partial behavior
//             cool (R<B) → cyan/blue    → inharmonic partial behavior
//
// This gives musicians a direct visual representation of the timbral data
// that will be fed to the LuxSynth additive synthesis engine.
//==============================================================================
void CisVisualizerComponent::paintFftColorMode(juce::Graphics& g, int W, int H)
{
    g.fillAll(juce::Colour(0xff080808));
    if (cisPixelsCount == 0 || localDataGray.empty()) return;

    computeFftMagnitudes();

    const int nBins = static_cast<int>(fftMagnitudesSmoothed_.size());
    if (nBins < 2 || fftNumHarmonics_ < 1) return;

    const int displayBins = fftNumHarmonics_;

    // Ensure harmonicity buffer is large enough
    if (fftHarmonicity_.size() < static_cast<size_t>(nBins))
        fftHarmonicity_.resize(static_cast<size_t>(nBins), 0.5f);

    auto logScale = [](float x) -> float {
        return std::log10(1.0f + 9.0f * juce::jlimit(0.0f, 1.0f, x));
    };

    for (int x = 0; x < W; ++x)
    {
        const float t = static_cast<float>(x) / static_cast<float>(juce::jmax(1, W - 1));
        const int bin    = 1 + static_cast<int>(t * static_cast<float>(displayBins - 1));
        const int binIdx = juce::jlimit(1, nBins - 1, bin);

        const float mag    = fftMagnitudesSmoothed_[static_cast<size_t>(binIdx)];
        const float logMag = logScale(mag);
        const int   barH   = juce::jmax(1, static_cast<int>(logMag * static_cast<float>(H)));

        // Hue from harmonicity:
        //   1.0 (warm/harmonic)  → hue ≈ 0.05 (orange-red)
        //   0.5 (neutral)        → hue ≈ 0.35 (green-yellow)
        //   0.0 (cool/inharmonic)→ hue ≈ 0.60 (cyan-blue)
        const float harm = fftHarmonicity_[static_cast<size_t>(binIdx)];
        const float hue  = 0.60f - harm * 0.55f;  // [0.60..0.05] as harm→1
        const float val  = 0.15f + 0.85f * logMag;

        g.setColour(juce::Colour::fromHSV(hue, 0.75f, 0.04f + 0.04f * logMag, 1.0f));
        g.fillRect(x, 0, 1, H - barH);

        g.setColour(juce::Colour::fromHSV(hue, 0.90f, val, 1.0f));
        g.fillRect(x, H - barH, 1, barH);

        if (barH > 2)
        {
            g.setColour(juce::Colour::fromHSV(hue, 0.35f, 1.0f, 0.65f));
            g.fillRect(x, H - barH, 1, 2);
        }
    }

    // Reference grid
    g.setColour(juce::Colour(0x16ffffff));
    for (float ref : {0.1f, 0.3f, 0.7f, 0.9f})
    {
        const int refY = H - static_cast<int>(logScale(ref) * static_cast<float>(H));
        g.fillRect(0, refY, W, 1);
    }

    // ── Harmonicity legend strip (4 px at top) ────────────────────────────────
    // Shows the mapping: harmonic (left/orange) → inharmonic (right/blue)
    for (int x2 = 0; x2 < W; ++x2)
    {
        const float t2  = static_cast<float>(x2) / static_cast<float>(juce::jmax(1, W - 1));
        const float h2  = 0.05f + t2 * 0.55f;  // orange→blue from left to right
        g.setColour(juce::Colour::fromHSV(h2, 0.90f, 0.75f, 0.60f));
        g.fillRect(x2, 0, 1, 4);
    }

    g.setColour(juce::Colour(0x70ffffff));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("Harm.",
               juce::Rectangle<int>(4, 5, 36, 12),
               juce::Justification::centredLeft, false);
    g.drawText("Inharm.",
               juce::Rectangle<int>(W - 44, 5, 40, 12),
               juce::Justification::centredRight, false);

    // ── Live harmonicity cursor ────────────────────────────────────────────────
    // A white downward triangle moves along the legend strip in real-time (30 fps),
    // showing the mean harmonicity of the current frame.
    // Left = harmonic (warm/orange), Right = inharmonic (cool/blue).
    {
        // Magnitude-weighted mean harmonicity:
        // Silent bins (near-zero magnitude) don't drag the cursor to mid-position.
        // Only bins with actual energy determine where the cursor sits.
        float sumH = 0.0f, sumMag = 0.0f;
        const int nH = fftNumHarmonics_;
        const size_t nbSize = fftMagnitudesSmoothed_.size();
        for (int k = 1; k <= nH; ++k)
        {
            const float mag = (static_cast<size_t>(k) < nbSize)
                              ? fftMagnitudesSmoothed_[static_cast<size_t>(k)]
                              : 0.0f;
            sumH   += fftHarmonicity_[static_cast<size_t>(k)] * mag;
            sumMag += mag;
        }
        const float avgH = (sumMag > 1e-6f) ? (sumH / sumMag) : 0.5f;

        // Map: harm=1 → x=0 (left), harm=0 → x=W-1 (right)
        const int ix = juce::jlimit(4, W - 5,
            static_cast<int>((1.0f - avgH) * static_cast<float>(W - 1) + 0.5f));

        // Thin full-height reference line (very transparent)
        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.fillRect(ix, 8, 1, H - 8);

        // Downward triangle sitting flush on top of the legend strip (Y=0..7)
        juce::Path tri;
        tri.addTriangle(static_cast<float>(ix - 4), 0.f,
                        static_cast<float>(ix + 4), 0.f,
                        static_cast<float>(ix),     7.f);
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.fillPath(tri);

        // Percentage label: "NNH" (e.g. "73H" = 73% harmonic)
        // Placed on the side with more space to avoid overlap with Harm./Inharm. labels
        const int harmPct   = juce::roundToInt(avgH * 100.f);
        const juce::String pct = juce::String(harmPct) + "% H";
        const bool putLeft  = (ix > W / 2);  // label goes opposite side of cursor
        const int  labelX   = putLeft ? juce::jmax(40, ix - 38)
                                      : juce::jmin(W - 46, ix + 6);
        g.setColour(juce::Colours::white.withAlpha(0.78f));
        g.setFont(juce::FontOptions(8.5f));
        g.drawText(pct, labelX, 2, 36, 10,
                   juce::Justification::centredLeft, false);
    }

    // Harmonic count badge
    {
        const juce::String badge = juce::String(displayBins) + " harmonics";
        g.setColour(juce::Colour(0xa0000000));
        g.fillRoundedRectangle(static_cast<float>(W - 84), 18.f, 80.f, 16.f, 3.f);
        g.setColour(juce::Colour(0xffcc88cc));  // SYNTH_FFT_COLOR accent
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(badge, W - 84, 18, 80, 16, juce::Justification::centred, false);
    }

    g.setColour(juce::Colour(0x50ffffff));
    g.setFont(juce::FontOptions(9.0f));
    g.drawText("LuxSynth harmonics  \xe2\x80\x94 color = harmonicity",
               juce::Rectangle<int>(4, H - 16, W - 8, 13),
               juce::Justification::centredLeft, false);
}
