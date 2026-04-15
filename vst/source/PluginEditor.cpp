#include "PluginProcessor.h"
#include "PluginEditor.h"

// ── Helper ───────────────────────────────────────────────────────────────────
static void initSlider(juce::Slider& s, const char* suffix = nullptr)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                      Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
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

    // ── Tab buttons (IMAGE | SYNTH | SAMPLER) ─────────────────────────────────
    imageTabBtn.onClick   = [this] { switchToTab(Tab::Image);   };
    synthTabBtn.onClick   = [this] { switchToTab(Tab::Synth);   };
    samplerTabBtn.onClick = [this] { switchToTab(Tab::Sampler); };
    // Mark as tab buttons so LookAndFeel skips default background painting
    imageTabBtn.getProperties().set("isTab", true);
    synthTabBtn.getProperties().set("isTab", true);
    samplerTabBtn.getProperties().set("isTab", true);
    addAndMakeVisible(imageTabBtn);
    addAndMakeVisible(synthTabBtn);
    addAndMakeVisible(samplerTabBtn);

    // ── LuxStral controls (audio only — no image pre-processing params) ────────
    deviceOnToggle.setButtonText("Active");
    addAndMakeVisible(deviceOnToggle);
    deviceOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "deviceEnabled", deviceOnToggle);

    initSlider(masterVolumeSlider);
    addAndMakeVisible(masterVolumeSlider);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "masterVolume", masterVolumeSlider);

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

    // ── StrokeForge controls (right column, SYNTH tab) ────────────────────────
    sfEnabledToggle.setButtonText("StrokeForge Active");
    addChildComponent(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfEnabled", sfEnabledToggle);

    initSlider(sfMorphWidthSlider);
    addChildComponent(sfMorphWidthSlider);
    sfMorphWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfMorphWidthScale", sfMorphWidthSlider);

    initSlider(sfFocusSigmaSlider, " notes");
    addChildComponent(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    initSlider(sfSpectralThreshSlider, " notes");
    addChildComponent(sfSpectralThreshSlider);
    sfSpectralThreshAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfSpectralWidthThreshold", sfSpectralThreshSlider);

    sfFocusOnlyToggle.setButtonText("Focus Without Morph");
    addChildComponent(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfFocusOnly", sfFocusOnlyToggle);

    // ── IMAGE page ────────────────────────────────────────────────────────────
    imagePage = std::make_unique<ImagePageComponent>(audioProcessor);
    addChildComponent(imagePage.get());

    // Wire visualizer source selection:
    // When the user clicks a pipeline node (RAW / S / L / M / SPCTR_* / SYNTH_*),
    // propagate the selected mode to the CIS visualizer.
    imagePage->onVisualizerModeChanged = [this](VisualizerMode mode)
    {
        if (cisVisualizer)
            cisVisualizer->setActiveSource(mode);
    };

    // ── Sampler page (hidden by default) ──────────────────────────────────────
    samplerPage = std::make_unique<SamplerPageComponent>(audioProcessor);
    addChildComponent(samplerPage.get());

    // ── Header gear button ────────────────────────────────────────────────────
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    // Start on IMAGE tab
    switchToTab(Tab::Image);
    juce::LookAndFeel::setDefaultLookAndFeel(&sp3ctraLaf);
    setSize(740, 760);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    settingsWindow.reset();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::switchToTab(Tab tab)
{
    currentTab = tab;

    // SYNTH controls (audio-only params + StrokeForge right column)
    const bool synthVis = (tab == Tab::Synth);
    std::array<juce::Component*, 12> synthCtrls = {
        &deviceOnToggle,   &masterVolumeSlider,
        &attackSlider,     &releaseSlider,
        &stereoTempSlider, &sumExpSlider, &noiseGateSlider,
        &sfEnabledToggle,
        &sfMorphWidthSlider, &sfFocusSigmaSlider,
        &sfSpectralThreshSlider, &sfFocusOnlyToggle
    };
    for (auto* c : synthCtrls) c->setVisible(synthVis);

    // Page components
    if (imagePage)   imagePage->setVisible(tab == Tab::Image);
    if (samplerPage) samplerPage->setVisible(tab == Tab::Sampler);

    // Blob overlay: visible only when IMAGE tab is active
    if (cisVisualizer) cisVisualizer->setBlobOverlayVisible(tab == Tab::Image);

    // Tab button colours — use theme tokens
    auto styleTab = [](juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      juce::Colour(active ? Sp3ctraTheme::kColTabActiveBg
                                          : Sp3ctraTheme::kColTabInactiveBg));
        btn.setColour(juce::TextButton::textColourOffId,
                      juce::Colour(active ? Sp3ctraTheme::kColTabActiveText
                                          : Sp3ctraTheme::kColTabInactiveText));
    };
    styleTab(imageTabBtn,   tab == Tab::Image);
    styleTab(synthTabBtn,   tab == Tab::Synth);
    styleTab(samplerTabBtn, tab == Tab::Sampler);

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
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTitle)).boldened());
    g.drawText("Sp3ctra", juce::Rectangle<int>(12, 0, getWidth()-24, kHeaderH),
               juce::Justification::centredLeft, true);

    // Version — centred between logo and gear button
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    g.setColour(juce::Colour(0xff777777));
    g.drawText("v0.1.5", juce::Rectangle<int>(0, 0, getWidth(), kHeaderH),
               juce::Justification::centred, true);

    g.setColour(juce::Colour(0xff444444));
    g.fillRect(0, kHeaderH, getWidth(), 1);

    // ── Tab bar — proper tab styling ──────────────────────────────────────────
    {
        // Full bar background
        g.setColour(juce::Colour(Sp3ctraTheme::kColTabBarBg));
        g.fillRect(0, kTabsY, getWidth(), kTabsH);

        // Bottom separator line (below entire tab bar)
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.fillRect(0, kTabsY + kTabsH, getWidth(), 1);

        // Draw each tab background + accent
        const juce::TextButton* tabs[]  = { &imageTabBtn, &synthTabBtn, &samplerTabBtn };
        const Tab               tabIds[] = { Tab::Image,   Tab::Synth,   Tab::Sampler };

        for (int i = 0; i < 3; ++i)
        {
            const bool active = (currentTab == tabIds[i]);
            const auto tbr = tabs[i]->getBounds().toFloat();

            if (active)
            {
                // Active tab background — rounded top, flat bottom
                juce::Path tabPath;
                const float r = Sp3ctraTheme::kTabCornerR;
                tabPath.addRoundedRectangle(tbr.getX(), tbr.getY(),
                                            tbr.getWidth(), tbr.getHeight() + r,
                                            r, r, true, true, false, false);
                g.setColour(juce::Colour(Sp3ctraTheme::kColTabActiveBg));
                g.fillPath(tabPath);

                // Subtle border on active tab (top + sides only)
                g.setColour(juce::Colour(Sp3ctraTheme::kColTabBorderActive));
                g.strokePath(tabPath, juce::PathStrokeType(1.0f));

                // Accent underline at the bottom of the active tab
                g.setColour(juce::Colour(Sp3ctraTheme::kColTabAccent));
                g.fillRect(tbr.getX(), tbr.getBottom() - (float)Sp3ctraTheme::kTabUnderlineH,
                           tbr.getWidth(), (float)Sp3ctraTheme::kTabUnderlineH);
            }
            else
            {
                // Inactive tab — flat, subtle
                g.setColour(juce::Colour(Sp3ctraTheme::kColTabInactiveBg));
                g.fillRect(tbr);

                // Very subtle bottom border for separation
                g.setColour(juce::Colour(Sp3ctraTheme::kColTabBorderInactive));
                g.fillRect(tbr.getX(), tbr.getBottom() - 1.f, tbr.getWidth(), 1.f);
            }
        }
    }

    // ── SYNTH page labels (drawn inline — ImagePage draws itself) ─────────────
    if (currentTab == Tab::Synth)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rsy = rowsStartY();

        // LuxStral section badge (audio-only, no image pipeline params)
        g.setColour(juce::Colour(0xff1c3755));
        g.fillRoundedRectangle(juce::Rectangle<int>(lxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xff7ab0f0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("LUXSTRAL",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Row labels — 7 audio-only rows (left col)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));

        static const char* const lsLbls[kLS_ROWS] = {
            "Device On", "Volume", "Attack", "Release",
            "Stereo Temp.", "Sum. Exp.", "Noise Gate"
        };
        for (int i = 0; i < kLS_ROWS; ++i)
            g.drawText(lsLbls[i],
                       juce::Rectangle<int>(lxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);

        // ── StrokeForge section (right column) ────────────────────────────────
        const int rxp = colRX();

        // Slot-style background for SF panel
        g.setColour(juce::Colour(0xff131320));
        g.fillRoundedRectangle(
            juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH + kSectionGap +
                                 kSF_ROWS * kRowStep + kSectionGap).toFloat(), 4.f);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawRoundedRectangle(
            juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH + kSectionGap +
                                 kSF_ROWS * kRowStep + kSectionGap).toFloat(), 4.f, 1.f);

        // SF badge (purple)
        g.setColour(juce::Colour(0xff2c1f4a));
        g.fillRoundedRectangle(
            juce::Rectangle<int>(rxp+4, kPageTop+4, cw-8, kSectionH-2).toFloat(), 3.f);
        g.setColour(juce::Colour(0xffb07af0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("STROKEFORGE  --  Waveform Morphing  (Sine -> Square)",
                   juce::Rectangle<int>(rxp+10, kPageTop+4, cw-20, kSectionH-2),
                   juce::Justification::centredLeft, true);

        // SF row labels
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));

        static const char* const sfLbls[kSF_ROWS] = {
            "Enable", "Square at Width", "Focus Sigma",
            "Spectral Threshold", "Focus Only (spectral)"
        };
        for (int i = 0; i < kSF_ROWS; ++i)
            g.drawText(sfLbls[i],
                       juce::Rectangle<int>(rxp+4, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);
    }
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::resized()
{
    const int lxp = colLX();
    const int rsy = rowsStartY();

    // ── CIS Visualizer ────────────────────────────────────────────────────────
    cisVisualizer->setBounds(kHPad, kVisY, getWidth() - 2*kHPad, kVisH);

    // ── Tab buttons (IMAGE | SYNTH | SAMPLER) ─────────────────────────────────
    imageTabBtn  .setBounds(kHPad,       kTabsY + 2, 80, Sp3ctraTheme::kTabBtnH);
    synthTabBtn  .setBounds(kHPad + 84,  kTabsY + 2, 80, Sp3ctraTheme::kTabBtnH);
    samplerTabBtn.setBounds(kHPad + 168, kTabsY + 2, 92, Sp3ctraTheme::kTabBtnH);

    // ── LuxStral controls (SYNTH page, left column) ───────────────────────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        deviceOnToggle    .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        masterVolumeSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        attackSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        releaseSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        stereoTempSlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sumExpSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        noiseGateSlider   .setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── StrokeForge controls (SYNTH page, right column) ──────────────────────
    {
        const int rxp = colRX();
        const int cw  = colWidth();
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = cw - kCtrlOffset - 12;
        int cy = rsy;
        sfEnabledToggle      .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfMorphWidthSlider   .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfFocusSigmaSlider   .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfSpectralThreshSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfFocusOnlyToggle    .setBounds(cx, cy, cw2, kRowH);
    }

    // ── Header gear button (top-right) ────────────────────────────────────────
    const int btnSz = kHeaderH - 8;
    settingsButton.setBounds(getWidth() - btnSz - 4, 4, btnSz, btnSz);

    // ── IMAGE page (full page area below tabs) ────────────────────────────────
    if (imagePage)
    {
        const int pageH = getHeight() - kPageTop - 8;
        imagePage->setBounds(kHPad, kPageTop, getWidth() - 2*kHPad, pageH);
    }

    // ── Sampler page ──────────────────────────────────────────────────────────
    if (samplerPage)
    {
        const int pageH = getHeight() - kPageTop - 8;
        samplerPage->setBounds(kHPad, kPageTop, getWidth() - 2*kHPad, pageH);
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
