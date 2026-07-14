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
    #include "processing/image_chain.h"
    #include "processing/chain_plan.h"
    #include "processing/lux_pitch.h"
    #include "processing/lux_mask.h"
    #include "processing/internal_source.h"
    #include "synthesis/luxsynth/kissfft/kiss_fftr.h"
    #include "synthesis/luxsynth/synth_luxsynth_engine.h"
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"
}

// Forward-declare C hooks defined in LuxSampler.cpp.
extern "C" int lux_sampler_is_playing(void);
extern "C" int lux_sampler_is_recording(void);

//==============================================================================
// CisHoverTooltip — desktop-level floating tooltip (never clipped by parent)
//
// Added to the JUCE desktop via addToDesktop() so that it appears above ALL
// plugin UI elements regardless of the CisVisualizerComponent's position in
// the component hierarchy.  Being a top-level window it is not subject to any
// parent paint-region clipping.
//
// Lifetime: owned by CisVisualizerComponent (unique_ptr). Hidden on mouseExit.
// Thread safety: all methods called from the UI/message thread only.
//==============================================================================
class CisVisualizerComponent::CisHoverTooltip : public juce::Component
{
public:
    CisHoverTooltip()
    {
        setInterceptsMouseClicks(false, false);
        // Add as a temporary top-level window so JUCE paints it independently of
        // any parent component clip regions.
        addToDesktop(juce::ComponentPeer::windowIsTemporary
                   | juce::ComponentPeer::windowIgnoresMouseClicks
                   | juce::ComponentPeer::windowIgnoresKeyPresses);
        setAlwaysOnTop(true);
        setVisible(false);
    }

    ~CisHoverTooltip() override
    {
        removeFromDesktop();
    }

    // Show tooltip at absolute SCREEN coordinates with given content.
    void showAt(juce::Rectangle<int> screenBounds,
                const juce::String& title,
                const juce::String& l2,
                const juce::String& l3,
                const juce::String& l4,
                juce::Colour accent)
    {
        title_  = title;
        l2_     = l2;
        l3_     = l3;
        l4_     = l4;
        accent_ = accent;
        setBounds(screenBounds);
        setVisible(true);
        toFront(false);
        repaint();
    }

    void hide() { setVisible(false); }

    void paint(juce::Graphics& g) override
    {
        const float kTW = static_cast<float>(getWidth());
        const float kTH = static_cast<float>(getHeight());
        constexpr float kTR = 4.f;

        g.setColour(juce::Colour(0xee0d0d0d));
        g.fillRoundedRectangle(0.f, 0.f, kTW, kTH, kTR);
        g.setColour(accent_.withAlpha(0.90f));
        g.drawRoundedRectangle(0.f, 0.f, kTW, kTH, kTR, 1.2f);

        const int lw = getWidth() - 8;
        const auto ti = [](int li) { return 4 + li * 16; };

        g.setFont(juce::FontOptions(9.5f));
        g.setColour(accent_.brighter(0.25f));
        g.drawText(title_, 4, ti(0), lw, 14, juce::Justification::centredLeft, false);

        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(8.5f));
        g.drawText(l2_,    4, ti(1), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(l3_,    4, ti(2), lw, 13, juce::Justification::centredLeft, false);
        g.drawText(l4_,    4, ti(3), lw, 13, juce::Justification::centredLeft, false);
    }

private:
    juce::String title_, l2_, l3_, l4_;
    juce::Colour accent_ { juce::Colours::white };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CisHoverTooltip)
};

//==============================================================================
CisVisualizerComponent::CisVisualizerComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    startTimer(1000 / kTimerFps);
    // Desktop-level tooltip overlay — never clipped by parent components.
    hoverTooltip_ = std::make_unique<CisHoverTooltip>();
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
void CisVisualizerComponent::setActiveSources(const std::vector<VisualizerMode>& sources)
{
    // Message thread only.  Rebuild the panel list, preserving the buffers of
    // panels whose mode is unchanged (avoids a frame of black on re-selection).
    std::vector<PanelData> next;
    next.reserve(sources.size());
    for (auto mode : sources)
    {
        PanelData pd;
        pd.mode = mode;
        // Reuse an existing same-mode panel's buffers when possible.
        for (auto& old : panels_)
            if (old.mode == mode)
            {
                pd.r = std::move(old.r); pd.g = std::move(old.g);
                pd.b = std::move(old.b); pd.gray = std::move(old.gray);
                break;
            }
        // Size buffers now so the first paint after a re-selection never sees
        // empty panel buffers (updateCisData() refills them every timer tick).
        if (cisPixelsCount > 0)
        {
            auto fit = [this](std::vector<uint8_t>& v)
            { if (static_cast<int>(v.size()) != cisPixelsCount)
                  v.assign(static_cast<size_t>(cisPixelsCount), 255); };
            fit(pd.r); fit(pd.g); fit(pd.b); fit(pd.gray);
        }
        next.push_back(std::move(pd));
    }
    panels_ = std::move(next);
    repaint();
}

