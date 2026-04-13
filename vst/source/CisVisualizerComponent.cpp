#include "CisVisualizerComponent.h"
#include "PluginProcessor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" {
    #include "audio/buffers/audio_image_buffers.h"
    #include "config/config_instrument.h"
    #include "config/config_loader.h"
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

    // Select rendering mode
    const int mode = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("visualizerMode")->load());

    switch (mode)
    {
        case 1:  paintWaveformMode(g, W, H, false); break;
        case 2:  paintWaveformMode(g, W, H, true);  break;
        default: paintImageMode   (g, W, H);        break;
    }

    // Blob overlay — IMAGE tab only, image mode only
    if (blobOverlayVisible && mode == 0)
        paintBlobOverlay(g, W, H);
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
    juce::Graphics& g, int W, int H, bool inverted) const
{
    g.fillAll(juce::Colours::white);
    const int centerY   = H / 2;
    const int halfHeight = H / 2;

    for (int x = 0; x < W; ++x)
    {
        const uint8_t r  = interpolateCisPixel(localDataR.data(), x, W);
        const uint8_t gr = interpolateCisPixel(localDataG.data(), x, W);
        const uint8_t b  = interpolateCisPixel(localDataB.data(), x, W);
        const int maxCh  = std::max({r, gr, b});

        int barH = inverted
                 ? ((255 - maxCh) * halfHeight) / 255
                 : (maxCh * halfHeight) / 255;

        if (barH > 0)
        {
            g.setColour(juce::Colour(r, gr, b));
            g.fillRect(x, centerY - barH, 1, barH * 2);
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
    const int liveFreezeMode = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("imageFreezeMode")->load());

    // ── FramePlayerThread writes to AudioImageBuffers only when a slot is
    // actively playing AND the sampler transport is not STOP.
    // frame_sampler_is_playing() is the authoritative gate; it reflects the
    // actual FramePlayerThread state (slot active).
    // smpFreezeMode==2 (STOP) prevents FramePlayerThread from writing even
    // when a slot is queued, so both conditions must hold.
    const int  smpFreezeMode = g_sp3ctra_config.sampler_freeze_mode;
    const bool samplerWriting = (frame_sampler_is_playing() != 0)
                                && (smpFreezeMode != 2);

    // ── No active signal → white ──────────────────────────────────────────────
    // Live is WHITE/STOP and sampler is not writing to AudioImageBuffers.
    if (liveFreezeMode == 2 && !samplerWriting)
    {
        std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
        std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
        std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
        std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
        // Publish the all-white frame so BlobVisualizerComponent sees silence.
        if (auto* fs = processor.getFrameSampler())
            fs->setFinalGrayBuffer(localDataGray);
        return;
    }

    // ── Live HOLD and sampler idle → freeze the display ───────────────────────
    // localDataGray (and R/G/B) keep their last values — intended behaviour.
    // Exception: a sequencer STEP_EMPTY still forces white (silence).
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

    // ── Read from AudioImageBuffers (the visual mix bus) ─────────────────────
    // When samplerWriting=true: FramePlayerThread has baked both opacities
    // into AudioImageBuffers — no further scaling needed here.
    // When samplerWriting=false: UDP thread wrote live data; apply live opacity.
    uint8_t *pR, *pG, *pB;
    audio_image_buffers_get_read_pointers(buffers, &pR, &pG, &pB);
    std::memcpy(localDataR.data(), pR, cisPixelsCount);
    std::memcpy(localDataG.data(), pG, cisPixelsCount);
    std::memcpy(localDataB.data(), pB, cisPixelsCount);

    // ── Apply live opacity only when sampler is not the writer ────────────────
    if (!samplerWriting && liveFreezeMode == 0)
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
    // This is the DEFINITIVE visual output: the image that the user sees in
    // image mode IS this array.  It mirrors exactly what preprocess_luxstral()
    // and preprocess_luxstral_sampler() produce inside the synthesis engine,
    // so visualization and audio always reflect the same processed image.
    //
    // Pipeline (same as preprocess_luxstral* steps 1, 3, 4):
    //   STEP 1 — RGB → grayscale (Rec. 601 weights)
    //   STEP 3 — optional inversion
    //   STEP 4 — optional gamma  [uses sampler_gamma or additive_gamma_value
    //             depending on which source is currently driving AudioImageBuffers]
    {
        const int   doInvert = g_sp3ctra_config.invert_intensity;

        // ── Gamma: select correct source depending on active signal path ───────
        // LIVE path  → controlled by the live gamma-enable toggle + live gamma value.
        // SAMPLER path → always applied whenever sampler_gamma > 0 (the user-visible
        //   "Gamma" slider in the SAMPLER column of the IMAGE page).  The live
        //   gamma-enable flag is irrelevant for the sampler path.
        // Bug fix: previously gammaOn was always additive_enable_non_linear_mapping,
        //   so with the live gamma toggle OFF the sampler gamma had no visual effect
        //   even when the slider was non-zero (surexposition).
        const float gammaVal = samplerWriting
                               ? g_sp3ctra_config.sampler_gamma
                               : g_sp3ctra_config.additive_gamma_value;
        const int gammaOn = samplerWriting
                            ? (gammaVal > 0.0f ? 1 : 0)
                            : g_sp3ctra_config.additive_enable_non_linear_mapping;

        localDataGray.resize(cisPixelsCount); // no-op if already sized
        for (int i = 0; i < cisPixelsCount; ++i)
        {
            // STEP 1: Rec. 601 luminance
            float gray = (0.299f * static_cast<float>(localDataR[i])
                        + 0.587f * static_cast<float>(localDataG[i])
                        + 0.114f * static_cast<float>(localDataB[i])) / 255.0f;
            if (gray < 0.0f) gray = 0.0f;
            if (gray > 1.0f) gray = 1.0f;

            // STEP 3: inversion
            if (doInvert) gray = 1.0f - gray;

            // STEP 4: gamma — surexposition / non-linear mapping
            if (gammaOn && gammaVal > 0.0f)
                gray = std::pow(gray, gammaVal);

            if (gray < 0.0f) gray = 0.0f;
            if (gray > 1.0f) gray = 1.0f;

            localDataGray[i] = static_cast<uint8_t>(gray * 255.0f + 0.5f);
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
