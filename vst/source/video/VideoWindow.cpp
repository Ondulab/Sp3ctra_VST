#include "VideoWindow.h"
#include "../PluginProcessor.h"
#include "VideoScrollMode.h"

//==============================================================================
// ContentComponent
//==============================================================================

VideoWindow::ContentComponent::ContentComponent(Sp3ctraAudioProcessor& proc,
                                                VideoWindow& owner)
    : display(proc), proc_(proc), owner_(owner),
      fullscreenBtn_("[ ] Full Screen"),
      closeBtn_     ("x  Close")
{
    setOpaque(true);

    addAndMakeVisible(display);
    addAndMakeVisible(fullscreenBtn_);
    addAndMakeVisible(closeBtn_);

    // Toolbar button styling
    auto styleBtn = [](juce::TextButton& btn,
                       uint32_t bg = 0xff2a2a2a,
                       uint32_t fg = 0xffcccccc)
    {
        btn.setColour(juce::TextButton::buttonColourId,  juce::Colour(bg));
        btn.setColour(juce::TextButton::textColourOffId, juce::Colour(fg));
    };
    styleBtn(fullscreenBtn_, 0xff1a3020, 0xff66cc88);
    styleBtn(closeBtn_,      0xff2a1818, 0xffcc6666);

    fullscreenBtn_.onClick = [this] { owner_.toggleFullscreen(); };
    closeBtn_.onClick      = [this] { owner_.closeButtonPressed(); };

    // Wire double-click on the display to fullscreen
    display.onFullscreenRequested = [this] { owner_.toggleFullscreen(); };
}

void VideoWindow::ContentComponent::paint(juce::Graphics& g)
{
    const bool fs = owner_.isFullScreen();

    if (!fs)
    {
        // Toolbar background
        g.setColour(juce::Colour(0xff1a1a1a));
        g.fillRect(0, 0, getWidth(), kToolbarH);

        g.setColour(juce::Colour(0xff333333));
        g.drawLine(0.f, (float)kToolbarH, (float)getWidth(), (float)kToolbarH, 1.f);

        // Mode label — read current scroll mode from APVTS
        const int modeVal = static_cast<int>(
            proc_.getAPVTS().getRawParameterValue("videoScrollMode")->load());
        const VideoScrollMode mode = static_cast<VideoScrollMode>(
            juce::jlimit(0, (int)VideoScrollMode::COUNT - 1, modeVal));

        juce::String label = juce::String("VIDEO  |  ") + videoScrollModeLabel(mode);

        g.setColour(juce::Colour(0xff66cc88));
        g.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
        g.drawText(label, 8, 0, getWidth() - 280, kToolbarH,
                   juce::Justification::centredLeft, true);
    }

    // Waterfall area background (always)
    g.setColour(juce::Colours::black);
    g.fillRect(0, fs ? 0 : kToolbarH, getWidth(), getHeight() - (fs ? 0 : kToolbarH));
}

void VideoWindow::ContentComponent::resized()
{
    const bool fs    = owner_.isFullScreen();
    const int  btnH  = kToolbarH - 4;
    const int  btnW  = 130;
    const int  yOff  = 2;

    // In fullscreen: hide toolbar buttons, display occupies full content area
    if (fs)
    {
        fullscreenBtn_.setVisible(false);
        closeBtn_     .setVisible(false);
        display.setBounds(0, 0, getWidth(), getHeight());
    }
    else
    {
        fullscreenBtn_.setVisible(true);
        closeBtn_     .setVisible(true);
        fullscreenBtn_.setBounds(getWidth() - (btnW + 4 + btnW + 4), yOff, btnW, btnH);
        closeBtn_     .setBounds(getWidth() - (btnW + 4),             yOff, btnW, btnH);
        display.setBounds(0, kToolbarH, getWidth(), getHeight() - kToolbarH);
    }
}

//==============================================================================
// VideoWindow
//==============================================================================

VideoWindow::VideoWindow(Sp3ctraAudioProcessor& processor)
    : juce::DocumentWindow("Sp3ctra - Video Scroll",

                           juce::Colour(0xff1a1a1a),
                           DocumentWindow::allButtons),
      processor_(processor)
{
    setUsingNativeTitleBar(true);
    setResizable(true, true);

    // Read saved dimensions from APVTS (or default 800 × 600)
    auto& apvts = processor_.getAPVTS();
    const int w = juce::jlimit(320, 2560,
        static_cast<int>(apvts.getRawParameterValue("videoWindowWidth")->load()));
    const int h = juce::jlimit(240, 1440,
        static_cast<int>(apvts.getRawParameterValue("videoWindowHeight")->load()));

    // Create and set content (DocumentWindow takes ownership)
    auto* c = new ContentComponent(processor_, *this);
    content_ = c;
    setContentOwned(c, true);

    centreWithSize(w, h);
    setVisible(true);
}

VideoWindow::~VideoWindow()
{
    // Save current window dimensions to APVTS before destroying
    if (!isFullScreen())
    {
        auto& apvts = processor_.getAPVTS();
        if (auto* p = apvts.getParameter("videoWindowWidth"))
            p->setValueNotifyingHost(
                p->convertTo0to1(juce::jlimit(320.f, 2560.f, (float)getWidth())));
        if (auto* p = apvts.getParameter("videoWindowHeight"))
            p->setValueNotifyingHost(
                p->convertTo0to1(juce::jlimit(240.f, 1440.f, (float)getHeight())));
    }
}

void VideoWindow::closeButtonPressed()
{
    // Notify processor that video scroll is disabled
    auto& apvts = processor_.getAPVTS();
    if (auto* p = apvts.getParameter("videoScrollEnabled"))
        p->setValueNotifyingHost(0.0f);

    setVisible(false);
}

void VideoWindow::toggleFullscreen()
{
    if (isFullScreen())
    {
        setFullScreen(false);
        auto& apvts = processor_.getAPVTS();
        const int w = juce::jlimit(320, 2560,
            static_cast<int>(apvts.getRawParameterValue("videoWindowWidth")->load()));
        const int h = juce::jlimit(240, 1440,
            static_cast<int>(apvts.getRawParameterValue("videoWindowHeight")->load()));
        centreWithSize(w, h);
    }
    else
    {
        setFullScreen(true);
    }
}

VideoDisplayComponent* VideoWindow::getDisplay() noexcept
{
    return content_ ? &content_->display : nullptr;
}
