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
        int yEnable;
        int secSrc, ySource;
        int secScroll, yMode, ySpeed, yDir, yMax;
        int secDisplay, yZoom, yBlend;
        int secWin, yButtons;
    };

    VScrollLayout computeLayout(int width, int height)
    {
        // Natural (un-scaled) full height of the stack, used as the reference
        // point — below this the layout compresses; above it, it sits at top.
        constexpr float kFullH = 498.0f;
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

        L.yEnable    = L.top + L.secH + L.secG;
        L.secSrc     = L.yEnable + L.step + L.secExtra;
        L.ySource    = L.secSrc + L.step;
        L.secScroll  = L.secSrc + 2 * L.step + L.secExtra;
        L.yMode      = L.secScroll + L.step;
        L.ySpeed     = L.secScroll + 2 * L.step;
        L.yDir       = L.secScroll + 3 * L.step;
        L.yMax       = L.secScroll + 4 * L.step;
        L.secDisplay = L.secScroll + 5 * L.step + L.secExtra;
        L.yZoom      = L.secDisplay + L.step;
        L.yBlend     = L.secDisplay + 2 * L.step;
        L.secWin     = L.secDisplay + 3 * L.step + L.secExtra;
        L.yButtons   = L.secWin + L.step;
        return L;
    }
} // namespace

//==============================================================================
VideoScrollTab::VideoScrollTab(Sp3ctraAudioProcessor& processor)
    : processor_(processor)
{
    auto& apvts = processor_.getAPVTS();

    // ── Enable ────────────────────────────────────────────────────────────────
    enableToggle_.setTooltip("Enable video scroll window.");
    addAndMakeVisible(enableToggle_);
    enableAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoScrollEnabled", enableToggle_);
    enableToggle_.onStateChange = [this] { updateUIFromState(); };

    // ── Source ────────────────────────────────────────────────────────────────
    // The waterfall now follows the IMAGE INPUT of a synthesis engine — not a
    // raw bus selector.  Each option mirrors what the engine actually "sees":
    //   - LuxStral          → reads luxstral_source_type (S/L/M/P/K) routing
    //   - LuxSynth/LuxWave  → reads luxsynth_source_type (LuxWave shares it)
    //   - AllSynth          → 50/50 luminance blend of the two streams above
    sourceCombo_.addItem("LuxStral",         1);
    sourceCombo_.addItem("LuxSynth/LuxWave", 2);
    sourceCombo_.addItem("AllSynth",         3);
    sourceCombo_.setTooltip(
        "Which synthesis engine's image input to visualize.\n"
        "Follows each engine's own source routing (set in IMAGE tab):\n"
        "  - LuxStral          → image fed to LuxStral synthesis\n"
        "  - LuxSynth/LuxWave  → image fed to LuxSynth and LuxWave (shared)\n"
        "  - AllSynth          → 50/50 blend of both streams above\n"
        "The waterfall always matches what the audio engine actually processes,\n"
        "regardless of which view is selected in IMAGE > SOURCES.");


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

    // ── Speed ─────────────────────────────────────────────────────────────────
    styleSliderH(speedSlider_, " x");
    speedSlider_.setTooltip("Scroll speed multiplier.");
    addAndMakeVisible(speedSlider_);
    speedAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollSpeed", speedSlider_);

    // ── Direction ─────────────────────────────────────────────────────────────
    directionCombo_.addItem("Forward", 1);
    directionCombo_.addItem("Reverse", 2);
    directionCombo_.setTooltip("Playback direction.");
    addAndMakeVisible(directionCombo_);
    directionAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "videoScrollDirection", directionCombo_);

    // ── Max Frames (seq recording limit) ──────────────────────────────────────
    styleSliderH(maxDurSlider_, " fr");
    maxDurSlider_.setTooltip(
        "Maximum number of frames recorded before Seq playback starts.\n"
        "~1000 fr = 1 second of CIS data at real-time speed.");
    addAndMakeVisible(maxDurSlider_);
    maxDurAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollMaxDuration", maxDurSlider_);

    // ── Zoom (live) ───────────────────────────────────────────────────────────
    styleSliderH(zoomSlider_, " x");
    zoomSlider_.setTooltip("Zoom factor (1.0 = fit, >1 = zoomed).");
    addAndMakeVisible(zoomSlider_);
    zoomAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollZoom", zoomSlider_);

    // ── Blend Mode ────────────────────────────────────────────────────────────
    blendModeCombo_.addItem("Mix    (weighted average)", 1);
    blendModeCombo_.addItem("Add    (luminosity boost)", 2);
    blendModeCombo_.addItem("Screen (Photoshop screen)", 3);
    blendModeCombo_.addItem("Mask   (multiplicative)",   4);
    blendModeCombo_.setTooltip("Blend mode for mixing sequenced frames.");
    addAndMakeVisible(blendModeCombo_);
    blendModeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "videoScrollBlendMode", blendModeCombo_);

    // ── Action buttons ────────────────────────────────────────────────────────
    windowBtn_.setButtonText("Open Window");
    windowBtn_.onClick = [this] { toggleVideoWindow(); };
    addAndMakeVisible(windowBtn_);

    fullscreenBtn_.setButtonText("[ ] Full Screen");
    fullscreenBtn_.onClick = [this] { requestFullscreen(); };
    addAndMakeVisible(fullscreenBtn_);

    auto styleBtn = [](juce::TextButton& btn, juce::uint32 bg, juce::uint32 fg)
    {
        btn.setColour(juce::TextButton::buttonColourId,  juce::Colour(bg));
        btn.setColour(juce::TextButton::textColourOffId, juce::Colour(fg));
    };
    styleBtn(windowBtn_,     0xff1e2e20u, 0xff88cc88u);
    styleBtn(fullscreenBtn_, 0xff1e202eu, 0xff88aaccu);

    apvts.addParameterListener("videoScrollEnabled", this);
    updateUIFromState();
}