VisualizerMode CisVisualizerComponent::panelModeAtY(int y) const noexcept
{
    const int n = static_cast<int>(panels_.size());
    const int H = getHeight();
    if (n <= 0 || H <= 0) return VisualizerMode::SELECTED_TAP;
    int idx = (y * n) / H;
    idx = juce::jlimit(0, n - 1, idx);
    return panels_[static_cast<size_t>(idx)].mode;
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

        // Distinguish the two no-data states so the user sees the *real* cause:
        //   1) Standalone with no audio output device selected → prepareToPlay()
        //      is never invoked by the host, the shared pipeline never starts,
        //      the UDP socket is never bound and CIS data cannot be received.
        //   2) Pipeline is running but no UDP packets have arrived yet.
        const bool pipelineReady = processor.isPipelineReady();

        if (!pipelineReady)
        {
            // ── Two-line centred message ─────────────────────────────────────
            const auto full     = getLocalBounds();
            const int  cy       = full.getCentreY();
            const int  lineH    = juce::jmax(14, full.getHeight() / 14);
            const auto line1Box = juce::Rectangle<int>(full.getX(),
                                                       cy - lineH,
                                                       full.getWidth(),
                                                       lineH);
            const auto line2Box = juce::Rectangle<int>(full.getX(),
                                                       cy + 2,
                                                       full.getWidth(),
                                                       lineH);

            // Primary line — warm amber, bold, attention-grabbing.
            g.setColour(juce::Colour(0xffe0a040));
            g.setFont(juce::Font(juce::FontOptions((float) lineH * 0.85f,
                                                   juce::Font::bold)));
            g.drawText("No audio output configured",
                       line1Box, juce::Justification::centred);

            // Secondary line — actionable hint.
            g.setColour(juce::Colours::grey);
            g.setFont(juce::Font(juce::FontOptions((float) lineH * 0.65f)));
            g.drawText("Open Options > Audio/MIDI Settings and select an output "
                       "device to start the CIS receiver.",
                       line2Box, juce::Justification::centred);
        }
        else
        {
            g.setColour(juce::Colours::grey);
            g.drawText("Waiting for CIS data...",
                       getLocalBounds(), juce::Justification::centred);
        }
        return;
    }

    // ── Stacked panels: one per active pipeline output ────────────────────────
    // The fixed visualizer height is divided between N panels (top-to-bottom).
    // Each panel renders its own source from its own frame buffers, which were
    // filled by updateCisData() → fillSourceBuffers().
    const int n = static_cast<int>(panels_.size());
    if (n <= 0)
    {
        // No selection (empty rack): idle state, never a stale frame.
        g.fillAll(juce::Colour(0xff1a1a1a));
        g.setColour(juce::Colours::grey);
        g.drawText("No module selected",
                   getLocalBounds(), juce::Justification::centred);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        const int y  = (i * H) / n;
        const int ph = ((i + 1) * H) / n - y;
        if (ph <= 0) continue;

        // Copy this panel's frame into the localData* scratch buffers that the
        // paint helpers read.  (≤4 panels × 30 fps → negligible.)
        const auto& pd = panels_[static_cast<size_t>(i)];
        localDataR    = pd.r;
        localDataG    = pd.g;
        localDataB    = pd.b;
        localDataGray = pd.gray;

        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(0, y, W, ph);
            g.setOrigin(0, y);
            paintSource(g, pd.mode, W, ph);
        }

        // Separator line between panels.
        if (i > 0)
        {
            g.setColour(juce::Colour(0x40ffffff));
            g.fillRect(0, y, W, 1);
        }
    }
}

