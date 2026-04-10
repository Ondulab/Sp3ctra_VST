#include "PluginProcessor.h"
#include "PluginEditor.h"

// ── Helper ───────────────────────────────────────────────────────────────────
static void initSlider(juce::Slider& s, const char* suffix = nullptr)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 20);
    if (suffix) s.setTextValueSuffix(suffix);
}

//==============================================================================
Sp3ctraAudioProcessorEditor::Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    auto& apvts = audioProcessor.getAPVTS();

    // ── CIS Visualizer ────────────────────────────────────────────────────────
    cisVisualizer = std::make_unique<CisVisualizerComponent>(audioProcessor);
    addAndMakeVisible(cisVisualizer.get());

    // ── Tab buttons ───────────────────────────────────────────────────────────
    synthTabBtn.onClick   = [this] { switchToPage(false); };
    samplerTabBtn.onClick = [this] { switchToPage(true);  };
    addAndMakeVisible(synthTabBtn);
    addAndMakeVisible(samplerTabBtn);

    // ── LuxStral controls ─────────────────────────────────────────────────────
    deviceOnToggle.setButtonText("Active");
    addAndMakeVisible(deviceOnToggle);
    deviceOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "deviceEnabled", deviceOnToggle);

    initSlider(masterVolumeSlider);
    addAndMakeVisible(masterVolumeSlider);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "masterVolume", masterVolumeSlider);

    initSlider(gammaSlider);
    addAndMakeVisible(gammaSlider);
    gammaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralGammaValue", gammaSlider);

    initSlider(contrastMinSlider);
    addAndMakeVisible(contrastMinSlider);
    contrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralContrastMin", contrastMinSlider);

    initSlider(attackSlider, " ms");
    addAndMakeVisible(attackSlider);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralAttackMs", attackSlider);

    initSlider(releaseSlider, " ms");
    addAndMakeVisible(releaseSlider);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralReleaseMs", releaseSlider);

    initSlider(stereoTempSlider);
    addAndMakeVisible(stereoTempSlider);
    stereoTempAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralStereoTempAmp", stereoTempSlider);

    initSlider(sumExpSlider);
    addAndMakeVisible(sumExpSlider);
    sumExpAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSummationResponseExp", sumExpSlider);

    initSlider(noiseGateSlider);
    addAndMakeVisible(noiseGateSlider);
    noiseGateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNoiseGateThreshold", noiseGateSlider);

    // ── StrokeForge controls ──────────────────────────────────────────────────
    sfEnabledToggle.setButtonText("Active");
    addAndMakeVisible(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfEnabled", sfEnabledToggle);

    initSlider(sfBlobThreshSlider);
    addAndMakeVisible(sfBlobThreshSlider);
    sfBlobThreshAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobBaseThreshold", sfBlobThreshSlider);

    initSlider(sfMergeGapSlider, " pix");
    addAndMakeVisible(sfMergeGapSlider);
    sfMergeGapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMergeGap", sfMergeGapSlider);

    initSlider(sfFocusSigmaSlider, " pix");
    addAndMakeVisible(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    initSlider(sfSpectralWidthSlider, " pix");
    addAndMakeVisible(sfSpectralWidthSlider);
    sfSpectralWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfSpectralWidthThreshold", sfSpectralWidthSlider);

    sfFocusOnlyToggle.setButtonText("On (no morph)");
    addAndMakeVisible(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfFocusOnly", sfFocusOnlyToggle);

    // ── Sampler page (hidden by default) ──────────────────────────────────────
    samplerPage = std::make_unique<SamplerPageComponent>(audioProcessor);
    addChildComponent(samplerPage.get());

    // ── Footer ────────────────────────────────────────────────────────────────
    settingsButton.setButtonText("Settings...");
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::FontOptions(12.f));
    addAndMakeVisible(statusLabel);

    // Start on SYNTH tab
    switchToPage(false);
    startTimer(200);
    setSize(740, 760);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    stopTimer();
    settingsWindow.reset();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::switchToPage(bool showSampler)
{
    showingSamplerPage = showSampler;

    // Toggle SYNTH controls visibility
    const bool synthVis = !showSampler;
    std::array<juce::Component*, 15> synthCtrls = {
        &deviceOnToggle,    &masterVolumeSlider,   &gammaSlider,
        &contrastMinSlider, &attackSlider,         &releaseSlider,
        &stereoTempSlider,  &sumExpSlider,         &noiseGateSlider,
        &sfEnabledToggle,   &sfBlobThreshSlider,   &sfMergeGapSlider,
        &sfFocusSigmaSlider,&sfSpectralWidthSlider, &sfFocusOnlyToggle
    };
    for (auto* c : synthCtrls) c->setVisible(synthVis);

    if (samplerPage) samplerPage->setVisible(showSampler);

    // Tab button colours
    auto styleTab = [](juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? juce::Colour(0xff3a3a3a) : juce::Colour(0xff222222));
        btn.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colours::white : juce::Colour(0xff888888));
    };
    styleTab(synthTabBtn,   !showSampler);
    styleTab(samplerTabBtn,  showSampler);

    repaint();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));

    // ── Header ────────────────────────────────────────────────────────────────
    g.setGradientFill(juce::ColourGradient(
        juce::Colour(0xff383838), 0.f, 0.f,
        juce::Colour(0xff262626), 0.f, (float)kHeaderH, false));
    g.fillRect(0, 0, getWidth(), kHeaderH);

    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(22.f)).boldened());
    g.drawText("Sp3ctra", juce::Rectangle<int>(12, 0, getWidth()-24, kHeaderH),
               juce::Justification::centredLeft, true);

    g.setFont(juce::FontOptions(11.f));
    g.setColour(juce::Colour(0xff888888));
    g.drawText("v0.0.1", juce::Rectangle<int>(0, 0, getWidth()-12, kHeaderH),
               juce::Justification::centredRight, true);

    g.setColour(juce::Colour(0xff444444));
    g.fillRect(0, kHeaderH, getWidth(), 1);

    // ── Tab bar borders ───────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff2a2a2a));
    g.fillRect(0, kTabsY, getWidth(), kTabsH);
    g.setColour(juce::Colour(0xff444444));
    g.fillRect(0, kTabsY - 1, getWidth(), 1);
    g.fillRect(0, kTabsY + kTabsH, getWidth(), 1);

    // ── SYNTH page labels ─────────────────────────────────────────────────────
    if (!showingSamplerPage)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rxp = colRX();
        const int rsy = rowsStartY();

        // LuxStral section badge
        g.setColour(juce::Colour(0xff1c3755));
        g.fillRoundedRectangle(juce::Rectangle<int>(lxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xff7ab0f0));
        g.setFont(juce::Font(juce::FontOptions(12.f)).boldened());
        g.drawText("LUXSTRAL",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // StrokeForge section badge
        g.setColour(juce::Colour(0xff3d2e00));
        g.fillRoundedRectangle(juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xffffc84a));
        g.setFont(juce::Font(juce::FontOptions(12.f)).boldened());
        g.drawText("STROKEFORGE",
                   juce::Rectangle<int>(rxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Row labels
        g.setFont(juce::FontOptions(12.f));
        g.setColour(juce::Colour(0xffb8c4d0));

        static const char* const lsLbls[kLS_ROWS] = {
            "Device On", "Volume", "Gamma", "Contrast Min", "Attack",
            "Release", "Stereo Temp.", "Sum. Exp.", "Noise Gate"
        };
        for (int i = 0; i < kLS_ROWS; ++i)
            g.drawText(lsLbls[i],
                       juce::Rectangle<int>(lxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);

        static const char* const sfLbls[kSF_ROWS] = {
            "SF Active", "Blob Thr.", "Merge Gap",
            "Focus \xcf\x83", "Spectral Thr.", "Focus Only"
        };
        for (int i = 0; i < kSF_ROWS; ++i)
            g.drawText(juce::String::fromUTF8(sfLbls[i]),
                       juce::Rectangle<int>(rxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);
    }

    // ── Footer separator ──────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff3a3a3a));
    g.fillRect(0, footerY() - 6, getWidth(), 1);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::resized()
{
    const int lxp = colLX();
    const int rxp = colRX();
    const int rsy = rowsStartY();

    // ── CIS Visualizer ────────────────────────────────────────────────────────
    cisVisualizer->setBounds(kHPad, kVisY, getWidth() - 2*kHPad, kVisH);

    // ── Tab buttons ───────────────────────────────────────────────────────────
    synthTabBtn  .setBounds(kHPad,          kTabsY + 2, 92, kTabsH - 4);
    samplerTabBtn.setBounds(kHPad + 96,     kTabsY + 2, 92, kTabsH - 4);

    // ── LuxStral controls ─────────────────────────────────────────────────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        deviceOnToggle    .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        masterVolumeSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        gammaSlider       .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        contrastMinSlider .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        attackSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        releaseSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        stereoTempSlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sumExpSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        noiseGateSlider   .setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── StrokeForge controls ──────────────────────────────────────────────────
    {
        const int cx = rxp + kCtrlOffset;
        int cy = rsy;
        sfEnabledToggle      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sfBlobThreshSlider   .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sfMergeGapSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sfFocusSigmaSlider   .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sfSpectralWidthSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sfFocusOnlyToggle    .setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── Sampler page ──────────────────────────────────────────────────────────
    if (samplerPage)
    {
        const int pageH = footerY() - kPageTop - 8;
        samplerPage->setBounds(kHPad, kPageTop, getWidth() - 2*kHPad, pageH);
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    const int fy = footerY();
    settingsButton.setBounds(kHPad, fy, 92, 28);
    statusLabel   .setBounds(kHPad + 100, fy, getWidth() - kHPad - 104, 28);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::timerCallback()
{
    // ── UDP status label ──────────────────────────────────────────────────────
    const auto* core = audioProcessor.getSp3ctraCore();
    if (core != nullptr && core->isInitialized())
    {
        const int  port = static_cast<int>(
            audioProcessor.getAPVTS().getRawParameterValue("udpPort")->load());
        statusLabel.setText("UDP :" + juce::String(port), juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    }
    else
    {
        statusLabel.setText("waiting for CIS data...", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    }
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::openSettings()
{
    if (!settingsWindow)
        settingsWindow = std::make_unique<SettingsWindow>(audioProcessor);
    settingsWindow->setVisible(true);
    settingsWindow->toFront(true);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::suspendVisualizer()
{
    if (cisVisualizer) cisVisualizer->suspend();
}

void Sp3ctraAudioProcessorEditor::resumeVisualizer()
{
    if (cisVisualizer) cisVisualizer->resume();
}