VideoScrollTab::~VideoScrollTab()
{
    processor_.getAPVTS().removeParameterListener("videoScrollEnabled", this);
    videoWindow_.reset();
}

//==============================================================================
void VideoScrollTab::parameterChanged(const juce::String& paramID, float newValue)
{
    if (paramID == "videoScrollEnabled")
    {
        juce::MessageManager::callAsync([this, newValue]
        {
            if (newValue < 0.5f) closeVideoWindow();
            else if (!videoWindow_) openVideoWindow();
            updateUIFromState();
        });
    }
}

void VideoScrollTab::onTabActivated()
{
    if (enableToggle_.getToggleState() && !videoWindow_)
        openVideoWindow();
    updateUIFromState();
}

void VideoScrollTab::onTabDeactivated() {}

//==============================================================================
void VideoScrollTab::openVideoWindow()
{
    if (!videoWindow_)
        videoWindow_ = std::make_unique<VideoWindow>(processor_);
    else { videoWindow_->setVisible(true); videoWindow_->toFront(true); }
    updateUIFromState();
}

void VideoScrollTab::closeVideoWindow()
{
    videoWindow_.reset();
    updateUIFromState();
}

void VideoScrollTab::toggleVideoWindow()
{
    if (videoWindow_ && videoWindow_->isVisible()) closeVideoWindow();
    else openVideoWindow();
}

void VideoScrollTab::requestFullscreen()
{
    if (!videoWindow_) openVideoWindow();
    if (videoWindow_) videoWindow_->toggleFullscreen();
}

void VideoScrollTab::updateUIFromState()
{
    const bool open = (videoWindow_ != nullptr && videoWindow_->isVisible());
    windowBtn_.setButtonText(open ? "Close Window" : "Open Window");
    repaint();
}

//==============================================================================
void VideoScrollTab::paint(juce::Graphics& g)
{
    const int W = getWidth();
    const VScrollLayout L = computeLayout(W, getHeight());

    // Badge
    g.setColour(juce::Colour(0xff1a3020u));
    g.fillRoundedRectangle(juce::Rectangle<int>(kHP, L.top, W - 2*kHP, L.secH).toFloat(), 3.f);
    g.setColour(juce::Colour(0xff66cc88u));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("VIDEO SCROLL  --  Image Scroll Visualization",
               kHP+6, L.top, W - 2*kHP - 12, L.secH, juce::Justification::centredLeft, true);

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
    drawLabel("Enable", L.yEnable);

    drawSection("SOURCE", L.secSrc);
    drawLabel("Source", L.ySource);

    drawSection("SCROLL", L.secScroll);
    drawLabel("Mode",       L.yMode);
    drawLabel("Speed",      L.ySpeed);
    drawLabel("Direction",  L.yDir);
    drawLabel("Max Frames", L.yMax);

    drawSection("DISPLAY", L.secDisplay);
    drawLabel("Zoom",  L.yZoom);
    drawLabel("Blend", L.yBlend);

    // Window section status dot
    drawSection("WINDOW", L.secWin);
    const bool open = (videoWindow_ != nullptr && videoWindow_->isVisible());
    g.setColour(open ? juce::Colour(0xff44cc66u) : juce::Colour(0xff666666u));
    g.fillEllipse(kHP + 80.f, (float)L.secWin + (L.ch - 8) * 0.5f, 8.f, 8.f);
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    g.setColour(juce::Colour(Sp3ctraTheme::kColTextMuted));
    g.drawText(open ? "open" : "closed", kHP + 92, L.secWin, 60, L.ch,
               juce::Justification::centredLeft, true);
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
    setTb(maxDurSlider_);
    setTb(zoomSlider_);

    enableToggle_.setBounds(ctrlX, L.yEnable, juce::jmin(90, ctrlW), L.ch);

    sourceCombo_.setBounds(ctrlX, L.ySource, ctrlW, L.ch);

    modeCombo_     .setBounds(ctrlX, L.yMode,  ctrlW, L.ch);
    speedSlider_   .setBounds(ctrlX, L.ySpeed, ctrlW, L.ch);
    directionCombo_.setBounds(ctrlX, L.yDir,   ctrlW, L.ch);
    maxDurSlider_  .setBounds(ctrlX, L.yMax,   ctrlW, L.ch);

    zoomSlider_    .setBounds(ctrlX, L.yZoom,  ctrlW, L.ch);
    blendModeCombo_.setBounds(ctrlX, L.yBlend, ctrlW, L.ch);

    // Two action buttons share the full width so the second never clips.
    const int btnGap = 8;
    const int btnW   = juce::jmax(60, (W - 2*kHP - btnGap) / 2);
    windowBtn_    .setBounds(kHP,              L.yButtons, btnW, L.ch + L.btnExtra);
    fullscreenBtn_.setBounds(kHP + btnW + btnGap, L.yButtons, btnW, L.ch + L.btnExtra);
}
