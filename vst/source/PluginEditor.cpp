#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "IconPaths.h"

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

    // ── Tab buttons (IMAGE | SYNTH | SAMPLER | VIDEO) ─────────────────────────
    imageTabBtn.onClick   = [this] { switchToTab(Tab::Image);   };
    synthTabBtn.onClick   = [this] { switchToTab(Tab::Synth);   };
    samplerTabBtn.onClick = [this] { switchToTab(Tab::Sampler); };
    videoTabBtn.onClick   = [this] { switchToTab(Tab::Video);   };
    // Mark as tab buttons so LookAndFeel skips default background painting
    imageTabBtn.getProperties().set("isTab", true);
    synthTabBtn.getProperties().set("isTab", true);
    samplerTabBtn.getProperties().set("isTab", true);
    videoTabBtn.getProperties().set("isTab", true);
    addAndMakeVisible(imageTabBtn);
    addAndMakeVisible(synthTabBtn);
    addAndMakeVisible(samplerTabBtn);
    addAndMakeVisible(videoTabBtn);

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

    stereoEnableToggle.setButtonText("Active");
    addAndMakeVisible(stereoEnableToggle);
    stereoEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralStereoEnable", stereoEnableToggle);

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

    // ── Synth sub-tab buttons ────────────────────────────────────────────────
    luxstralSubBtn.onClick  = [this] { switchSynthSubTab(SynthSub::LuxStral); };
    luxsynthSubBtn.onClick  = [this] { switchSynthSubTab(SynthSub::LuxSynth); };
    luxstralSubBtn.getProperties().set("isTab", true);
    luxsynthSubBtn.getProperties().set("isTab", true);
    addChildComponent(luxstralSubBtn);
    addChildComponent(luxsynthSubBtn);

    // ── LuxSynth audio controls ─────────────────────────────────────────────
    lxEnableToggle.setButtonText("Active");
    addChildComponent(lxEnableToggle);
    lxEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxsynthEnabled", lxEnableToggle);

    initSlider(lxAttackSlider, " ms");   addChildComponent(lxAttackSlider);
    lxAttackAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthAttackMs", lxAttackSlider);
    initSlider(lxDecaySlider, " ms");    addChildComponent(lxDecaySlider);
    lxDecayAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthDecayMs", lxDecaySlider);
    initSlider(lxSustainSlider);         addChildComponent(lxSustainSlider);
    lxSustainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthSustainLevel", lxSustainSlider);
    initSlider(lxReleaseSlider, " ms");  addChildComponent(lxReleaseSlider);
    lxReleaseAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthReleaseMs", lxReleaseSlider);

    initSlider(lxFltAttackSlider, " ms");  addChildComponent(lxFltAttackSlider);
    lxFltAttackAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterAttackMs", lxFltAttackSlider);
    initSlider(lxFltDecaySlider, " ms");   addChildComponent(lxFltDecaySlider);
    lxFltDecayAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterDecayMs", lxFltDecaySlider);
    initSlider(lxFltSustainSlider);        addChildComponent(lxFltSustainSlider);
    lxFltSustainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterSustain", lxFltSustainSlider);
    initSlider(lxFltReleaseSlider, " ms"); addChildComponent(lxFltReleaseSlider);
    lxFltReleaseAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterReleaseMs", lxFltReleaseSlider);
    initSlider(lxFltCutoffSlider, " Hz");  addChildComponent(lxFltCutoffSlider);
    lxFltCutoffAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterCutoff", lxFltCutoffSlider);
    initSlider(lxFltDepthSlider);          addChildComponent(lxFltDepthSlider);
    lxFltDepthAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthFilterEnvDepth", lxFltDepthSlider);

    initSlider(lxGammaSlider);    addChildComponent(lxGammaSlider);
    lxGammaAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthGamma", lxGammaSlider);
    initSlider(lxNumOscSlider);   addChildComponent(lxNumOscSlider);
    lxNumOscAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthNumOscillators", lxNumOscSlider);

    initSlider(lxLfoRateSlider, " Hz");  addChildComponent(lxLfoRateSlider);
    lxLfoRateAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthLfoRate", lxLfoRateSlider);
    initSlider(lxLfoDepthSlider);        addChildComponent(lxLfoDepthSlider);
    lxLfoDepthAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxsynthLfoDepth", lxLfoDepthSlider);

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

    // ── VIDEO page (hidden by default) ────────────────────────────────────────
    videoScrollPage = std::make_unique<VideoScrollTab>(audioProcessor);
    addChildComponent(videoScrollPage.get());

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
    const bool synthVis = (tab == Tab::Synth);

    // Sub-tab buttons — visible only on SYNTH page
    luxstralSubBtn.setVisible(synthVis);
    luxsynthSubBtn.setVisible(synthVis);

    // Hide all synth controls first, then let switchSynthSubTab() show the right ones
    std::array<juce::Component*, 13> luxstralCtrls = {
        &deviceOnToggle,   &masterVolumeSlider,
        &attackSlider,     &releaseSlider,
        &stereoEnableToggle, &stereoTempSlider, &sumExpSlider, &noiseGateSlider,
        &sfEnabledToggle,
        &sfMorphWidthSlider, &sfFocusSigmaSlider,
        &sfSpectralThreshSlider, &sfFocusOnlyToggle
    };
    for (auto* c : luxstralCtrls) c->setVisible(false);

    std::array<juce::Component*, 16> luxsynthCtrls = {
        &lxEnableToggle,
        &lxAttackSlider, &lxDecaySlider, &lxSustainSlider, &lxReleaseSlider,
        &lxFltAttackSlider, &lxFltDecaySlider, &lxFltSustainSlider, &lxFltReleaseSlider,
        &lxFltCutoffSlider, &lxFltDepthSlider,
        &lxGammaSlider, &lxNumOscSlider,
        &lxLfoRateSlider, &lxLfoDepthSlider,
        nullptr  // padding to 16
    };
    for (auto* c : luxsynthCtrls) if (c) c->setVisible(false);

    // Show the correct sub-tab controls
    if (synthVis)
        switchSynthSubTab(currentSynthSub);

    // Page components
    if (imagePage)       imagePage->setVisible(tab == Tab::Image);
    if (samplerPage)     samplerPage->setVisible(tab == Tab::Sampler);
    if (videoScrollPage)
    {
        const bool videoVis = (tab == Tab::Video);
        videoScrollPage->setVisible(videoVis);
        if (videoVis)
            videoScrollPage->onTabActivated();
        else
            videoScrollPage->onTabDeactivated();
    }

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
    styleTab(videoTabBtn,   tab == Tab::Video);

    repaint();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::switchSynthSubTab(SynthSub sub)
{
    currentSynthSub = sub;

    const bool isLuxStral = (sub == SynthSub::LuxStral);
    const bool isLuxSynth = (sub == SynthSub::LuxSynth);

    // LuxStral controls
    deviceOnToggle.setVisible(isLuxStral);
    masterVolumeSlider.setVisible(isLuxStral);
    attackSlider.setVisible(isLuxStral);
    releaseSlider.setVisible(isLuxStral);
    stereoEnableToggle.setVisible(isLuxStral);
    stereoTempSlider.setVisible(isLuxStral);
    sumExpSlider.setVisible(isLuxStral);
    noiseGateSlider.setVisible(isLuxStral);
    sfEnabledToggle.setVisible(isLuxStral);
    sfMorphWidthSlider.setVisible(isLuxStral);
    sfFocusSigmaSlider.setVisible(isLuxStral);
    sfSpectralThreshSlider.setVisible(isLuxStral);
    sfFocusOnlyToggle.setVisible(isLuxStral);

    // LuxSynth controls
    lxEnableToggle.setVisible(isLuxSynth);
    lxAttackSlider.setVisible(isLuxSynth);
    lxDecaySlider.setVisible(isLuxSynth);
    lxSustainSlider.setVisible(isLuxSynth);
    lxReleaseSlider.setVisible(isLuxSynth);
    lxFltAttackSlider.setVisible(isLuxSynth);
    lxFltDecaySlider.setVisible(isLuxSynth);
    lxFltSustainSlider.setVisible(isLuxSynth);
    lxFltReleaseSlider.setVisible(isLuxSynth);
    lxFltCutoffSlider.setVisible(isLuxSynth);
    lxFltDepthSlider.setVisible(isLuxSynth);
    lxGammaSlider.setVisible(isLuxSynth);
    lxNumOscSlider.setVisible(isLuxSynth);
    lxLfoRateSlider.setVisible(isLuxSynth);
    lxLfoDepthSlider.setVisible(isLuxSynth);

    // Sub-tab button styling
    auto styleSubTab = [](juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      juce::Colour(active ? 0xff2a3a50 : 0xff1a1a24));
        btn.setColour(juce::TextButton::textColourOffId,
                      juce::Colour(active ? 0xffc0d8f0 : 0xff667788));
    };
    styleSubTab(luxstralSubBtn, isLuxStral);
    styleSubTab(luxsynthSubBtn, isLuxSynth);

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

    // Logo picto (5 coloured bars) — left side of header
    constexpr float pictoW = 36.f;
    constexpr float pictoH = 40.f;
    const float pictoX = 10.f;
    const float pictoY = ((float)kHeaderH - pictoH) * 0.5f;
    Icons::drawSp3ctraLogoPicto(g, { pictoX, pictoY, pictoW, pictoH });

    // "Sp3ctra" text — right of the picto
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTitle)).boldened());
    const int textX = (int)(pictoX + pictoW + 6.f);
    g.drawText("Sp3ctra", juce::Rectangle<int>(textX, 0, getWidth() - textX - 24, kHeaderH),
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
        const juce::TextButton* tabs[]   = { &imageTabBtn, &synthTabBtn, &samplerTabBtn, &videoTabBtn };
        const Tab               tabIds[] = { Tab::Image,   Tab::Synth,   Tab::Sampler,   Tab::Video };

        for (int i = 0; i < 4; ++i)
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

    // ── SYNTH sub-tab bar background ─────────────────────────────────────────
    if (currentTab == Tab::Synth)
    {
        g.setColour(juce::Colour(0xff161620));
        g.fillRect(0, kSubTabsY, getWidth(), kSubTabsH);
        g.setColour(juce::Colour(0xff333344));
        g.fillRect(0, kSubTabsY + kSubTabsH - 1, getWidth(), 1);

        // Active sub-tab accent
        const auto& activeBtn = (currentSynthSub == SynthSub::LuxStral) ? luxstralSubBtn : luxsynthSubBtn;
        g.setColour(juce::Colour(0xff4488cc));
        g.fillRect(activeBtn.getX(), kSubTabsY + kSubTabsH - 2,
                   activeBtn.getWidth(), 2);
    }

    // ── SYNTH page labels (drawn inline — ImagePage draws itself) ─────────────
    if (currentTab == Tab::Synth && currentSynthSub == SynthSub::LuxStral)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rsy = rowsStartY();

        // LuxStral section badge
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
            "Stereo", "Stereo Temp.", "Sum. Exp.", "Noise Gate"
        };
        for (int i = 0; i < kLS_ROWS; ++i)
            g.drawText(lsLbls[i],
                       juce::Rectangle<int>(lxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);

        // ── StrokeForge section (right column) ──────────────────────────────
        const int rxp = colRX();

        g.setColour(juce::Colour(0xff131320));
        g.fillRoundedRectangle(
            juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH + kSectionGap +
                                 kSF_ROWS * kRowStep + kSectionGap).toFloat(), 4.f);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawRoundedRectangle(
            juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH + kSectionGap +
                                 kSF_ROWS * kRowStep + kSectionGap).toFloat(), 4.f, 1.f);

        g.setColour(juce::Colour(0xff2c1f4a));
        g.fillRoundedRectangle(
            juce::Rectangle<int>(rxp+4, kPageTop+4, cw-8, kSectionH-2).toFloat(), 3.f);
        g.setColour(juce::Colour(0xffb07af0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("STROKEFORGE  --  Waveform Morphing  (Sine -> Square)",
                   juce::Rectangle<int>(rxp+10, kPageTop+4, cw-20, kSectionH-2),
                   juce::Justification::centredLeft, true);

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

    // ── LUXSYNTH labels ──────────────────────────────────────────────────────
    if (currentTab == Tab::Synth && currentSynthSub == SynthSub::LuxSynth)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rxp = colRX();
        const int rsy = rowsStartY();

        // Left column badge — VOLUME ADSR (teal)
        g.setColour(juce::Colour(0xff1a3a3a));
        g.fillRoundedRectangle(juce::Rectangle<int>(lxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xff66ccaa));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("LUXSYNTH  —  Volume ADSR + Spectral",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Left column labels (8 rows)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        static const char* const lxLeftLbls[] = {
            "Enable", "Attack", "Decay", "Sustain", "Release",
            "Gamma", "Oscillators", "LFO Rate"
        };
        for (int i = 0; i < 8; ++i)
            g.drawText(lxLeftLbls[i],
                       juce::Rectangle<int>(lxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);

        // Right column badge — FILTER ADSR (purple-pink)
        g.setColour(juce::Colour(0xff2a1a3a));
        g.fillRoundedRectangle(juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xffcc88cc));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("FILTER ADSR + LFO",
                   juce::Rectangle<int>(rxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Right column labels (7 rows)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        static const char* const lxRightLbls[] = {
            "Flt. Attack", "Flt. Decay", "Flt. Sustain", "Flt. Release",
            "Cutoff", "Env Depth", "LFO Depth"
        };
        for (int i = 0; i < 7; ++i)
            g.drawText(lxRightLbls[i],
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

    // ── Tab buttons (IMAGE | SYNTH | SAMPLER | VIDEO) ─────────────────────────
    imageTabBtn  .setBounds(kHPad,       kTabsY + 2, 80, Sp3ctraTheme::kTabBtnH);
    synthTabBtn  .setBounds(kHPad + 84,  kTabsY + 2, 80, Sp3ctraTheme::kTabBtnH);
    samplerTabBtn.setBounds(kHPad + 168, kTabsY + 2, 92, Sp3ctraTheme::kTabBtnH);
    videoTabBtn  .setBounds(kHPad + 264, kTabsY + 2, 70, Sp3ctraTheme::kTabBtnH);

    // ── Synth sub-tab buttons ────────────────────────────────────────────────
    luxstralSubBtn.setBounds(kHPad,      kSubTabsY + 1, 72, kSubTabsH - 2);
    luxsynthSubBtn.setBounds(kHPad + 76, kSubTabsY + 1, 72, kSubTabsH - 2);

    // ── LuxStral controls (SYNTH page, left column) ───────────────────────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        deviceOnToggle    .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        masterVolumeSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        attackSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        releaseSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        stereoEnableToggle.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
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

    // ── LuxSynth controls (SYNTH page, left column = vol ADSR + spectral) ────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        lxEnableToggle .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxAttackSlider .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxDecaySlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxSustainSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxReleaseSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxGammaSlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxNumOscSlider .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxLfoRateSlider.setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── LuxSynth controls (SYNTH page, right column = filter ADSR + LFO) ─────
    {
        const int rxp = colRX();
        const int cw  = colWidth();
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = cw - kCtrlOffset - 12;
        int cy = rsy;
        lxFltAttackSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltDecaySlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltSustainSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltReleaseSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltCutoffSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltDepthSlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxLfoDepthSlider  .setBounds(cx, cy, cw2, kRowH);
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

    // ── Video scroll page ─────────────────────────────────────────────────────
    if (videoScrollPage)
    {
        const int pageH = getHeight() - kPageTop - 8;
        videoScrollPage->setBounds(kHPad, kPageTop, getWidth() - 2*kHPad, pageH);
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
