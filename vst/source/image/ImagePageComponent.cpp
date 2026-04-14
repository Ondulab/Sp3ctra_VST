#include "ImagePageComponent.h"

using namespace Sp3ctraTheme;

// ── Colour palette ────────────────────────────────────────────────────────────
static const juce::Colour kLiveBadgeBg    { 0xff1c3755 };  // dark blue
static const juce::Colour kLiveBadgeFg    { 0xff7ab0f0 };
static const juce::Colour kLivePanelBg    { 0xff111c28 };
static const juce::Colour kLivePanelBdr   { 0xff263550 };

static const juce::Colour kSmpBadgeBg     { 0xff3a2208 };  // dark amber
static const juce::Colour kSmpBadgeFg     { 0xffffb060 };
static const juce::Colour kSmpPanelBg     { 0xff231808 };
static const juce::Colour kSmpPanelBdr    { 0xff503020 };

static const juce::Colour kBlobBadgeBg    { 0xff2c1f4a };  // purple
static const juce::Colour kBlobBadgeFg    { 0xffb07af0 };

static const juce::Colour kTransportPlay  { 0xff2a6040 };
static const juce::Colour kTransportHold  { 0xff6040a0 };
static const juce::Colour kTransportWhite { 0xff5a4020 };
static const juce::Colour kTransportPause { 0xff304070 };
static const juce::Colour kTransportOff   { 0xff2a2a2a };
static const juce::Colour kLabelColour    { 0xffb8c4d0 };

//==============================================================================
void ImagePageComponent::initSliderH(juce::Slider& s, const char* suffix)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, kTbStd, kTextBoxH);
    if (suffix) s.setTextValueSuffix(suffix);
}

//==============================================================================
ImagePageComponent::ImagePageComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    auto& apvts = processor.getAPVTS();

    // ── Left column: LIVE stream ──────────────────────────────────────────────
    initSliderH(liveOpacitySlider);
    addAndMakeVisible(liveOpacitySlider);
    liveOpacityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "imageLiveOpacity", liveOpacitySlider);

    initSliderH(gammaSlider);
    addAndMakeVisible(gammaSlider);
    gammaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralGammaValue", gammaSlider);

    initSliderH(contrastMinSlider);
    addAndMakeVisible(contrastMinSlider);
    contrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralContrastMin", contrastMinSlider);

    initSliderH(fadeInSlider, " ms");
    addAndMakeVisible(fadeInSlider);
    fadeInAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "imageFadeInMs", fadeInSlider);

    // Live transport buttons — PLAY=0, HOLD=1, WHITE=2
    auto setupLive = [this, &apvts](juce::TextButton& btn, int mode)
    {
        btn.onClick = [this, &apvts, mode]
        {
            apvts.getParameterAsValue("imageFreezeMode").setValue(mode);
            updateLiveTransportButtons();
        };
        addAndMakeVisible(btn);
    };
    setupLive(livePlayBtn,  0);
    setupLive(liveHoldBtn,  1);
    setupLive(liveWhiteBtn, 2);
    updateLiveTransportButtons();

    // ── Right column: SAMPLER stream ──────────────────────────────────────────
    initSliderH(samplerOpacitySlider);
    addAndMakeVisible(samplerOpacitySlider);
    samplerOpacityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "imageSamplerOpacity", samplerOpacitySlider);

    initSliderH(samplerGammaSlider);
    addAndMakeVisible(samplerGammaSlider);
    samplerGammaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "samplerGamma", samplerGammaSlider);

    initSliderH(samplerContrastMinSlider);
    addAndMakeVisible(samplerContrastMinSlider);
    samplerContrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "samplerContrastMin", samplerContrastMinSlider);

    initSliderH(samplerFadeInSlider, " ms");
    addAndMakeVisible(samplerFadeInSlider);
    samplerFadeInAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "samplerFadeInMs", samplerFadeInSlider);

    // Sampler transport — PLAY=0, HOLD=1, STOP=2
    // Also drives FrameSequencer so Sampler tab stays in sync.
    auto setupSampler = [this, &apvts](juce::TextButton& btn, int mode)
    {
        btn.onClick = [this, &apvts, mode]
        {
            apvts.getParameterAsValue("samplerFreezeMode").setValue(mode);
            if (auto* seq = processor.getFrameSequencer())
            {
                if (mode == 0)
                {
                    // Resume from the current step if paused, otherwise restart.
                    if (seq->isHeld())
                        seq->uiResume();
                    else
                        seq->uiPlay();
                }
                else if (mode == 1) seq->uiHold();
                else                seq->uiStop();
            }
            updateSamplerTransportButtons();
        };
        addAndMakeVisible(btn);
    };
    setupSampler(samplerPlayBtn, 0);
    setupSampler(samplerHoldBtn, 1);
    setupSampler(samplerStopBtn, 2);
    updateSamplerTransportButtons();

    startTimer(200);   // periodic refresh to stay in sync with Sampler tab changes

    // ── Blob detection section ────────────────────────────────────────────────
    blobViz = std::make_unique<BlobVisualizerComponent>(processor);
    addAndMakeVisible(blobViz.get());

    initSliderH(sfBlobThreshSlider);
    addAndMakeVisible(sfBlobThreshSlider);
    sfBlobThreshAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobBaseThreshold", sfBlobThreshSlider);

    initSliderH(sfMinWidthSlider);
    addAndMakeVisible(sfMinWidthSlider);
    sfMinWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMinWidth", sfMinWidthSlider);

    initSliderH(sfMergeGapSlider);
    addAndMakeVisible(sfMergeGapSlider);
    sfMergeGapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMergeGap", sfMergeGapSlider);
}