//==============================================================================
void CisVisualizerComponent::paintSource(
    juce::Graphics& g, VisualizerMode source, int W, int H)
{
    // ── SPCTR_BLOB: dedicated coloured blob visualizer (LuxStral path) ───────
    if (source == VisualizerMode::SPCTR_BLOB)
    {
        paintSpctrBlobMode(g, W, H);
        paintSourceLabel(g, source, W, H);
        return;
    }

    // ── SYNTH_BLOB: dedicated coloured blob visualizer ────────────────────────
    // Intercept before the generic rendering path; has its own full renderer.
    if (source == VisualizerMode::SYNTH_BLOB)
    {
        paintSynthBlobMode(g, W, H);
        paintSourceLabel(g, source, W, H);
        return;
    }

    // ── FFT mode: dedicated spectrum renderer ────────────────────────────────
    if (source == VisualizerMode::SYNTH_FFT_COLOR)
    {
        paintFftColorMode(g, W, H);
        paintSourceLabel(g, source, W, H);
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
        const bool isSourceView = (source == VisualizerMode::SRC_IMAGE
                                || source == VisualizerMode::SRC_VIDEO
                                || source == VisualizerMode::SRC_CAMERA
                                || source == VisualizerMode::SELECTED_TAP);

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
    paintSourceLabel(g, source, W, H);
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
    juce::Graphics& g, VisualizerMode source, int W, int H) const
{
    juce::ignoreUnused(W, H);
    // SELECTED_TAP badges the actual module + chain ("MASK - CHAIN 2"),
    // pushed by the editor on selection; other modes use the static label.
    const juce::String label =
        (source == VisualizerMode::SELECTED_TAP && selectedTapLabel_.isNotEmpty())
            ? selectedTapLabel_
            : juce::String(visualizerModeLabel(source));

    // Semi-transparent pill badge — top-left corner
    juce::GlyphArrangement ga;
    ga.addLineOfText(juce::Font(juce::FontOptions(10.f)), label, 0.f, 0.f);
    const float pillW = 8.f + ga.getBoundingBox(0, -1, false).getWidth() + 8.f;
    constexpr float pillH = 16.f;
    constexpr float pillX = 4.f;
    constexpr float pillY = 4.f;

    // Accent colour based on pipeline family
    juce::Colour accent;
    switch (source)
    {
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

    // Always run FFT + feed spectral data to the LuxSynth engine, regardless
    // of which visualizer tab is active.  Without this, the engine only receives
    // spectral data when the user is on the FFT Color view, causing silence on
    // all other tabs.
    computeFftMagnitudes();

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

    // ── Render one stacked panel per active source ────────────────────────────
    if (panels_.empty())
        return;
    for (size_t i = 0; i < panels_.size(); ++i)
        fillSourceBuffers(panels_[i], i == 0);

}

//==============================================================================
void CisVisualizerComponent::fillSourceBuffers(PanelData& out, bool isPrimary)
{
    auto* core = processor.getSp3ctraCore();
    if (!core || !core->isInitialized()) return;
    auto* buffers = core->getAudioImageBuffers();
    if (!buffers || !buffers->initialized) return;

    extern sp3ctra_config_t g_sp3ctra_config;
    const VisualizerMode vizSource = out.mode;

    // ── Restore this panel's previous frame into the localData* scratch ───────
    // The paint helpers read localData*; HOLD / freeze-hold paths leave it
    // untouched so the panel keeps its last frame.  Side-effects that feed
    // synthesis (final-gray publish) only run for the primary panel.
    auto ensureSized = [this](std::vector<uint8_t>& v)
    {
        if (static_cast<int>(v.size()) != cisPixelsCount)
            v.assign(static_cast<size_t>(cisPixelsCount), 255);
    };
    ensureSized(out.r); ensureSized(out.g); ensureSized(out.b); ensureSized(out.gray);
    localDataR = out.r; localDataG = out.g; localDataB = out.b; localDataGray = out.gray;

    // ── publish helper: only the primary panel feeds synthesis/BlobVisualizer ─
    auto publishGray = [this, isPrimary]
    {
        if (isPrimary)
            if (auto* fs = processor.getLuxSampler())
                fs->setFinalGrayBuffer(localDataGray);
    };

    // ── Source-specific freeze gates ──────────────────────────────────────────
    if (vizSource == VisualizerMode::SRC_IMAGE
          || vizSource == VisualizerMode::SRC_VIDEO
          || vizSource == VisualizerMode::SRC_CAMERA
          || vizSource == VisualizerMode::SELECTED_TAP)
    {
        // Media source modules own their transport (imgSrcPlay / vidSrcPlay /
        // camera device) — the device/live/sampler freeze gates do not apply.
        // The module's line is read from the internal source pool below.
        // SELECTED_TAP mirrors its executor's stream verbatim: whatever gating
        // applies upstream is already reflected in the published frames.
    }
    else
    {
        // Downstream views (SPCTR_*, SYNTH_*) — PER-CHAIN freeze gating
        // (2026-07-10): which transport may whiten/hold the panel follows the
        // ENGINE'S OWN CHAIN (RT plan), not the legacy global source type:
        //   • chain holds a SAMPLER → sampler transport gates (STOP → white,
        //     HOLD → freeze), bypassed while recording;
        //   • chain is DEVICE-fed (live source, or no source module = live
        //     fallback / engine absent) → device transport gates;
        //   • internal source (IMAGE/VIDEO/CAMERA) without sampler → NO
        //     transport gate: media modules own their transport (mirror of
        //     the SRC_* views exemption above); the engine input tap already
        //     reflects the module's own play/pause state.
        const bool isSpctrLocal = (vizSource == VisualizerMode::SPCTR_GRAY
                                || vizSource == VisualizerMode::SPCTR_COLOR
                                || vizSource == VisualizerMode::SPCTR_BLOB);
        ChainPlan gatePlan;
        chain_plan_get(&gatePlan);
        /* (P4-M4) plan.synth[] is gone — the gate follows the chain that
         * actually FEEDS the viewed engine: SPCTR = the first "→ LUXSTRAL"
         * send's chain (the head tap's source), SYNTH = the Path-B chain
         * (first "→ LUXSYNTH" OUT, else "→ LUXWAVE"). No OUT anywhere →
         * unfed engine: the tap is already white, nothing to gate. */
        const SynthChainPlan* spGate = nullptr;
        if (isSpctrLocal)
        {
            if (gatePlan.num_ls_sends > 0)
                spGate = &gatePlan.ls_send[0].recipe;
        }
        else
        {
            const SynthChainPlan* lw = nullptr;
            for (int c = 0; c < gatePlan.num_chains && spGate == nullptr; ++c)
            {
                const SynthChainPlan& sp = gatePlan.chain[c];
                if (! sp.present) continue;
                for (int i = 0; i < sp.num_inserts; ++i)
                {
                    if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                    { spGate = &sp; break; }
                    if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE
                        && lw == nullptr)
                        lw = &sp;
                }
            }
            if (spGate == nullptr) spGate = lw;
        }

        if (spGate != nullptr)
        {
            // ONE transport authority (chain_send_transport): the chain that
            // feeds the viewed engine decides. Sampler-record bypass: while
            // recording, the incoming stream IS what is captured — never
            // blank/freeze the display.
            const bool recBypass = spGate->has_sampler
                                && (lux_sampler_is_recording() != 0);
            int gateFreeze = 0, gateFade = 0;
            chain_send_transport(spGate, &gateFreeze, &gateFade);
            if (!recBypass)
            {
                if (gateFreeze == 2)
                {
                    std::fill(localDataR.begin(),    localDataR.end(),    uint8_t{255});
                    std::fill(localDataG.begin(),    localDataG.end(),    uint8_t{255});
                    std::fill(localDataB.begin(),    localDataB.end(),    uint8_t{255});
                    std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});
                    publishGray();
                    goto done;
                }
                if (gateFreeze == 1) goto done;
            }
        }
        // spGate == nullptr: no OUT feeds this engine → tap is white already.
    }

    // ── Read from appropriate buffer ─────────────────────────────────────────
    // Source-level views read their dedicated buffer.
    // Downstream views (SPCTR_*, SYNTH_*) respect the per-path source selector
    //   so that changing the Source combo in the UI is reflected in the visualizer.
    // Assignments (not initializations) — the freeze gates above jump over
    // this point via `goto done`, and C++ forbids bypassing initializations.
    uint8_t *pR, *pG, *pB;
    pR = pG = pB = nullptr;
    if (vizSource == VisualizerMode::SRC_IMAGE
     || vizSource == VisualizerMode::SRC_VIDEO
     || vizSource == VisualizerMode::SRC_CAMERA)
    {
        // Contextual media-source view: the selected module's OWN line from
        // the internal source pool — never the global live/device buffers.
        const int kind = (vizSource == VisualizerMode::SRC_IMAGE) ? INTERNAL_SRC_IMAGE
                       : (vizSource == VisualizerMode::SRC_VIDEO) ? INTERNAL_SRC_VIDEO
                                                                  : INTERNAL_SRC_CAMERA;
        if (internal_source_copy(kind, localDataR.data(), localDataG.data(),
                                 localDataB.data(), cisPixelsCount) <= 0)
        {
            // Module inactive / nothing loaded → white (= silence), never a
            // stale frame or the live device feed.
            std::fill(localDataR.begin(), localDataR.end(), uint8_t{255});
            std::fill(localDataG.begin(), localDataG.end(), uint8_t{255});
            std::fill(localDataB.begin(), localDataB.end(), uint8_t{255});
        }
        // pR/pG/pB stay null — the data is already in localData*.
    }
    else if (vizSource == VisualizerMode::SELECTED_TAP)
    {
        // Contextual view: the stream AT the selected module's position in ITS
        // chain, published by the chain executor (selection-tap bus). White
        // when the chain is silent/unfed (cleared on every selection change).
        audio_image_buffers_get_selection_tap_pointers(buffers, &pR, &pG, &pB);
    }
    else
    {
        // (P4-M3) The MODULATED / LUXPITCH_OUTPUT / LUXMASK_OUTPUT modes are
        // dead: every module selection is contextual (SELECTED_TAP) and the
        // global modulated bus + insert taps are gone. Legacy persisted modes
        // fall through here harmlessly (engine-tap read).
        // Downstream views — PER-CHAIN display (2026-07-10): read the ENGINE
        // INPUT TAP, the exact RGB frame the engine's pipeline consumed on its
        // last committed cycle, published by whichever thread owned that
        // commit (udpThread / feeder tick / FramePlayerThread). The legacy
        // luxstral/luxsynth_source_type → RAW/SAMPLER/MODULATED switch is gone
        // from the display path: it routed by GLOBAL source type, so any
        // topology the global buses did not carry (internal source + processor
        // upstream, per-chain inserts, playback without device) showed a dead
        // or wrong bus. White tap = engine unfed (chain without signal).
        const bool isSpctr = (vizSource == VisualizerMode::SPCTR_GRAY
                           || vizSource == VisualizerMode::SPCTR_COLOR
                           || vizSource == VisualizerMode::SPCTR_BLOB);
        audio_image_buffers_get_engine_input_pointers(
            buffers,
            isSpctr ? AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A
                    : AUDIO_IMAGE_ENGINE_TAP_PATHB,
            &pR, &pG, &pB);
    }
    if (pR != nullptr)   // null for SRC_* views (already filled localData* above)
    {
        std::memcpy(localDataR.data(), pR, cisPixelsCount);
        std::memcpy(localDataG.data(), pG, cisPixelsCount);
        std::memcpy(localDataB.data(), pB, cisPixelsCount);
    }

    // (Insert-tap demand is set once per frame in updateCisData(), based on
    //  whichever panels are displayed — see the loop there.)

    // ── Apply live opacity ───────────────────────────────────────────────────
    // Opacity controls affect ONLY the MIX bus (the blended output).
    // RAW, LIVE, and SAMPLER show their pure data without opacity adjustments.
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
        const bool isSourceView = (vizSource == VisualizerMode::SRC_IMAGE
                                || vizSource == VisualizerMode::SRC_VIDEO
                                || vizSource == VisualizerMode::SRC_CAMERA
                                || vizSource == VisualizerMode::SELECTED_TAP);
        const bool isSpctrView = (vizSource == VisualizerMode::SPCTR_GRAY
                               || vizSource == VisualizerMode::SPCTR_COLOR
                               || vizSource == VisualizerMode::SPCTR_BLOB);

        /* Per-path flags — synth-split P1: read the per-OUT conditioning banks
         * (SPCTR_* = LuxStral OUT slot 0, SYNTH_* = LuxSynth OUT slot 0), the
         * same values the pipeline consumes. */
        const int doInvert = isSourceView ? 0
                           : (isSpctrView ? g_sp3ctra_config.luxstral_out[0].negative
                                          : g_sp3ctra_config.luxsynth_out[0].negative);
        const int doDcBlock = isSourceView ? 0
                            : (isSpctrView ? g_sp3ctra_config.luxstral_out[0].dc_blocking
                                           : g_sp3ctra_config.luxsynth_out[0].dc_blocking);

        /* Gamma: per-OUT bank value; no enable toggle — identity at 1.0. */
        float gammaVal;
        int   gammaOn;
        if (isSourceView) {
            gammaVal = 0.0f;
            gammaOn  = 0;
        } else if (isSpctrView) {
            gammaVal = g_sp3ctra_config.luxstral_out[0].gamma;
            gammaOn  = (gammaVal > 0.0f && gammaVal != 1.0f) ? 1 : 0;
        } else {
            gammaVal = g_sp3ctra_config.luxsynth_out[0].gamma;
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
        auto* fs_ = processor.getLuxSampler();
        if (fs_ && fs_->isSeqSilentStepActive())
            std::fill(localDataGray.begin(), localDataGray.end(), uint8_t{255});

        // ── Publish the mix-final gray buffer (primary panel only) ────────────
        // BlobVisualizerComponent reads this via getFinalGrayBuffer() so that
        // it always operates on the same image that is displayed to the user.
        if (isPrimary && fs_)
            fs_->setFinalGrayBuffer(localDataGray);
    }

