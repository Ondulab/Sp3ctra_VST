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
}

// Forward-declare the C hook defined in FrameSampler.cpp.
// Returns 1 when a FramePlayerThread slot is actually writing to AudioImageBuffers,
// 0 otherwise.  This is the authoritative gate for the visual mix path.
extern "C" int frame_sampler_is_playing(void);

//==============================================================================
CisVisualizerComponent::CisVisualizerComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    startTimer(1000 / kTimerFps);
}

CisVisualizerComponent::~CisVisualizerComponent()
{
    stopTimer();
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
        case VisualizerMode::SYNTH_FFT_GRAY:
            accent = juce::Colour(0xffe06868); break;
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
    else
    {
        // MIX / downstream — combined freeze logic
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
         *  SPCTR_* (LUXSTRAL): additive_gamma_value, guarded by additive_enable_non_linear_mapping
         *  SYNTH_*  (LUXSYNTH): luxsynth_gamma_value, always active (no enable flag; 1.0 = no-op) */
        float gammaVal;
        int   gammaOn;
        if (isSourceView) {
            gammaVal = 0.0f;
            gammaOn  = 0;
        } else if (isSpctrView) {
            gammaVal = samplerWriting ? g_sp3ctra_config.sampler_gamma
                                      : g_sp3ctra_config.additive_gamma_value;
            gammaOn  = samplerWriting ? (gammaVal > 0.0f ? 1 : 0)
                                      : g_sp3ctra_config.additive_enable_non_linear_mapping;
        } else {
            /* SYNTH_* — gamma always active; 1.0 is a mathematical no-op */
            gammaVal = g_sp3ctra_config.luxsynth_gamma_value;
            gammaOn  = (gammaVal > 0.0f && gammaVal != 1.0f) ? 1 : 0;
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
    return m == VisualizerMode::SPCTR_COLOR
        || m == VisualizerMode::SYNTH_COLOR
        || m == VisualizerMode::SYNTH_FFT_COLOR;
}

//==============================================================================
bool CisVisualizerComponent::supportsDisplayModes(VisualizerMode m) const noexcept
{
    // Display mode switching (Image / Waveform / Inverted) is only relevant
    // for RAW data, Live, Sampler, Mix, and grayscale pipeline views.
    // COLOR and BLOB have their own dedicated renderers.
    switch (m)
    {
        case VisualizerMode::RAW:
        case VisualizerMode::LIVE:
        case VisualizerMode::SAMPLER:
        case VisualizerMode::MIX:
        case VisualizerMode::SPCTR_GRAY:
        case VisualizerMode::SYNTH_GRAY:
        case VisualizerMode::SYNTH_FFT_GRAY:
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
