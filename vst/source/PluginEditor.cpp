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

    // ── Tab buttons (SOURCES | PITCH | MASK | SAMPLER | SYNTH | VIDEO) ────────
    sourcesTabBtn.onClick = [this] { switchToTab(Tab::Sources); };
    pitchTabBtn  .onClick = [this] { switchToTab(Tab::Pitch);   };
    maskTabBtn   .onClick = [this] { switchToTab(Tab::Mask);    };
    samplerTabBtn.onClick = [this] { switchToTab(Tab::Sampler); };
    synthTabBtn  .onClick = [this] { switchToTab(Tab::Synth);   };
    videoTabBtn  .onClick = [this] { switchToTab(Tab::Video);   };
    // Mark as tab buttons so LookAndFeel skips default background painting
    sourcesTabBtn.getProperties().set("isTab", true);
    pitchTabBtn  .getProperties().set("isTab", true);
    maskTabBtn   .getProperties().set("isTab", true);
    samplerTabBtn.getProperties().set("isTab", true);
    synthTabBtn  .getProperties().set("isTab", true);
    videoTabBtn  .getProperties().set("isTab", true);
    addAndMakeVisible(sourcesTabBtn);
    addAndMakeVisible(pitchTabBtn);
    addAndMakeVisible(maskTabBtn);
    addAndMakeVisible(samplerTabBtn);
    addAndMakeVisible(synthTabBtn);
    addAndMakeVisible(videoTabBtn);

    // ── LuxStral controls (audio only — no image pre-processing params) ────────
    deviceOnToggle.setButtonText("Active");
    addAndMakeVisible(deviceOnToggle);
    deviceOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "deviceEnabled", deviceOnToggle);

    initSlider(luxstralVolumeSlider);
    addAndMakeVisible(luxstralVolumeSlider);
    luxstralVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralVolume", luxstralVolumeSlider);

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
    imgLuxStralSubBtn.onClick = [this] { switchSynthSubTab(SynthSub::ImgLuxStral); };
    imgLuxSynthSubBtn.onClick = [this] { switchSynthSubTab(SynthSub::ImgLuxSynth); };
    audioStralSubBtn .onClick = [this] { switchSynthSubTab(SynthSub::AudioStral);  };
    audioSynthSubBtn .onClick = [this] { switchSynthSubTab(SynthSub::AudioSynth);  };
    audioWaveSubBtn  .onClick = [this] { switchSynthSubTab(SynthSub::AudioWave);   };
    imgLuxStralSubBtn.getProperties().set("isTab", true);
    imgLuxSynthSubBtn.getProperties().set("isTab", true);
    audioStralSubBtn .getProperties().set("isTab", true);
    audioSynthSubBtn .getProperties().set("isTab", true);
    audioWaveSubBtn  .getProperties().set("isTab", true);
    addChildComponent(imgLuxStralSubBtn);
    addChildComponent(imgLuxSynthSubBtn);
    addChildComponent(audioStralSubBtn);
    addChildComponent(audioSynthSubBtn);
    addChildComponent(audioWaveSubBtn);

    // ── LuxSynth audio controls ─────────────────────────────────────────────
    lxEnableToggle.setButtonText("Active");
    addChildComponent(lxEnableToggle);
    lxEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxsynthEnabled", lxEnableToggle);

    initSlider(luxsynthVolumeSlider);
    addChildComponent(luxsynthVolumeSlider);
    luxsynthVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxsynthVolume", luxsynthVolumeSlider);

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

    // ── LuxWave audio controls ───────────────────────────────────────────────
    lwEnableToggle.setButtonText("Active");
    addChildComponent(lwEnableToggle);
    lwEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxwaveEnabled", lwEnableToggle);

    initSlider(luxwaveVolumeSlider);
    addChildComponent(luxwaveVolumeSlider);
    luxwaveVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxwaveVolume", luxwaveVolumeSlider);

    initSlider(lwAttackSlider, " ms");   addChildComponent(lwAttackSlider);
    lwAttackAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveAttackMs", lwAttackSlider);
    initSlider(lwDecaySlider, " ms");    addChildComponent(lwDecaySlider);
    lwDecayAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveDecayMs", lwDecaySlider);
    initSlider(lwSustainSlider);         addChildComponent(lwSustainSlider);
    lwSustainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveSustainLevel", lwSustainSlider);
    initSlider(lwReleaseSlider, " ms");  addChildComponent(lwReleaseSlider);
    lwReleaseAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveReleaseMs", lwReleaseSlider);

    initSlider(lwFltCutoffSlider, " Hz"); addChildComponent(lwFltCutoffSlider);
    lwFltCutoffAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveFilterCutoff", lwFltCutoffSlider);
    initSlider(lwFltDepthSlider, " Hz");  addChildComponent(lwFltDepthSlider);
    lwFltDepthAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveFilterEnvDepth", lwFltDepthSlider);

    initSlider(lwLfoRateSlider, " Hz");  addChildComponent(lwLfoRateSlider);
    lwLfoRateAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveLfoRate", lwLfoRateSlider);
    initSlider(lwLfoDepthSlider);        addChildComponent(lwLfoDepthSlider);
    lwLfoDepthAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveLfoDepth", lwLfoDepthSlider);

    initSlider(lwAmplitudeSlider);       addChildComponent(lwAmplitudeSlider);
    lwAmplitudeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "luxwaveAmplitude", lwAmplitudeSlider);

    addChildComponent(lwScanModeCombo);
    lwScanModeCombo.addItem("Forward",     1);
    lwScanModeCombo.addItem("Ping-Pong",   2);
    lwScanModeCombo.addItem("Random",      3);
    lwScanModeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxwaveScanMode", lwScanModeCombo);

    // ── Image pipeline pages (formerly nested in IMAGE / ImagePageComponent) ──
    // The old IMAGE page is split: SOURCES / PITCH / MASK are now top-level
    // tabs, while LUXSTRAL / LUXSYNTH have moved under SYNTH as sub-tabs.
    sourcesPage     = std::make_unique<SourcesTabComponent>(audioProcessor);
    pitchPage       = std::make_unique<LuxPitchTabComponent>(audioProcessor);
    maskPage        = std::make_unique<LuxMaskTabComponent>(audioProcessor);
    imgLuxStralPage = std::make_unique<LuxStralTabComponent>(audioProcessor);
    imgLuxSynthPage = std::make_unique<LuxSynthTabComponent>(audioProcessor);

    addChildComponent(sourcesPage.get());
    addChildComponent(pitchPage.get());
    addChildComponent(maskPage.get());
    addChildComponent(imgLuxStralPage.get());
    addChildComponent(imgLuxSynthPage.get());

    // Wire node-click events from every image pipeline page so that the
    // active source is broadcast to the CIS visualiser and mirrored across
    // all pages (single source-of-truth for the "selected node" highlight).
    sourcesPage    ->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };
    pitchPage      ->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };
    maskPage       ->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };
    imgLuxStralPage->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };
    imgLuxSynthPage->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };

    // ── Sampler page (hidden by default) ──────────────────────────────────────
    samplerPage = std::make_unique<SamplerPageComponent>(audioProcessor);
    addChildComponent(samplerPage.get());

    // ── VIDEO page (hidden by default) ────────────────────────────────────────
    videoScrollPage = std::make_unique<VideoScrollTab>(audioProcessor);
    addChildComponent(videoScrollPage.get());

    // ── Header gear button ────────────────────────────────────────────────────
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    // Start on SOURCES tab
    switchToTab(Tab::Sources);
    juce::LookAndFeel::setDefaultLookAndFeel(&sp3ctraLaf);
    setSize(740, 760);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    settingsWindow.reset();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::handleNodeClicked(VisualizerMode mode)
{
    // Update visualizer source
    if (cisVisualizer)
        cisVisualizer->setActiveSource(mode);

    // Mirror the active-node highlight across every image pipeline page so
    // the selected node remains coherent when the user switches tabs.
    if (sourcesPage)     sourcesPage    ->setActiveMode(mode);
    if (pitchPage)       pitchPage      ->setActiveMode(mode);
    if (maskPage)        maskPage       ->setActiveMode(mode);
    if (imgLuxStralPage) imgLuxStralPage->setActiveMode(mode);
    if (imgLuxSynthPage) imgLuxSynthPage->setActiveMode(mode);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::switchToTab(Tab tab)
{
    currentTab = tab;
    const bool synthVis = (tab == Tab::Synth);

    // Sub-tab buttons — visible only on SYNTH page
    imgLuxStralSubBtn.setVisible(synthVis);
    imgLuxSynthSubBtn.setVisible(synthVis);
    audioStralSubBtn .setVisible(synthVis);
    audioSynthSubBtn .setVisible(synthVis);
    audioWaveSubBtn  .setVisible(synthVis);

    // Hide all synth audio controls first, then let switchSynthSubTab() show
    // the right ones if the SYNTH tab is active.
    std::array<juce::Component*, 13> luxstralCtrls = {
        &deviceOnToggle,   &luxstralVolumeSlider,
        &attackSlider,     &releaseSlider,
        &stereoEnableToggle, &stereoTempSlider, &sumExpSlider, &noiseGateSlider,
        &sfEnabledToggle,
        &sfMorphWidthSlider, &sfFocusSigmaSlider,
        &sfSpectralThreshSlider, &sfFocusOnlyToggle
    };
    for (auto* c : luxstralCtrls) c->setVisible(false);

    std::array<juce::Component*, 15> luxsynthCtrls = {
        &lxEnableToggle, &luxsynthVolumeSlider,
        &lxAttackSlider, &lxDecaySlider, &lxSustainSlider, &lxReleaseSlider,
        &lxFltAttackSlider, &lxFltDecaySlider, &lxFltSustainSlider, &lxFltReleaseSlider,
        &lxFltCutoffSlider, &lxFltDepthSlider,
        &lxNumOscSlider,
        &lxLfoRateSlider, &lxLfoDepthSlider
    };
    for (auto* c : luxsynthCtrls) if (c) c->setVisible(false);

    std::array<juce::Component*, 12> luxwaveCtrls = {
        &lwEnableToggle, &luxwaveVolumeSlider,
        &lwAttackSlider, &lwDecaySlider, &lwSustainSlider, &lwReleaseSlider,
        &lwFltCutoffSlider, &lwFltDepthSlider,
        &lwLfoRateSlider, &lwLfoDepthSlider,
        &lwAmplitudeSlider, &lwScanModeCombo
    };
    for (auto* c : luxwaveCtrls) if (c) c->setVisible(false);

    // Image pipeline sub-pages: hide all by default, switchSynthSubTab() will
    // show the active one when on the SYNTH tab.
    if (imgLuxStralPage) imgLuxStralPage->setVisible(false);
    if (imgLuxSynthPage) imgLuxSynthPage->setVisible(false);

    // Top-level image pipeline pages
    if (sourcesPage) sourcesPage->setVisible(tab == Tab::Sources);
    if (pitchPage)   pitchPage  ->setVisible(tab == Tab::Pitch);
    if (maskPage)    maskPage   ->setVisible(tab == Tab::Mask);

    // SAMPLER / VIDEO pages
    if (samplerPage) samplerPage->setVisible(tab == Tab::Sampler);
    if (videoScrollPage)
    {
        const bool videoVis = (tab == Tab::Video);
        videoScrollPage->setVisible(videoVis);
        if (videoVis)
            videoScrollPage->onTabActivated();
        else
            videoScrollPage->onTabDeactivated();
    }

    // Show the correct SYNTH sub-tab controls
    if (synthVis)
        switchSynthSubTab(currentSynthSub);

    // Blob overlay: visible on the source-listening pipeline pages (any image
    // tab including the SYNTH image sub-tabs).
    const bool imageMode = (tab == Tab::Sources)
                        || (tab == Tab::Pitch)
                        || (tab == Tab::Mask)
                        || (synthVis && (currentSynthSub == SynthSub::ImgLuxStral
                                       || currentSynthSub == SynthSub::ImgLuxSynth));
    if (cisVisualizer) cisVisualizer->setBlobOverlayVisible(imageMode);

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
    styleTab(sourcesTabBtn, tab == Tab::Sources);
    styleTab(pitchTabBtn,   tab == Tab::Pitch);
    styleTab(maskTabBtn,    tab == Tab::Mask);
    styleTab(samplerTabBtn, tab == Tab::Sampler);
    styleTab(synthTabBtn,   tab == Tab::Synth);
    styleTab(videoTabBtn,   tab == Tab::Video);

    resized();
    repaint();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::switchSynthSubTab(SynthSub sub)
{
    currentSynthSub = sub;

    const bool isImgStral  = (sub == SynthSub::ImgLuxStral);
    const bool isImgSynth  = (sub == SynthSub::ImgLuxSynth);
    const bool isAudStral  = (sub == SynthSub::AudioStral);
    const bool isAudSynth  = (sub == SynthSub::AudioSynth);
    const bool isAudWave   = (sub == SynthSub::AudioWave);

    // Image pipeline sub-pages
    if (imgLuxStralPage) imgLuxStralPage->setVisible(isImgStral);
    if (imgLuxSynthPage) imgLuxSynthPage->setVisible(isImgSynth);

    // AUDIOSTRAL controls (legacy LuxStral audio)
    deviceOnToggle      .setVisible(isAudStral);
    luxstralVolumeSlider.setVisible(isAudStral);
    attackSlider        .setVisible(isAudStral);
    releaseSlider       .setVisible(isAudStral);
    stereoEnableToggle  .setVisible(isAudStral);
    stereoTempSlider    .setVisible(isAudStral);
    sumExpSlider        .setVisible(isAudStral);
    noiseGateSlider     .setVisible(isAudStral);
    sfEnabledToggle     .setVisible(isAudStral);
    sfMorphWidthSlider  .setVisible(isAudStral);
    sfFocusSigmaSlider  .setVisible(isAudStral);
    sfSpectralThreshSlider.setVisible(isAudStral);
    sfFocusOnlyToggle   .setVisible(isAudStral);

    // AUDIOSYNTH controls (legacy LuxSynth audio)
    lxEnableToggle      .setVisible(isAudSynth);
    luxsynthVolumeSlider.setVisible(isAudSynth);
    lxAttackSlider      .setVisible(isAudSynth);
    lxDecaySlider       .setVisible(isAudSynth);
    lxSustainSlider     .setVisible(isAudSynth);
    lxReleaseSlider     .setVisible(isAudSynth);
    lxFltAttackSlider   .setVisible(isAudSynth);
    lxFltDecaySlider    .setVisible(isAudSynth);
    lxFltSustainSlider  .setVisible(isAudSynth);
    lxFltReleaseSlider  .setVisible(isAudSynth);
    lxFltCutoffSlider   .setVisible(isAudSynth);
    lxFltDepthSlider    .setVisible(isAudSynth);
    lxNumOscSlider      .setVisible(isAudSynth);
    lxLfoRateSlider     .setVisible(isAudSynth);
    lxLfoDepthSlider    .setVisible(isAudSynth);

    // AUDIOWAVE controls (legacy LuxWave audio)
    lwEnableToggle      .setVisible(isAudWave);
    luxwaveVolumeSlider .setVisible(isAudWave);
    lwAttackSlider      .setVisible(isAudWave);
    lwDecaySlider       .setVisible(isAudWave);
    lwSustainSlider     .setVisible(isAudWave);
    lwReleaseSlider     .setVisible(isAudWave);
    lwFltCutoffSlider   .setVisible(isAudWave);
    lwFltDepthSlider    .setVisible(isAudWave);
    lwLfoRateSlider     .setVisible(isAudWave);
    lwLfoDepthSlider    .setVisible(isAudWave);
    lwAmplitudeSlider   .setVisible(isAudWave);
    lwScanModeCombo     .setVisible(isAudWave);

    // Blob overlay follows image-pipeline sub-tabs
    if (cisVisualizer) cisVisualizer->setBlobOverlayVisible(isImgStral || isImgSynth);

    // Sub-tab button styling
    auto styleSubTab = [](juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      juce::Colour(active ? 0xff2a3a50 : 0xff1a1a24));
        btn.setColour(juce::TextButton::textColourOffId,
                      juce::Colour(active ? 0xffc0d8f0 : 0xff667788));
    };
    styleSubTab(imgLuxStralSubBtn, isImgStral);
    styleSubTab(imgLuxSynthSubBtn, isImgSynth);
    styleSubTab(audioStralSubBtn,  isAudStral);
    styleSubTab(audioSynthSubBtn,  isAudSynth);
    styleSubTab(audioWaveSubBtn,   isAudWave);

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
        const juce::TextButton* tabs[] = {
            &sourcesTabBtn, &pitchTabBtn, &maskTabBtn,
            &samplerTabBtn, &synthTabBtn, &videoTabBtn
        };
        const Tab tabIds[] = {
            Tab::Sources, Tab::Pitch, Tab::Mask,
            Tab::Sampler, Tab::Synth, Tab::Video
        };
        constexpr int kNumTabs = (int)(sizeof(tabIds) / sizeof(tabIds[0]));

        for (int i = 0; i < kNumTabs; ++i)
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

        // Active sub-tab accent underline
        const juce::TextButton* activeBtn = nullptr;
        switch (currentSynthSub)
        {
            case SynthSub::ImgLuxStral: activeBtn = &imgLuxStralSubBtn; break;
            case SynthSub::ImgLuxSynth: activeBtn = &imgLuxSynthSubBtn; break;
            case SynthSub::AudioStral:  activeBtn = &audioStralSubBtn;  break;
            case SynthSub::AudioSynth:  activeBtn = &audioSynthSubBtn;  break;
            case SynthSub::AudioWave:   activeBtn = &audioWaveSubBtn;   break;
        }
        if (activeBtn != nullptr)
        {
            g.setColour(juce::Colour(0xff4488cc));
            g.fillRect(activeBtn->getX(), kSubTabsY + kSubTabsH - 2,
                       activeBtn->getWidth(), 2);
        }
    }

    // ── SYNTH page labels (drawn inline — image pages draw themselves) ────────
    if (currentTab == Tab::Synth && currentSynthSub == SynthSub::AudioStral)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rsy = rowsStartY();

        // AUDIOSTRAL section badge
        g.setColour(juce::Colour(0xff1c3755));
        g.fillRoundedRectangle(juce::Rectangle<int>(lxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xff7ab0f0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("AUDIOSTRAL",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Row labels — 8 audio-only rows (left col)
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

    // ── AUDIOSYNTH labels ────────────────────────────────────────────────────
    if (currentTab == Tab::Synth && currentSynthSub == SynthSub::AudioSynth)
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
        g.drawText("AUDIOSYNTH  --  Volume ADSR + Spectral",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Left column labels (8 rows)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        static const char* const lxLeftLbls[] = {
            "Enable", "Volume", "Attack", "Decay", "Sustain", "Release",
            "Oscillators", "LFO Rate"
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

    // ── AUDIOWAVE labels ─────────────────────────────────────────────────────
    if (currentTab == Tab::Synth && currentSynthSub == SynthSub::AudioWave)
    {
        const int cw  = colWidth();
        const int lxp = colLX();
        const int rxp = colRX();
        const int rsy = rowsStartY();

        // Left column badge — Wavetable ADSR (orange-gold)
        g.setColour(juce::Colour(0xff3a2a1a));
        g.fillRoundedRectangle(juce::Rectangle<int>(lxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xffddaa44));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("AUDIOWAVE  --  Wavetable ADSR + Scan",
                   juce::Rectangle<int>(lxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Left column labels (8 rows)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        static const char* const lwLeftLbls[] = {
            "Enable", "Volume", "Amplitude", "Attack", "Decay", "Sustain", "Release",
            "Scan Mode"
        };
        for (int i = 0; i < 8; ++i)
            g.drawText(lwLeftLbls[i],
                       juce::Rectangle<int>(lxp, rsy + i*kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);

        // Right column badge — Filter + LFO (teal-dark)
        g.setColour(juce::Colour(0xff1a2a3a));
        g.fillRoundedRectangle(juce::Rectangle<int>(rxp, kPageTop, cw, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(0xff66aacc));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("FILTER + LFO",
                   juce::Rectangle<int>(rxp+6, kPageTop, cw-12, kSectionH),
                   juce::Justification::centredLeft, true);

        // Right column labels (4 rows)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        static const char* const lwRightLbls[] = {
            "Flt. Cutoff", "Flt. Env Depth", "LFO Rate", "LFO Depth"
        };
        for (int i = 0; i < 4; ++i)
            g.drawText(lwRightLbls[i],
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

    // ── Tab buttons (SOURCES | PITCH | MASK | SAMPLER | SYNTH | VIDEO) ────────
    // Compact widths so all 6 tabs fit comfortably within the editor width.
    {
        // Tab widths tuned to label length.
        constexpr int kSourcesW = 80;
        constexpr int kPitchW   = 60;
        constexpr int kMaskW    = 60;
        constexpr int kSamplerW = 80;
        constexpr int kSynthW   = 64;
        constexpr int kVideoW   = 60;
        constexpr int kGap      = 2;

        int x = kHPad;
        sourcesTabBtn.setBounds(x, kTabsY + 2, kSourcesW, Sp3ctraTheme::kTabBtnH); x += kSourcesW + kGap;
        pitchTabBtn  .setBounds(x, kTabsY + 2, kPitchW,   Sp3ctraTheme::kTabBtnH); x += kPitchW   + kGap;
        maskTabBtn   .setBounds(x, kTabsY + 2, kMaskW,    Sp3ctraTheme::kTabBtnH); x += kMaskW    + kGap;
        samplerTabBtn.setBounds(x, kTabsY + 2, kSamplerW, Sp3ctraTheme::kTabBtnH); x += kSamplerW + kGap;
        synthTabBtn  .setBounds(x, kTabsY + 2, kSynthW,   Sp3ctraTheme::kTabBtnH); x += kSynthW   + kGap;
        videoTabBtn  .setBounds(x, kTabsY + 2, kVideoW,   Sp3ctraTheme::kTabBtnH);
    }

    // ── Synth sub-tab buttons (5 buttons) ────────────────────────────────────
    {
        constexpr int kSubW = 80;
        constexpr int kGap  = 4;
        int x = kHPad;
        imgLuxStralSubBtn.setBounds(x, kSubTabsY + 1, kSubW, kSubTabsH - 2); x += kSubW + kGap;
        imgLuxSynthSubBtn.setBounds(x, kSubTabsY + 1, kSubW, kSubTabsH - 2); x += kSubW + kGap;
        audioStralSubBtn .setBounds(x, kSubTabsY + 1, kSubW, kSubTabsH - 2); x += kSubW + kGap;
        audioSynthSubBtn .setBounds(x, kSubTabsY + 1, kSubW, kSubTabsH - 2); x += kSubW + kGap;
        audioWaveSubBtn  .setBounds(x, kSubTabsY + 1, kSubW, kSubTabsH - 2);
    }

    // ── AUDIOSTRAL controls (SYNTH page, left column) ─────────────────────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        deviceOnToggle    .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        luxstralVolumeSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
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

    // ── AUDIOSYNTH controls (SYNTH page, left column = vol ADSR + spectral) ──
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        lxEnableToggle      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        luxsynthVolumeSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxAttackSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxDecaySlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxSustainSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxReleaseSlider.setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxNumOscSlider .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lxLfoRateSlider.setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── AUDIOSYNTH controls (SYNTH page, right column = filter ADSR + LFO) ───
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

    // ── AUDIOWAVE controls (SYNTH page, left column = ADSR + Volume) ─────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;
        lwEnableToggle      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        luxwaveVolumeSlider .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwAmplitudeSlider   .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwAttackSlider      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwDecaySlider       .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwSustainSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwReleaseSlider     .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        lwScanModeCombo     .setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── AUDIOWAVE controls (SYNTH page, right column = Filter + LFO) ─────────
    {
        const int rxp = colRX();
        const int cw  = colWidth();
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = cw - kCtrlOffset - 12;
        int cy = rsy;
        lwFltCutoffSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwFltDepthSlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwLfoRateSlider   .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwLfoDepthSlider  .setBounds(cx, cy, cw2, kRowH);
    }

    // ── Header gear button (top-right) ────────────────────────────────────────
    const int btnSz = kHeaderH - 8;
    settingsButton.setBounds(getWidth() - btnSz - 4, 4, btnSz, btnSz);

    // ── Top-level image pipeline pages share the full page area ──────────────
    // Note: the SYNTH image sub-tabs start lower (below the sub-tab bar).
    const int pageH       = getHeight() - kPageTop - 8;
    const int topPageY    = kSubTabsY;  // SOURCES/PITCH/MASK have no sub-tab bar
    const int topPageH    = getHeight() - topPageY - 8;
    const int pageW       = getWidth() - 2 * kHPad;

    if (sourcesPage) sourcesPage->setBounds(kHPad, topPageY, pageW, topPageH);
    if (pitchPage)   pitchPage  ->setBounds(kHPad, topPageY, pageW, topPageH);
    if (maskPage)    maskPage   ->setBounds(kHPad, topPageY, pageW, topPageH);

    // SYNTH image sub-pages sit below the sub-tab bar
    if (imgLuxStralPage) imgLuxStralPage->setBounds(kHPad, kPageTop, pageW, pageH);
    if (imgLuxSynthPage) imgLuxSynthPage->setBounds(kHPad, kPageTop, pageW, pageH);

    // ── Sampler page ──────────────────────────────────────────────────────────
    if (samplerPage)
        samplerPage->setBounds(kHPad, topPageY, pageW, topPageH);

    // ── Video scroll page ─────────────────────────────────────────────────────
    if (videoScrollPage)
        videoScrollPage->setBounds(kHPad, topPageY, pageW, topPageH);
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
