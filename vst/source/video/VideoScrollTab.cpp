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
    sourceCombo_.addItem("L  (Live CIS)", 1);
    sourceCombo_.addItem("Sample",        2);
    sourceCombo_.addItem("Mix",           3);
    sourceCombo_.addItem("LuxPitch",      4);
    sourceCombo_.setTooltip("Image source fed to the video scroll window.");
    addAndMakeVisible(sourceCombo_);
    sourceAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "videoScrollSource", sourceCombo_);

    // ── Scroll Mode ───────────────────────────────────────────────────────────
    modeCombo_.addItem("Live L->R  (left to right)",    1);
    modeCombo_.addItem("Live R->L  (right to left)",    2);
    modeCombo_.addItem("Live Dual  (ping-pong L<->R)",  3);
    modeCombo_.addItem("Seq. Loop Simple  (A->B loop)", 4);
    modeCombo_.addItem("Seq. Ping-Pong   (A->B->A)",   5);
    modeCombo_.addItem("Seq. One-Shot    (A->B stop)",  6);
    modeCombo_.setTooltip("Scroll direction and sequencer loop mode.");
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

    // Badge
    g.setColour(juce::Colour(0xff1a3020u));
    g.fillRoundedRectangle(juce::Rectangle<int>(kHP, 10, W - 2*kHP, kSecH).toFloat(), 3.f);
    g.setColour(juce::Colour(0xff66cc88u));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("VIDEO SCROLL  --  Image Scroll Visualization",
               kHP+6, 10, W - 2*kHP - 12, kSecH, juce::Justification::centredLeft, true);

    // Label helper
    auto drawLabel = [&](const juce::String& txt, int y)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(Sp3ctraTheme::kColText));
        g.drawText(txt, juce::Rectangle<int>(kHP, y, kLW, kCH),
                   juce::Justification::centredRight, true);
    };

    // Section heading helper
    auto drawSection = [&](const juce::String& txt, int y)
    {
        g.setColour(juce::Colour(0xff66cc88u));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(txt, kHP, y, 160, kCH, juce::Justification::centredLeft, true);
    };

    // Layout mirrors resized()
    const int secEnable = 10 + kSecH + kSecG;
    drawLabel("Enable", secEnable);

    const int secSrc = secEnable + kStep + 10;
    drawSection("SOURCE", secSrc);
    drawLabel("Source", secSrc + kStep);

    const int secScroll = secSrc + 2*kStep + 10;
    drawSection("SCROLL", secScroll);
    drawLabel("Mode",       secScroll + kStep);
    drawLabel("Speed",      secScroll + 2*kStep);
    drawLabel("Direction",  secScroll + 3*kStep);
    drawLabel("Max Frames", secScroll + 4*kStep);

    const int secDisplay = secScroll + 5*kStep + 10;
    drawSection("DISPLAY", secDisplay);
    drawLabel("Zoom",  secDisplay + kStep);
    drawLabel("Blend", secDisplay + 2*kStep);

    // Window section status dot
    const int secWin = secDisplay + 3*kStep + 10;
    drawSection("WINDOW", secWin);
    const bool open = (videoWindow_ != nullptr && videoWindow_->isVisible());
    g.setColour(open ? juce::Colour(0xff44cc66u) : juce::Colour(0xff666666u));
    g.fillEllipse(kHP + 80.f, (float)secWin + 7.f, 8.f, 8.f);
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    g.setColour(juce::Colour(Sp3ctraTheme::kColTextMuted));
    g.drawText(open ? "open" : "closed", kHP + 92, secWin, 60, kCH,
               juce::Justification::centredLeft, true);
}

//==============================================================================
void VideoScrollTab::resized()
{
    const int ctrlX = kHP + kLW + kGap;
    const int ctrlW = juce::jmax(160, getWidth() / 2 - ctrlX + kHP);

    const int secEnable = 10 + kSecH + kSecG;
    enableToggle_.setBounds(ctrlX, secEnable, 90, kCH);

    const int secSrc = secEnable + kStep + 10;
    sourceCombo_.setBounds(ctrlX, secSrc + kStep, ctrlW + 50, kCH);

    const int secScroll = secSrc + 2*kStep + 10;
    modeCombo_     .setBounds(ctrlX, secScroll + kStep,   ctrlW + 50, kCH);
    speedSlider_   .setBounds(ctrlX, secScroll + 2*kStep, ctrlW,      kCH);
    directionCombo_.setBounds(ctrlX, secScroll + 3*kStep, ctrlW,      kCH);
    maxDurSlider_  .setBounds(ctrlX, secScroll + 4*kStep, ctrlW,      kCH);

    const int secDisplay = secScroll + 5*kStep + 10;
    zoomSlider_    .setBounds(ctrlX, secDisplay + kStep,   ctrlW, kCH);
    blendModeCombo_.setBounds(ctrlX, secDisplay + 2*kStep, ctrlW, kCH);

    const int secWin = secDisplay + 3*kStep + 10;
    const int btnY   = secWin + kStep;
    const int btnW   = 140;
    windowBtn_    .setBounds(kHP,             btnY, btnW, kCH + 4);
    fullscreenBtn_.setBounds(kHP + btnW + 8,  btnY, btnW, kCH + 4);
}
