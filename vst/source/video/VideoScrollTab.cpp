#include "VideoScrollTab.h"
#include "VideoWindow.h"

namespace
{
    constexpr int kHP   = Sp3ctraTheme::kHPad;
    constexpr int kLW   = Sp3ctraTheme::kLabelW;
    constexpr int kGap  = Sp3ctraTheme::kGap;
    constexpr int kCH   = Sp3ctraTheme::kControlH;
    constexpr int kStep = Sp3ctraTheme::kRowStep;
    constexpr int kTbW  = Sp3ctraTheme::kTbStd;
    constexpr int kTbH  = Sp3ctraTheme::kTextBoxH;
    constexpr int kSecH = Sp3ctraTheme::kSectionH;
    constexpr int kSecG = Sp3ctraTheme::kSectionGap;

    void styleSliderH(juce::Slider& s, const juce::String& suffix = "")
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, kTbW, kTbH);
        if (suffix.isNotEmpty()) s.setTextValueSuffix(suffix);
    }

    // ── Responsive vertical layout ──────────────────────────────────────────
    // The panel must survive a large reduction of its host zone — in BOTH
    // axes — without clipping rows (vertically) or controls (horizontally).
    // Every metric is derived from the available width/height so paint() and
    // resized() stay byte-for-byte in sync and the whole stack always fits.
    struct VScrollLayout
    {
        int top, secH, secG, step, ch, secExtra, btnExtra;
        int labelW, ctrlX, ctrlW;
        int secSrc, ySource;
        int secScroll, yMode, ySpeed, yLinePos;
        int secDisplay, yThickness, yZoom, yFade, yCompress;
    };

    VScrollLayout computeLayout(int width, int height)
    {
        // Natural (un-scaled) full height of the stack, used as the reference
        // point — below this the layout compresses; above it, it sits at top.
        constexpr float kFullH = 420.0f;
        const float f = juce::jlimit(0.5f, 1.0f, (float) height / kFullH);
        auto S = [f](int v) { return juce::roundToInt((float) v * f); };

        VScrollLayout L;
        L.top      = S(10);
        L.secH     = S(kSecH);  // 24
        L.secG     = S(kSecG);  // kSectionGap (4)
        L.step     = S(kStep);  // 32
        L.ch       = S(kCH);    // 22
        L.secExtra = S(10);     // gap above each section heading
        L.btnExtra = S(4);      // action buttons are slightly taller than kCH

        // ── Horizontal: condense the label column and let controls fill all
        //    remaining width up to the right margin, so nothing is truncated
        //    when the panel is docked narrow.
        L.labelW = juce::jlimit(64, kLW, width / 3);          // shrinks from 110
        L.ctrlX  = kHP + L.labelW + kGap;
        L.ctrlW  = juce::jmax(70, width - kHP - L.ctrlX);     // fill to right edge

        // SOURCE heading sits at the top of the scrollable control stack — the
        // window controls (open/close/fullscreen + status dot) live in the
        // column header above this viewport, not here.
        L.secSrc     = L.top + L.secExtra;
        L.ySource    = L.secSrc + L.step;
        // SCROLL: heading + Mode, Speed, Line Pos
        L.secScroll  = L.secSrc + 2 * L.step + L.secExtra;
        L.yMode      = L.secScroll + L.step;
        L.ySpeed     = L.secScroll + 2 * L.step;
        L.yLinePos   = L.secScroll + 3 * L.step;
        // DISPLAY: heading + Thickness, Zoom, Fade, Compression
        L.secDisplay = L.secScroll + 4 * L.step + L.secExtra;
        L.yThickness = L.secDisplay + L.step;
        L.yZoom      = L.secDisplay + 2 * L.step;
        L.yFade      = L.secDisplay + 3 * L.step;
        L.yCompress  = L.secDisplay + 4 * L.step;
        return L;
    }
} // namespace