ImagePageComponent::~ImagePageComponent()
{
    stopTimer();
}

void ImagePageComponent::timerCallback()
{
    // Keep both columns in sync when the Sampler tab (TransportBarComponent)
    // changes samplerFreezeMode independently.
    updateLiveTransportButtons();
    updateSamplerTransportButtons();
}

//==============================================================================
void ImagePageComponent::updateLiveTransportButtons()
{
    const int mode = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("imageFreezeMode")->load());

    auto style = [](juce::TextButton& btn, bool active, juce::Colour col)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? col : kTransportOff);
        btn.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colours::white : juce::Colour(0xff888888));
    };
    style(livePlayBtn,  mode == 0, kTransportPlay);
    style(liveHoldBtn,  mode == 1, kTransportHold);
    style(liveWhiteBtn, mode == 2, kTransportWhite);
}

void ImagePageComponent::updateSamplerTransportButtons()
{
    const int mode = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("samplerFreezeMode")->load());

    auto style = [](juce::TextButton& btn, bool active, juce::Colour col)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? col : kTransportOff);
        btn.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colours::white : juce::Colour(0xff888888));
    };
    style(samplerPlayBtn, mode == 0, kTransportPlay);
    style(samplerHoldBtn, mode == 1, kTransportHold);
    style(samplerStopBtn, mode == 2, kTransportWhite);  // STOP = white/silence, same colour
}

//==============================================================================
void ImagePageComponent::paint(juce::Graphics& g)
{
    const int cw   = colW();
    const int lx   = colLX();
    const int rx   = colRX();
    const int rs   = kSectionH + kSectionGap;   // rows start Y below badge
    const int bsy  = blobSectionY();
    const int bcy  = blobContentY();
    const int vizH = blobVizH();
    const int totW = cw * 2 + kColGap;

    // ── Left panel: LIVE ──────────────────────────────────────────────────────
    g.setColour(kLivePanelBg);
    g.fillRoundedRectangle(
        juce::Rectangle<int>(lx, 0, cw, kTopSectionH).toFloat(), 4.f);
    g.setColour(kLivePanelBdr);
    g.drawRoundedRectangle(
        juce::Rectangle<int>(lx, 0, cw, kTopSectionH).toFloat(), 4.f, 1.f);

    // LIVE badge (blue)
    g.setColour(kLiveBadgeBg);
    g.fillRoundedRectangle(
        juce::Rectangle<int>(lx + 4, 4, cw - 8, kSectionH - 2).toFloat(), 3.f);
    g.setColour(kLiveBadgeFg);
    g.setFont(juce::Font(juce::FontOptions(kFontBadge)).boldened());
    g.drawText("LIVE",
               juce::Rectangle<int>(lx + 10, 4, cw - 20, kSectionH - 2),
               juce::Justification::centredLeft, true);

    // LIVE row labels
    g.setFont(juce::FontOptions(kFontBadge));
    g.setColour(kLabelColour);
    static const char* const liveLbls[] = {
        "Opacity", "Gamma", "Contrast Min", "Fade-In", "Transport"
    };
    for (int i = 0; i < 5; ++i)
        g.drawText(liveLbls[i],
                   juce::Rectangle<int>(lx, rs + i * kRowStep, kLabelW, kRowH),
                   juce::Justification::centredRight, true);

    // ── Right panel: SAMPLER ──────────────────────────────────────────────────
    g.setColour(kSmpPanelBg);
    g.fillRoundedRectangle(
        juce::Rectangle<int>(rx, 0, cw, kTopSectionH).toFloat(), 4.f);
    g.setColour(kSmpPanelBdr);
    g.drawRoundedRectangle(
        juce::Rectangle<int>(rx, 0, cw, kTopSectionH).toFloat(), 4.f, 1.f);

    // SAMPLER badge (amber)
    g.setColour(kSmpBadgeBg);
    g.fillRoundedRectangle(
        juce::Rectangle<int>(rx + 4, 4, cw - 8, kSectionH - 2).toFloat(), 3.f);
    g.setColour(kSmpBadgeFg);
    g.setFont(juce::Font(juce::FontOptions(kFontBadge)).boldened());
    g.drawText("SAMPLER",
               juce::Rectangle<int>(rx + 10, 4, cw - 20, kSectionH - 2),
               juce::Justification::centredLeft, true);

    // SAMPLER row labels
    g.setFont(juce::FontOptions(kFontBadge));
    g.setColour(kLabelColour);
    static const char* const smpLbls[] = {
        "Opacity", "Gamma", "Contrast Min", "Fade-In", "Transport"
    };
    for (int i = 0; i < 5; ++i)
        g.drawText(smpLbls[i],
                   juce::Rectangle<int>(rx, rs + i * kRowStep, kLabelW, kRowH),
                   juce::Justification::centredRight, true);

    // ── BLOB DETECTION section (full width, capped height) ────────────────────
    const int blobPanelH = kSectionH + kSectionGap + vizH + kSectionGap;
    g.setColour(juce::Colour(0xff131320));
    g.fillRoundedRectangle(
        juce::Rectangle<int>(lx, bsy, totW, blobPanelH).toFloat(), 4.f);
    g.setColour(juce::Colour(0xff2a2a40));
    g.drawRoundedRectangle(
        juce::Rectangle<int>(lx, bsy, totW, blobPanelH).toFloat(), 4.f, 1.f);

    // BLOB badge
    g.setColour(kBlobBadgeBg);
    g.fillRoundedRectangle(
        juce::Rectangle<int>(lx + 4, bsy + 4, totW - 8, kSectionH - 2).toFloat(), 3.f);
    g.setColour(kBlobBadgeFg);
    g.setFont(juce::Font(juce::FontOptions(kFontBadge)).boldened());
    g.drawText("BLOB DETECTION",
               juce::Rectangle<int>(lx + 10, bsy + 4, totW - 20, kSectionH - 2),
               juce::Justification::centredLeft, true);

    // Blob param labels (right half)
    g.setFont(juce::FontOptions(kFontBadge));
    g.setColour(kLabelColour);
    static const char* const blobLbls[] = { "Threshold", "Min Width", "Merge Gap" };
    for (int i = 0; i < 3; ++i)
        g.drawText(blobLbls[i],
                   juce::Rectangle<int>(rx, bcy + i * kRowStep, kLabelW, kRowH),
                   juce::Justification::centredRight, true);
}