done:
    // ── Store the computed frame back into the panel's buffers ────────────────
    out.r = localDataR; out.g = localDataG; out.b = localDataB; out.gray = localDataGray;
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
        case VisualizerMode::SPCTR_GRAY:
        case VisualizerMode::SYNTH_GRAY:
        case VisualizerMode::SRC_IMAGE:
        case VisualizerMode::SRC_VIDEO:
        case VisualizerMode::SRC_CAMERA:
        case VisualizerMode::SELECTED_TAP:
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
        showDisplayModeMenu(panelModeAtY(event.y));
        return;
    }

    // Default left-click behaviour
    juce::Component::mouseDown(event);
}

//==============================================================================
void CisVisualizerComponent::showDisplayModeMenu(VisualizerMode source)
{
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

    // ── Tooltip: rendered by hoverTooltip_ desktop overlay (see mouseMove) ───
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

    // ── Tooltip: rendered by hoverTooltip_ desktop overlay (see mouseMove) ───
}

//==============================================================================
void CisVisualizerComponent::mouseMove(const juce::MouseEvent& event)
{
    // Hit-test only when the panel under the cursor is a blob view.
    const auto ms = panelModeAtY(event.getPosition().y);
    const bool isSpctr = (ms == VisualizerMode::SPCTR_BLOB);
    if (ms != VisualizerMode::SYNTH_BLOB && !isSpctr)
    {
        hoverBlobIdx_ = -1;
        if (hoverTooltip_) hoverTooltip_->hide();
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

    // ── Show desktop-level tooltip overlay ────────────────────────────────────
    // The tooltip is a top-level window (addToDesktop) so it is NEVER clipped
    // by parent component bounds — it appears above all plugin UI elements.
    if (hoverTooltip_)
    {
        if (hoverBlobIdx_ >= 0 && hoverBlobIdx_ < static_cast<int>(blobs.size()))
        {
            const auto& blob = blobs[hoverBlobIdx_];
            const juce::String line1 = "Blob #" + juce::String(hoverBlobIdx_ + 1);
            const juce::String line2 = "Width:  " + juce::String(blob.endPx - blob.startPx) + " px";
            const juce::String line3 =
                "Peak:   " + juce::String(static_cast<int>(blob.peakIntensity * 100.f)) + "%"
                + "  avg: " + juce::String(static_cast<int>(blob.avgIntensity * 100.f)) + "%";
            const juce::String tempStr =
                (blob.avgColorTemp >  0.08f) ? "Warm" :
                (blob.avgColorTemp < -0.08f) ? "Cool" : "Neutral";
            const juce::String line4 = "Temp:   " + tempStr;

            // Convert component-local cursor pos to absolute screen coords.
            // No clamping needed — the overlay is positioned on the desktop.
            constexpr int kTW = 148, kTH = 70;
            const auto screenPos = localPointToGlobal(hoverPos_);
            int tx = screenPos.x + 14;
            int ty = screenPos.y - kTH / 2;

            hoverTooltip_->showAt({ tx, ty, kTW, kTH },
                                  line1, line2, line3, line4, blob.color);
        }
        else
        {
            hoverTooltip_->hide();
        }
    }
}

//==============================================================================
void CisVisualizerComponent::mouseExit(const juce::MouseEvent&)
{
    hoverBlobIdx_ = -1;
    if (hoverTooltip_) hoverTooltip_->hide();
}

//==============================================================================
// FFT — computeFftMagnitudes
// Computes a Hann-windowed real FFT using KissFFT — DISPLAY ONLY: the engine
// feed is core-side (processing/luxsynth_feed.c, M4). Reads the Path-B engine
// tap, NOT the visualizer's localDataGray, so the bars match what LuxSynth
// actually hears regardless of the selected view.
// Results are stored in fftMagnitudesSmoothed_ with exponential smoothing.
// The KissFFT config is cached in fftCfg_ and reallocated only when
// cisPixelsCount changes.  All work is O(N log N) on the UI thread at 30 fps.
//==============================================================================
void CisVisualizerComponent::computeFftMagnitudes()
{
    if (cisPixelsCount == 0)
    {
        fftMagnitudes_.clear();
        fftMagnitudesSmoothed_.clear();
        fftHarmonicity_.clear();
        fftNumHarmonics_ = 0;
        return;
    }

    // ── Populate lxFft buffers from LuxSynth source (independent of visualizer) ─
    {
        auto* core = processor.getSp3ctraCore();
        auto* buffers = (core && core->isInitialized()) ? core->getAudioImageBuffers() : nullptr;
        if (!buffers || !buffers->initialized)
        {
            fftMagnitudes_.clear();
            fftMagnitudesSmoothed_.clear();
            fftHarmonicity_.clear();
            fftNumHarmonics_ = 0;
            return;
        }

        lxFftR_.resize(static_cast<size_t>(cisPixelsCount));
        lxFftG_.resize(static_cast<size_t>(cisPixelsCount));
        lxFftB_.resize(static_cast<size_t>(cisPixelsCount));
        lxFftGray_.resize(static_cast<size_t>(cisPixelsCount));

        uint8_t* pR = nullptr;
        uint8_t* pG = nullptr;
        uint8_t* pB = nullptr;

        // PER-CHAIN display (2026-07-10): the FFT view mirrors the exact frame
        // LuxSynth's pipeline consumed (engine input tap) — same routing as
        // the SYNTH_* head panels, no UI-side source-type guessing.
        audio_image_buffers_get_engine_input_pointers(
            buffers, AUDIO_IMAGE_ENGINE_TAP_PATHB, &pR, &pG, &pB);

        if (!pR || !pG || !pB)
        {
            std::fill(lxFftGray_.begin(), lxFftGray_.end(), uint8_t{255});
            std::fill(lxFftR_.begin(),    lxFftR_.end(),    uint8_t{255});
            std::fill(lxFftB_.begin(),    lxFftB_.end(),    uint8_t{255});
        }
        else
        {
            std::memcpy(lxFftR_.data(), pR, static_cast<size_t>(cisPixelsCount));
            std::memcpy(lxFftG_.data(), pG, static_cast<size_t>(cisPixelsCount));
            std::memcpy(lxFftB_.data(), pB, static_cast<size_t>(cisPixelsCount));

            // LuxSynth preprocessing — synth-split P1: per-OUT bank (slot 0),
            // the same values preprocess_luxsynth() consumes. Intensity is the
            // pre-FFT mix weight of the send (1.0 = parity).
            const int doInvert  = g_sp3ctra_config.luxsynth_out[0].negative;
            const int doDcBlock = g_sp3ctra_config.luxsynth_out[0].dc_blocking;
            const float gammaVal = g_sp3ctra_config.luxsynth_out[0].gamma;
            const int gammaOn   = (gammaVal > 0.0f && gammaVal != 1.0f) ? 1 : 0;
            float outIntensity  = g_sp3ctra_config.luxsynth_out[0].intensity;
            if (outIntensity < 0.0f) outIntensity = 0.0f;

            thread_local std::vector<float> grayF;
            grayF.resize(static_cast<size_t>(cisPixelsCount));

            // Pass 1: grayscale + inversion
            for (int i = 0; i < cisPixelsCount; ++i)
            {
                float gray = (0.299f * static_cast<float>(lxFftR_[i])
                            + 0.587f * static_cast<float>(lxFftG_[i])
                            + 0.114f * static_cast<float>(lxFftB_[i])) / 255.0f;
                if (gray < 0.0f) gray = 0.0f;
                if (gray > 1.0f) gray = 1.0f;
                if (doInvert) gray = 1.0f - gray;
                grayF[static_cast<size_t>(i)] = gray;
            }

            // Pass 2: DC blocking
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

            // Pass 3: gamma + per-OUT intensity + quantisation
            for (int i = 0; i < cisPixelsCount; ++i)
            {
                float gray = grayF[static_cast<size_t>(i)];
                if (gammaOn && gammaVal > 0.0f)
                    gray = std::pow(gray, 1.0f / gammaVal);
                gray *= outIntensity;
                if (gray < 0.0f) gray = 0.0f;
                if (gray > 1.0f) gray = 1.0f;
                lxFftGray_[static_cast<size_t>(i)] =
                    static_cast<uint8_t>(gray * 255.0f + 0.5f);
            }
        }
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

    // ── Per-chain transport gate (P4 — ONE authority) ────────────────────────
    // The display spectrum follows the transport of the chain that FEEDS
    // LuxSynth (first "→ LUXSYNTH" OUT, else "→ LUXWAVE") — the same
    // chain_send_transport rule that gates the engine feed at staging time.
    //   PLAY → recompute, HOLD → keep last spectrum, STOP → silence.
    int kChain2Freeze = 0;
    {
        ChainPlan fftPlan;
        chain_plan_get(&fftPlan);
        const SynthChainPlan* fg = nullptr;
        const SynthChainPlan* lw = nullptr;
        for (int c = 0; c < fftPlan.num_chains && fg == nullptr; ++c)
        {
            const SynthChainPlan& sp = fftPlan.chain[c];
            if (! sp.present) continue;
            for (int i = 0; i < sp.num_inserts; ++i)
            {
                if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                { fg = &sp; break; }
                if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE
                    && lw == nullptr)
                    lw = &sp;
            }
        }
        if (fg == nullptr) fg = lw;
        if (fg != nullptr)
        {
            int fade = 0;
            chain_send_transport(fg, &kChain2Freeze, &fade);
        }
    }

    if (kChain2Freeze == 0)   // PLAY
    {
    // ── Hann window ───────────────────────────────────────────────────────────
    const float kTwoPiOverN =
        2.0f * static_cast<float>(M_PI) / static_cast<float>(juce::jmax(1, N - 1));
    for (int i = 0; i < N; ++i)
    {
        const float hann = 0.5f * (1.0f - std::cos(kTwoPiOverN * static_cast<float>(i)));
        inBuf[static_cast<size_t>(i)] =
            (static_cast<float>(lxFftGray_[static_cast<size_t>(i)]) / 255.0f) * hann;
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
    if (!lxFftR_.empty() && !lxFftB_.empty())
    {
        // ── Subtract global CIS sensor R-B bias ───────────────────────────────
        // The CIS sensor has a fixed warm bias (R > B globally on white surfaces).
        // Subtracting the per-frame global mean centres harmonicity around 0.5 so
        // that only *local* colour variations drive the per-bin values.
        float globalR = 0.0f, globalB = 0.0f;
        for (int i = 0; i < cisPixelsCount; ++i)
        {
            globalR += static_cast<float>(lxFftR_[static_cast<size_t>(i)]);
            globalB += static_cast<float>(lxFftB_[static_cast<size_t>(i)]);
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
                sumR += static_cast<float>(lxFftR_[static_cast<size_t>(i)]);
                sumB += static_cast<float>(lxFftB_[static_cast<size_t>(i)]);
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
    }   // end PLAY (Chain 2 transport)
    else if (kChain2Freeze == 2)   // STOP — silence the spectrum
    {
        std::fill(fftMagnitudesSmoothed_.begin(), fftMagnitudesSmoothed_.end(), 0.0f);
    }
    // HOLD (1): leave fftMagnitudesSmoothed_ / fftHarmonicity_ at their last
    // PLAY values so the frozen timbre keeps sounding while MIDI notes play.

    // ========================================================================
    // M4/D2 — the spectral ENGINE feed now lives in the core
    // (processing/luxsynth_feed.c, audio thread): it mixes the staged
    // "→ LUXSYNTH" sends and pushes the FFT even with the editor closed.
    // The FFT computed above is DISPLAY-ONLY (this view). Only the engine
    // CONFIG sync remains here (UI → engine, non-RT, ~30 fps).
    // ========================================================================
    if (luxsynth_are_buffers_ready() && nDisplay > 0)
    {
        // ── Sync engine config from APVTS (non-RT, ~30 fps) ──────────────────
        auto& apvts = processor.getAPVTS();
        LuxSynthConfig cfg;
        cfg.attack_ms            = apvts.getRawParameterValue("luxsynthAttackMs")->load();
        cfg.decay_ms             = apvts.getRawParameterValue("luxsynthDecayMs")->load();
        cfg.sustain_level        = apvts.getRawParameterValue("luxsynthSustainLevel")->load();
        cfg.release_ms           = apvts.getRawParameterValue("luxsynthReleaseMs")->load();
        cfg.attack_curve         = apvts.getRawParameterValue("luxsynthAttackCurve")->load();
        cfg.decay_curve          = apvts.getRawParameterValue("luxsynthDecayCurve")->load();
        cfg.release_curve        = apvts.getRawParameterValue("luxsynthReleaseCurve")->load();
        cfg.filter_attack_ms     = apvts.getRawParameterValue("luxsynthFilterAttackMs")->load();
        cfg.filter_decay_ms      = apvts.getRawParameterValue("luxsynthFilterDecayMs")->load();
        cfg.filter_sustain       = apvts.getRawParameterValue("luxsynthFilterSustain")->load();
        cfg.filter_release_ms    = apvts.getRawParameterValue("luxsynthFilterReleaseMs")->load();
        cfg.filter_attack_curve  = apvts.getRawParameterValue("luxsynthFilterAttackCurve")->load();
        cfg.filter_decay_curve   = apvts.getRawParameterValue("luxsynthFilterDecayCurve")->load();
        cfg.filter_release_curve = apvts.getRawParameterValue("luxsynthFilterReleaseCurve")->load();
        cfg.filter_cutoff        = apvts.getRawParameterValue("luxsynthFilterCutoff")->load();
        cfg.filter_env_depth     = apvts.getRawParameterValue("luxsynthFilterEnvDepth")->load();
        cfg.lfo_rate_hz          = apvts.getRawParameterValue("luxsynthLfoRate")->load();
        cfg.lfo_depth_semitones  = apvts.getRawParameterValue("luxsynthLfoDepth")->load();
        cfg.num_oscillators      = static_cast<int>(
            apvts.getRawParameterValue("luxsynthNumOscillators")->load());
        cfg.master_volume        = 0.20f; // legacy default — attenuate additive sum before hard clip
        cfg.sample_rate          = g_luxsynth_engine.sample_rate;
        cfg.buffer_size          = static_cast<int>(
            g_luxsynth_engine.sample_rate > 0 ? g_luxsynth_engine.sample_rate / 30.0f : 512);
        cfg.enabled              = apvts.getRawParameterValue("luxsynthEnabled")->load() > 0.5f;

        luxsynth_engine_set_config(&g_luxsynth_engine, &cfg);
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
    g.drawText(juce::String::fromUTF8("LuxSynth harmonics  \xe2\x80\x94 color = harmonicity"),
               juce::Rectangle<int>(4, H - 16, W - 8, 13),
               juce::Justification::centredLeft, false);
}