//==============================================================================
VideoScrollTab::VideoScrollTab(Sp3ctraAudioProcessor& processor)
    : processor_(processor)
{
    auto& apvts = processor_.getAPVTS();

    // ── Source ────────────────────────────────────────────────────────────────
    // The waterfall follows the IMAGE INPUT of a synthesis engine — the taps
    // published by the chain executors (AUDIO_IMAGE_ENGINE_TAP_*):
    //   - LuxStral          → engine tap A (mix of the → LUXSTRAL sends)
    //   - LuxSynth/LuxWave  → Path-B tap (LuxWave shares it)
    //   - AllSynth          → 50/50 luminance blend of the two streams above
    sourceCombo_.addItem("LuxStral",         1);
    sourceCombo_.addItem("LuxSynth/LuxWave", 2);
    sourceCombo_.addItem("AllSynth",         3);
    sourceCombo_.setTooltip(
        "Which synthesis engine's image input to visualize.\n"
        "  - LuxStral          → image fed to LuxStral synthesis\n"
        "  - LuxSynth/LuxWave  → image fed to LuxSynth and LuxWave (shared)\n"
        "  - AllSynth          → 50/50 blend of both streams above\n"
        "The waterfall always matches what the audio engine actually processes.");


    addAndMakeVisible(sourceCombo_);
    sourceAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "videoScrollSource", sourceCombo_);

    // Scroll Orientation
    modeCombo_.addItem("Scroll up",    1);
    modeCombo_.addItem("Scroll left",  2);
    modeCombo_.addItem("Scroll down",  3);
    modeCombo_.addItem("Scroll right", 4);
    modeCombo_.setTooltip(
        "Orientation of the waterfall scroll.\n"
        "90 deg is the classic L->R scanner view.\n"
        "270 deg is the R->L view.");

    addAndMakeVisible(modeCombo_);
    modeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "videoScrollMode", modeCombo_);

    // ── Speed (bipolar) ───────────────────────────────────────────────────────
    // One control replaces the old Speed slider + Direction dropdown:
    //   negative = reverse, 0 = frozen, positive = forward (exponential feel).
    styleSliderH(speedSlider_, "");
    speedSlider_.setTooltip(
        "Scroll speed AND direction in one control.\n"
        "  < 0  reverse   |   0  frozen   |   > 0  forward\n"
        "The magnitude is exponential (gentle near the centre).");
    addAndMakeVisible(speedSlider_);
    speedAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollSpeed", speedSlider_);

    // ── Line Position (birth line) ────────────────────────────────────────────
    styleSliderH(linePosSlider_, "");
    linePosSlider_.setTooltip(
        "Where new scanlines are born, and the axis the image scrolls around.\n"
        "  -1  far edge   |   0  centre (symmetric bidirectional)   |   +1  near edge\n"
        "+1 reproduces the classic single-direction waterfall.");
    addAndMakeVisible(linePosSlider_);
    linePosAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollLinePos", linePosSlider_);

    // ── Line Thickness ────────────────────────────────────────────────────────
    styleSliderH(thicknessSlider_, "");
    thicknessSlider_.setTooltip(
        "Thickness of each freshly-drawn scanline.\n"
        "0 = single pixel, 1 = full viewport (barcode mode).");
    addAndMakeVisible(thicknessSlider_);
    thicknessAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollLineThickness", thicknessSlider_);

    // ── Zoom (live) ───────────────────────────────────────────────────────────
    styleSliderH(zoomSlider_, " x");
    zoomSlider_.setTooltip("Zoom factor (1.0 = fit, >1 = zoomed in around centre).");
    addAndMakeVisible(zoomSlider_);
    zoomAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollZoom", zoomSlider_);

    // ── Fade / persistence ────────────────────────────────────────────────────
    styleSliderH(fadeSlider_, "");
    fadeSlider_.setTooltip(
        "Progressive aging of the image as it moves AWAY from the source line:\n"
        "the farther a scanline has travelled, the more it desaturates (loses\n"
        "colour) and dims (loses brightness).\n"
        "0 = no aging (uniform), 1 = strong fade to dark grey at the far edge.");
    addAndMakeVisible(fadeSlider_);
    fadeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollFade", fadeSlider_);

    // ── Compression (temporal) ────────────────────────────────────────────────
    styleSliderH(maxDurSlider_, " fr");
    maxDurSlider_.setTooltip(
        "Non-linear time squish: the image stays at full scale near the source\n"
        "line and is progressively COMPRESSED as it moves away, so older time\n"
        "is packed tighter toward the far edge (and softly blurred).\n"
        "1 = linear (no squish), higher = more history squeezed on screen.");
    addAndMakeVisible(maxDurSlider_);
    maxDurAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollMaxDuration", maxDurSlider_);

    updateUIFromState();
}

VideoScrollTab::~VideoScrollTab()
{
    videoWindow_.reset();
}

//==============================================================================
void VideoScrollTab::onTabActivated()
{
    // The window is no longer auto-opened on activation — it is opened on
    // demand from the column header's window icons. We only refresh state.
    updateUIFromState();
}