//==============================================================================
void ImagePageComponent::resized()
{
    const int cw  = colW();
    const int lx  = colLX();
    const int rx  = colRX();
    const int rs  = kSectionH + kSectionGap;
    const int bcy = blobContentY();
    const int vizH = blobVizH();

    // ── Left column: LIVE ─────────────────────────────────────────────────────
    {
        const int cx  = lx + kCtrlOff;
        const int cw2 = cw - kCtrlOff - 6;

        liveOpacitySlider .setBounds(cx, rs + 0 * kRowStep, cw2, kRowH);
        gammaSlider       .setBounds(cx, rs + 1 * kRowStep, cw2, kRowH);
        contrastMinSlider .setBounds(cx, rs + 2 * kRowStep, cw2, kRowH);
        fadeInSlider      .setBounds(cx, rs + 3 * kRowStep, cw2, kRowH);

        // Transport row — 3 equal buttons (PLAY / HOLD / WHITE)
        const int trY = rs + 4 * kRowStep;
        const int trW = (cw2 - 8) / 3;
        livePlayBtn .setBounds(cx,                  trY, trW, kRowH);
        liveHoldBtn .setBounds(cx + trW + 4,        trY, trW, kRowH);
        liveWhiteBtn.setBounds(cx + (trW + 4) * 2,  trY, trW, kRowH);
    }

    // ── Right column: SAMPLER ─────────────────────────────────────────────────
    {
        const int cx  = rx + kCtrlOff;
        const int cw2 = cw - kCtrlOff - 6;

        samplerOpacitySlider    .setBounds(cx, rs + 0 * kRowStep, cw2, kRowH);
        samplerGammaSlider      .setBounds(cx, rs + 1 * kRowStep, cw2, kRowH);
        samplerContrastMinSlider.setBounds(cx, rs + 2 * kRowStep, cw2, kRowH);
        samplerFadeInSlider     .setBounds(cx, rs + 3 * kRowStep, cw2, kRowH);

        // Transport row — 3 equal buttons (PLAY / HOLD / STOP)
        const int trY = rs + 4 * kRowStep;
        const int trW = (cw2 - 8) / 3;
        samplerPlayBtn.setBounds(cx,                 trY, trW, kRowH);
        samplerHoldBtn.setBounds(cx + trW + 4,       trY, trW, kRowH);
        samplerStopBtn.setBounds(cx + (trW + 4) * 2, trY, trW, kRowH);
    }

    // ── Blob section ──────────────────────────────────────────────────────────
    // Left half: BlobVisualizer (capped height)
    blobViz->setBounds(lx + 4, bcy, cw - 8, vizH);

    // Right half: 3 detection param sliders
    {
        const int cx  = rx + kCtrlOff + 4;
        const int cw2 = cw - kCtrlOff - 12;
        sfBlobThreshSlider.setBounds(cx, bcy + 0 * kRowStep, cw2, kRowH);
        sfMinWidthSlider  .setBounds(cx, bcy + 1 * kRowStep, cw2, kRowH);
        sfMergeGapSlider  .setBounds(cx, bcy + 2 * kRowStep, cw2, kRowH);
    }
}