void VideoScrollTab::onTabDeactivated() {}

//==============================================================================
void VideoScrollTab::openVideoWindow()
{
    if (!videoWindow_)
    {
        videoWindow_ = std::make_unique<VideoWindow>(processor_);
        // The window's own [✕] close button asks us to tear it down. Defer the
        // reset so we never delete the window from inside its own callback.
        videoWindow_->onCloseRequested = [this]
        {
            juce::MessageManager::callAsync([this] { closeVideoWindow(); });
        };
    }
    else { videoWindow_->setVisible(true); videoWindow_->toFront(true); }
    updateUIFromState();
}

void VideoScrollTab::closeVideoWindow()
{
    videoWindow_.reset();
    updateUIFromState();
}

void VideoScrollTab::toggleDetachedWindow()
{
    if (isVideoWindowOpen()) closeVideoWindow();
    else                     openVideoWindow();
}

void VideoScrollTab::requestFullscreenWindow()
{
    if (!videoWindow_) openVideoWindow();
    if (videoWindow_) videoWindow_->toggleFullscreen();
}

bool VideoScrollTab::isVideoWindowOpen() const noexcept
{
    return videoWindow_ != nullptr && videoWindow_->isVisible();
}

void VideoScrollTab::updateUIFromState()
{
    repaint();
    if (onWindowStateChanged) onWindowStateChanged();
}

//==============================================================================
void VideoScrollTab::paint(juce::Graphics& g)
{
    const int W = getWidth();
    const VScrollLayout L = computeLayout(W, getHeight());

    // Label helper
    auto drawLabel = [&](const juce::String& txt, int y)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(Sp3ctraTheme::kColText));
        g.drawText(txt, juce::Rectangle<int>(kHP, y, L.labelW, L.ch),
                   juce::Justification::centredRight, true);
    };

    // Section heading helper
    auto drawSection = [&](const juce::String& txt, int y)
    {
        g.setColour(juce::Colour(0xff66cc88u));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(txt, kHP, y, juce::jmin(160, W - 2*kHP), L.ch,
                   juce::Justification::centredLeft, true);
    };

    // Layout mirrors resized()
    drawSection("SOURCE", L.secSrc);
    drawLabel("Source", L.ySource);

    drawSection("SCROLL", L.secScroll);
    drawLabel("Mode",     L.yMode);
    drawLabel("Speed",    L.ySpeed);
    drawLabel("Line Pos", L.yLinePos);

    drawSection("DISPLAY", L.secDisplay);
    drawLabel("Thickness",   L.yThickness);
    drawLabel("Zoom",        L.yZoom);
    drawLabel("Fade",        L.yFade);
    drawLabel("Compression", L.yCompress);
}

//==============================================================================
void VideoScrollTab::resized()
{
    const int W = getWidth();
    const VScrollLayout L = computeLayout(W, getHeight());

    // Every combo and slider spans from the label column to the right margin,
    // so all dropdowns/value-boxes line up and nothing overflows when narrow.
    const int ctrlX = L.ctrlX;
    const int ctrlW = L.ctrlW;

    // Value boxes shrink with the row so the slider track always has room.
    const int tbW = juce::jlimit(48, kTbW, ctrlW / 3);
    auto setTb = [&](juce::Slider& s) {
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, tbW, L.ch);
    };
    setTb(speedSlider_);
    setTb(linePosSlider_);
    setTb(thicknessSlider_);
    setTb(zoomSlider_);
    setTb(fadeSlider_);
    setTb(maxDurSlider_);

    sourceCombo_.setBounds(ctrlX, L.ySource, ctrlW, L.ch);

    modeCombo_      .setBounds(ctrlX, L.yMode,    ctrlW, L.ch);
    speedSlider_    .setBounds(ctrlX, L.ySpeed,   ctrlW, L.ch);
    linePosSlider_  .setBounds(ctrlX, L.yLinePos, ctrlW, L.ch);

    thicknessSlider_.setBounds(ctrlX, L.yThickness, ctrlW, L.ch);
    zoomSlider_     .setBounds(ctrlX, L.yZoom,      ctrlW, L.ch);
    fadeSlider_     .setBounds(ctrlX, L.yFade,      ctrlW, L.ch);
    maxDurSlider_   .setBounds(ctrlX, L.yCompress,  ctrlW, L.ch);
}
