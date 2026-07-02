#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "IconPaths.h"
#include "ui/ScrollWheelGuard.h"

//==============================================================================
Sp3ctraAudioProcessorEditor::Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    auto& apvts = audioProcessor.getAPVTS();

    // ── ZONE 1: CIS Visualizer (full width, selection-driven) ─────────────────
    cisVisualizer = std::make_unique<CisVisualizerComponent>(audioProcessor);
    addAndMakeVisible(cisVisualizer.get());

    // ── Keyboard ruler strip directly under zone 1 (M5) ───────────────────────
    // Same x-extent as the visualizer so pixel columns align; visible only
    // while the selected block is PITCH or MASK (see selectBlock()).
    keyboardRuler = std::make_unique<KeyboardRulerComponent>(audioProcessor);
    addChildComponent(keyboardRuler.get());

    // ── MODULE CATALOGUE rail (far left — drag source for the chain rack) ─────
    catalogViewport.setViewedComponent(&moduleCatalog, false);
    catalogViewport.setScrollBarsShown(true, false);
    catalogViewport.setScrollBarThickness(8);
    addAndMakeVisible(catalogViewport);

    // Collapse / expand controls for the catalogue rail (mirrors ZONE 4).
    // Collapsing hides the rail and locks the chain rack (no deletion; reorder
    // still works); expanding restores both.
    catalogCollapseBtn.setTooltip("Hide modules - locks chain edits (reorder still works)");
    catalogCollapseBtn.onClick = [this] { setCatalogCollapsed(true); };
    addChildComponent(catalogCollapseBtn);
    catalogExpandBtn.setTooltip("Show modules - unlocks chain edits");
    catalogExpandBtn.onClick = [this] { setCatalogCollapsed(false); };
    addChildComponent(catalogExpandBtn);

    // ── ZONE 2: chain rack inside a vertical viewport ─────────────────────────
    chainRack = std::make_unique<ChainRackComponent>(audioProcessor);
    chainRack->onBlockSelected = [this](ChainBlockId id) { selectBlock(id); };
    // Selecting a VIDEO SCROLL output binds the contextual panel to that
    // instance's bank (fires just before onBlockSelected → selectBlock).
    chainRack->onVideoBlockSelected = [this](int slot)
    {
        if (videoScrollPage) videoScrollPage->setSlot(slot);
    };
    // Selecting a SAMPLER block binds the sampler page + setup to engine A/B
    // (slot 0 = A, 1 = B), fired just before onBlockSelected → selectBlock.
    chainRack->onSamplerBlockSelected = [this](int slot)
    {
        if (samplerPage)  samplerPage ->setSamplerIndex(slot);
        if (samplerSetup) samplerSetup->setSamplerIndex(slot);
    };
    // Selecting a LUXSTRAL block binds the module page + setup to engine A/B
    // (slot 0 = A, 1 = B), fired just before onBlockSelected → selectBlock —
    // so the PLAY/SETUP faces edit the right engine's parameter set (M8).
    chainRack->onLuxStralBlockSelected = [this](int slot)
    {
        luxStralEngineIndex_ = (slot == 1) ? 1 : 0;
        if (imgLuxStralPage) imgLuxStralPage->setEngineIndex(luxStralEngineIndex_);
        if (stralSetup)      stralSetup->setEngineIndex(luxStralEngineIndex_);
    };
    // A chain edit changes the rack's preferred height → re-run the zone layout.
    chainRack->onModelChanged  = [this]
    {
        layoutZones();
        if (waterfallColumn) waterfallColumn->refreshActiveSlots();   // outputs added/removed
    };
    rackViewport.setViewedComponent(chainRack.get(), false);
    rackViewport.setScrollBarsShown(true, false);
    rackViewport.setScrollBarThickness(8);
    addAndMakeVisible(rackViewport);

    // ── ZONE 3: block editor host (vertical viewport + content container) ─────
    zone3Viewport.setViewedComponent(&zone3Content, false);
    zone3Viewport.setScrollBarsShown(true, false);
    zone3Viewport.setScrollBarThickness(8);
    addAndMakeVisible(zone3Viewport);

    // Image pipeline pages (reused as-is from the former tab layout)
    sourcesPage     = std::make_unique<SourcesTabComponent>(audioProcessor);
    pitchPage       = std::make_unique<LuxPitchTabComponent>(audioProcessor);
    maskPage        = std::make_unique<LuxMaskTabComponent>(audioProcessor);
    imgLuxStralPage = std::make_unique<LuxStralTabComponent>(audioProcessor);
    // Stereo / StrokeForge toggles flip contextual top-bandeau panels (COLOR /
    // BLOB) on/off.  Defer to the next message tick so the APVTS attachment has
    // committed the new value before we re-read it; SafePointer guards teardown.
    imgLuxStralPage->onVisualizerSourcesChanged = [this]
    {
        juce::Component::SafePointer<Sp3ctraAudioProcessorEditor> sp(this);
        juce::MessageManager::callAsync([sp] { if (sp != nullptr) sp->refreshVisualizerSources(); });
    };
    imgLuxSynthPage = std::make_unique<LuxSynthTabComponent>(audioProcessor);

    zone3Content.addChildComponent(sourcesPage.get());
    zone3Content.addChildComponent(pitchPage.get());
    zone3Content.addChildComponent(maskPage.get());
    zone3Content.addChildComponent(imgLuxStralPage.get());
    zone3Content.addChildComponent(imgLuxSynthPage.get());

    // Sampler page (reused as-is)
    samplerPage = std::make_unique<SamplerPageComponent>(audioProcessor);
    zone3Content.addChildComponent(samplerPage.get());

    // SEQUENCER — step sequencer extracted from the sampler page into its own
    // module page (grid + transport/config bar).
    sequencerPage = std::make_unique<SequencerPageComponent>(audioProcessor);
    zone3Content.addChildComponent(sequencerPage.get());

    // SCORE — offline printable-spectrogram export tool (no SETUP face)
    scorePage = std::make_unique<ScoreGenTabComponent>(audioProcessor);
    zone3Content.addChildComponent(scorePage.get());

    videoScrollPage = std::make_unique<VideoScrollPage>(audioProcessor);
    zone3Content.addChildComponent(videoScrollPage.get());

    // Engine audio panels — the former SYNTH AUDIOSYNTH/AUDIOWAVE sub-pages,
    // repackaged as components (same params & attachments).  AUDIOSTRAL is now
    // part of the LUXSTRAL module page (imgLuxStralPage) itself.
    audioSynthPanel = std::make_unique<AudioSynthPanel>(audioProcessor);
    audioWavePanel  = std::make_unique<AudioWavePanel>(audioProcessor);
    zone3Content.addChildComponent(audioSynthPanel.get());
    zone3Content.addChildComponent(audioWavePanel.get());

    // SETUP faces (M5) — per-block settings migrated from the gear-wheel
    // window (same params & attachments), accent-matched to the chain rack.
    sourceSetup  = std::make_unique<SourceSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Chain1Source));
    pitchSetup   = std::make_unique<PitchSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Pitch));
    maskSetup    = std::make_unique<MaskSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Mask));
    stralSetup   = std::make_unique<LuxStralSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxStral));
    synthSetup   = std::make_unique<LuxSynthSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxSynth));
    waveSetup    = std::make_unique<LuxWaveSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxWave));
    samplerSetup = std::make_unique<SamplerSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Sampler));
    scoreSetup   = std::make_unique<ScoreSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Score));
    zone3Content.addChildComponent(sourceSetup.get());
    zone3Content.addChildComponent(pitchSetup.get());
    zone3Content.addChildComponent(maskSetup.get());
    zone3Content.addChildComponent(stralSetup.get());
    zone3Content.addChildComponent(synthSetup.get());
    zone3Content.addChildComponent(waveSetup.get());
    zone3Content.addChildComponent(samplerSetup.get());
    zone3Content.addChildComponent(scoreSetup.get());

    // PLAY | SETUP face switcher (above the zone-3 viewport). Every block now
    // has a SETUP face — the SP3CTRA source hosts the network/CIS config there.
    faceSwitch.onFaceChanged = [this](bool setup)
    {
        setupFace = setup;
        applyZone3Visibility();
        layoutZone3();
        zone3Viewport.setViewPosition(0, 0);
    };
    addChildComponent(faceSwitch);
    addChildComponent(modulePowerButton);

    // ── ZONE 4: video scroll column (hosts the former VIDEO tab) ──────────────
    waterfallColumn = std::make_unique<VideoMixerColumn>(audioProcessor);
    waterfallColumn->onCollapseToggled = [this](bool)
    {
        layoutZones();
        persistLayoutProps();
    };
    addAndMakeVisible(waterfallColumn.get());

    // ── Splitters (zone2|zone3 and zone3|zone4) ───────────────────────────────
    splitterLeft.onDragStart = [this] { splitterDragStartW = zone2Width; };
    splitterLeft.onDragged   = [this](int dx)
    {
        zone2Width = splitterDragStartW + dx;   // clamped in layoutZones()
        layoutZones();
    };
    splitterLeft.onDragEnd   = [this] { persistLayoutProps(); };
    addAndMakeVisible(splitterLeft);

    splitterRight.onDragStart = [this] { splitterDragStartW = zone4Width; };
    splitterRight.onDragged   = [this](int dx)
    {
        zone4Width = splitterDragStartW - dx;   // clamped in layoutZones()
        layoutZones();
    };
    splitterRight.onDragEnd   = [this] { persistLayoutProps(); };
    addAndMakeVisible(splitterRight);

    // ── Header gear button ────────────────────────────────────────────────────
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    // ── Header panic button (All Notes Off) ───────────────────────────────────
    panicButton.onClick = [this]
    {
        // Signal the audio thread to release every held/stuck note next block.
        audioProcessor.requestAllNotesOff();
    };
    addAndMakeVisible(panicButton);

    // ── Restore persisted layout (survives session reload) ────────────────────
    auto& state = apvts.state;
    zone2Width = (int) state.getProperty("zone2W", kZone2DefaultW);
    zone4Width = (int) state.getProperty("zone4W", kZone4DefaultW);
    if ((bool) state.getProperty("scrollCollapsed", false))
        waterfallColumn->setCollapsed(true, false);
    if ((bool) state.getProperty("catalogCollapsed", false))
        setCatalogCollapsed(true, false);   // also locks the chain rack

    // Initial selection: chain 1 source (zone 1 → Chain 1 view)
    selectBlock(ChainBlockId::Chain1Source);

    juce::LookAndFeel::setDefaultLookAndFeel(&sp3ctraLaf);

    // ── Resizable editor + persisted size ─────────────────────────────────────
    setResizable(true, true);
    setResizeLimits(kMinW, kMinH, kMaxW, kMaxH);
    const int w = juce::jlimit(kMinW, kMaxW, (int) state.getProperty("editorW", kDefaultW));
    const int h = juce::jlimit(kMinH, kMaxH, (int) state.getProperty("editorH", kDefaultH));
    setSize(w, h);

    // Scrolling a panel should never nudge the knob/slider under the cursor:
    // disable wheel-driven value changes on every Slider in the editor tree.
    Sp3ctraUI::disableSliderScrollWheel(*this);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    settingsWindow.reset();
}

//==============================================================================
bool Sp3ctraAudioProcessorEditor::blockHasSetup(ChainBlockId id) noexcept
{
    // Every block has a SETUP face — the SP3CTRA source hosts the network/CIS
    // configuration there (formerly the gear-wheel Network tab) — EXCEPT the
    // SEQUENCER module, whose controls all live on its single PLAY page, and the
    // VIDEO SCROLL output, whose params are all on its PLAY page (no setup).
    return id != ChainBlockId::Sequencer && id != ChainBlockId::VideoScroll;
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::applyZone3Visibility()
{
    const auto id   = selectedBlock;
    const bool play = !setupFace;

    // ── PLAY face: the page (or stacked pages) for this block ────────────────
    const bool showSources = play && (id == ChainBlockId::Chain1Source
                                   || id == ChainBlockId::Chain2Source);
    if (sourcesPage)     sourcesPage    ->setVisible(showSources);
    if (pitchPage)       pitchPage      ->setVisible(play && id == ChainBlockId::Pitch);
    if (maskPage)        maskPage       ->setVisible(play && id == ChainBlockId::Mask);
    if (samplerPage)     samplerPage    ->setVisible(play && id == ChainBlockId::Sampler);
    if (sequencerPage)   sequencerPage  ->setVisible(play && id == ChainBlockId::Sequencer);
    if (imgLuxStralPage) imgLuxStralPage->setVisible(play && id == ChainBlockId::LuxStral);
    if (imgLuxSynthPage) imgLuxSynthPage->setVisible(play && id == ChainBlockId::LuxSynth);
    if (audioSynthPanel) audioSynthPanel->setVisible(play && id == ChainBlockId::LuxSynth);
    if (audioWavePanel)  audioWavePanel ->setVisible(play && id == ChainBlockId::LuxWave);
    if (scorePage)       scorePage      ->setVisible(play && id == ChainBlockId::Score);
    if (videoScrollPage) videoScrollPage->setVisible(play && id == ChainBlockId::VideoScroll);

    // ── SETUP face: the per-block settings panel ──────────────────────────────
    if (sourceSetup)  sourceSetup ->setVisible(setupFace && (id == ChainBlockId::Chain1Source
                                                          || id == ChainBlockId::Chain2Source));
    if (pitchSetup)   pitchSetup  ->setVisible(setupFace && id == ChainBlockId::Pitch);
    if (maskSetup)    maskSetup   ->setVisible(setupFace && id == ChainBlockId::Mask);
    if (samplerSetup) samplerSetup->setVisible(setupFace && id == ChainBlockId::Sampler);
    if (scoreSetup)   scoreSetup  ->setVisible(setupFace && id == ChainBlockId::Score);
    if (stralSetup)   stralSetup  ->setVisible(setupFace && id == ChainBlockId::LuxStral);
    if (synthSetup)   synthSetup  ->setVisible(setupFace && id == ChainBlockId::LuxSynth);
    if (waveSetup)    waveSetup   ->setVisible(setupFace && id == ChainBlockId::LuxWave);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::selectBlock(ChainBlockId id)
{
    // Selecting another block always lands on the PLAY face (M5).
    if (id != selectedBlock)
        setupFace = false;

    selectedBlock = id;
    if (chainRack)
        chainRack->setSelectedBlock(id);

    // ── ZONE 1: stacked visualizer panels — ALL outputs of this block ─────────
    // The visualizer shows every output of the selected module simultaneously,
    // one stacked panel each (contextual to the module).  The FIRST entry is
    // the primary output and drives the synthesis side-effects.  The panel
    // count is resolved here, BEFORE layoutZones(), because it sets ZONE 1's
    // height (and therefore the top of zones 2/3/4).
    std::vector<VisualizerMode> sources;
    switch (id)
    {
        // Each SOURCE CIS is contextual to the chain it sits on: it shows ONLY
        // that chain's output (Chain 1 = Modulated, Chain 2 = Live).  The RAW
        // upstream feed is the instrument's own signal and no longer surfaces
        // here — the migration treats chains as modular slots.
        case ChainBlockId::Chain1Source:
            sources = { VisualizerMode::MODULATED };   // Chain 1
            break;
        case ChainBlockId::Chain2Source:
            sources = { VisualizerMode::LIVE };         // Chain 2
            break;
        case ChainBlockId::Pitch:
            sources = { VisualizerMode::LUXPITCH_OUTPUT };
            break;
        case ChainBlockId::Mask:
            sources = { VisualizerMode::LUXMASK_OUTPUT };
            break;
        // No dedicated SAMPLER view exists any more (deprecated alias) — the
        // sampler lives inside the Modulated channel, so show MODULATED.
        case ChainBlockId::Sampler:
            sources = { VisualizerMode::MODULATED };
            break;
        case ChainBlockId::LuxStral:
            sources = luxStralVisualizerSources();
            break;
        case ChainBlockId::LuxSynth:
            sources = { VisualizerMode::SYNTH_GRAY,
                        VisualizerMode::SYNTH_COLOR,
                        VisualizerMode::SYNTH_BLOB,
                        VisualizerMode::SYNTH_FFT_COLOR };
            break;
        case ChainBlockId::LuxWave:
            sources = { VisualizerMode::SYNTH_GRAY };
            break;
        // SCORE is an offline export tool — keep a neutral live view in zone 1.
        case ChainBlockId::Score:
            sources = { VisualizerMode::MODULATED };
            break;
        // SEQUENCER drives the sampler engines — neutral Modulated view in zone 1.
        case ChainBlockId::Sequencer:
            sources = { VisualizerMode::MODULATED };
            break;
        // VIDEO SCROLL output — neutral Modulated view in zone 1 (its own waterfall
        // is composited in the right-band mixer, not here).
        case ChainBlockId::VideoScroll:
            sources = { VisualizerMode::MODULATED };
            break;
    }
    // The SOURCES page transport follows the selected chain (1 or 2).
    if (sourcesPage)
    {
        if (id == ChainBlockId::Chain1Source)      sourcesPage->setChain(1);
        else if (id == ChainBlockId::Chain2Source) sourcesPage->setChain(2);
    }

    visPanelCount_ = juce::jmax(1, static_cast<int>(sources.size()));
    if (cisVisualizer)
    {
        cisVisualizer->setActiveSources(sources);

        // Blob overlay follows image-pipeline blocks (audio-only / sampler: off),
        // mirroring the former per-tab behaviour.
        const bool imageMode = (id != ChainBlockId::Sampler && id != ChainBlockId::LuxWave);
        cisVisualizer->setBlobOverlayVisible(imageMode);
    }

    // ── PLAY | SETUP switcher: visibility + accent follow the selection ──────
    if (!blockHasSetup(id))
        setupFace = false;
    faceSwitch.setVisible(blockHasSetup(id));
    faceSwitch.setAccent(ChainRackComponent::blockColour(id));
    faceSwitch.setFace(setupFace, false);

    // Module power toggle (right of the face row) — rebind to this block's enable
    // param, or hide for blocks without a power switch (SOURCE CIS, SCORE).
    {
        juce::String enableId = ChainRackComponent::enableParamId(id);
        // LuxStral engine B powers through its own param (the type-level
        // mapping returns deviceEnabled, which is engine A's toggle).
        if (id == ChainBlockId::LuxStral && luxStralEngineIndex_ == 1)
            enableId = "luxstralBEnabled";
        const bool showPower = blockHasSetup(id) && enableId.isNotEmpty();
        modulePowerAttachment.reset();   // detach before rebinding to a new param
        if (showPower)
        {
            modulePowerButton.setAccent(ChainRackComponent::blockColour(id));
            modulePowerAttachment = std::make_unique<
                juce::AudioProcessorValueTreeState::ButtonAttachment>(
                    audioProcessor.getAPVTS(), enableId, modulePowerButton);
        }
        modulePowerButton.setVisible(showPower);
    }

    // ── Keyboard ruler under zone 1 (M5): PITCH / MASK only ──────────────────
    if (keyboardRuler)
    {
        if (id == ChainBlockId::Pitch)
            keyboardRuler->setModule(KeyboardRulerComponent::Module::Pitch);
        else if (id == ChainBlockId::Mask)
            keyboardRuler->setModule(KeyboardRulerComponent::Module::Mask);
        keyboardRuler->setVisible(id == ChainBlockId::Pitch
                               || id == ChainBlockId::Mask);
    }

    // ── ZONE 3: show the page(s) / setup panel for this block ────────────────
    applyZone3Visibility();

    layoutZones();   // face-bar visibility changes the zone-3 viewport bounds
    zone3Viewport.setViewPosition(0, 0);

    repaint();
}

//==============================================================================
std::vector<VisualizerMode>
Sp3ctraAudioProcessorEditor::luxStralVisualizerSources() const
{
    // Contextual top-bandeau panels.  GRAY (the additive base) is always shown;
    // COLOR appears only when Stereo is on (colour-temperature extraction drives
    // the per-oscillator panning); BLOB appears only when StrokeForge is on.
    // When a panel is hidden its computation is skipped too — see image_pipeline.c
    // Stage 8 (pan, gated on stereo) and Stage 9 (blob, gated on StrokeForge).
    auto& apvts = audioProcessor.getAPVTS();
    std::vector<VisualizerMode> s { VisualizerMode::SPCTR_GRAY };
    // COLOR follows the SELECTED engine's stereo toggle (A/B have their own).
    const char* stereoId = (luxStralEngineIndex_ == 1) ? "luxstralBStereoEnable"
                                                       : "luxstralStereoEnable";
    if (apvts.getRawParameterValue(stereoId)->load() > 0.5f)
        s.push_back(VisualizerMode::SPCTR_COLOR);
    if (apvts.getRawParameterValue("sfEnabled")->load() > 0.5f)
        s.push_back(VisualizerMode::SPCTR_BLOB);
    return s;
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::refreshVisualizerSources()
{
    if (selectedBlock != ChainBlockId::LuxStral)
        return;  // only LUXSTRAL has contextual (toggle-driven) panels

    const auto sources = luxStralVisualizerSources();
    visPanelCount_ = juce::jmax(1, static_cast<int>(sources.size()));
    if (cisVisualizer)
        cisVisualizer->setActiveSources(sources);

    layoutZones();   // panel count drives ZONE 1 height → reflow zones 2/3/4
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

    // ── Separator between zone 1 (visualizer [+ keyboard ruler]) and zones ───
    g.setColour(juce::Colour(0xff333333));
    g.fillRect(0, zonesTopY() - 1, getWidth(), 1);

    // ── Module catalogue rail — mirrors ZONE 4 (header band / collapsed grip).
    //    The rack sits flush to the right; its own left border is the divider. ─
    {
        const int ry    = zonesTopY();
        const int rh    = juce::jmax(0, getHeight() - ry);

        if (catalogCollapsed)
        {
            // Collapsed grip: expand button (laid out in resized) + dotted spine
            // + rotated "MODULES" caption, centred — same as the VIDEO SCROLL grip.
            g.setColour(juce::Colour(0xff14141c));
            g.fillRect(0, ry, kCatGripW, rh);

            const float cx = kCatGripW * 0.5f;
            const float cy = ry + rh * 0.5f;

            const int spineTop = ry + 4 + 18 + 10;   // below the expand button
            g.setColour(juce::Colour(0xff2c2c3a));
            for (int y = spineTop; y < getHeight() - 12; y += 9)
                g.fillEllipse(cx - 1.5f, (float) y, 3.f, 3.f);

            g.setColour(juce::Colour(0xff7a86a0));
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
            g.saveState();
            g.addTransform(juce::AffineTransform::rotation(
                juce::MathConstants<float>::halfPi, cx, cy));
            g.drawText("MODULES",
                       juce::Rectangle<float>(cx - 130.f, cy - 9.f, 260.f, 18.f),
                       juce::Justification::centred, false);
            g.restoreState();
        }
        else
        {
            // Header band: same bg + title weight as ZONE 4 so the collapse ✕
            // button blends in identically.
            g.setColour(juce::Colour(0xff1f1f2c));
            g.fillRect(0, ry, kPaletteW, kCatHeaderH);

            const int btn = kCatHeaderH - 4;
            g.setColour(juce::Colour(0xff66cc88));
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
            g.drawText("MODULES", 8, ry, juce::jmax(0, kPaletteW - btn - 12), kCatHeaderH,
                       juce::Justification::centredLeft, false);

            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
            g.fillRect(0, ry + kCatHeaderH - 1, kPaletteW, 1);

            // Body bg behind the viewport: keeps the scrollbar-reserved strip the
            // rail's colour (never the window bg) right up to the rack edge.
            g.setColour(juce::Colour(0xff14141c));
            g.fillRect(0, ry + kCatHeaderH, kPaletteW, juce::jmax(0, rh - kCatHeaderH));
        }
    }

    // ZONE 5 (reserved — output / master / monitoring) is a collapsed strip
    // of height 0 at the bottom; nothing is drawn for it yet.
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::resized()
{
    // ── Header gear button (top-right) ────────────────────────────────────────
    const int btnSz = kHeaderH - 8;
    settingsButton.setBounds(getWidth() - btnSz - 4, 4, btnSz, btnSz);
    const int panicW = btnSz * 3 / 2;   // wider, rectangular: holds "PANIC"
    panicButton.setBounds(getWidth() - btnSz - 8 - panicW, 4, panicW, btnSz);

    // ── ZONE 1: CIS Visualizer — full window width; height = panel count ─────
    if (cisVisualizer)
        cisVisualizer->setBounds(kHPad, kVisY, getWidth() - 2 * kHPad, visHeight());

    layoutZones();

    // Persist the window size (+ current zone widths) in the session state.
    persistLayoutProps();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::setCatalogCollapsed(bool shouldCollapse, bool persist)
{
    catalogCollapsed = shouldCollapse;

    // Performance lock follows the rail: collapsed → chains can be reordered but
    // not deleted; expanded → full editing returns.
    if (chainRack)
        chainRack->setLocked(catalogCollapsed);

    layoutZones();            // rail width + button visibility + zone reflow
    if (persist)
        persistLayoutProps();
    repaint();                // header band / collapsed grip background
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::layoutZones()
{
    if (chainRack == nullptr || waterfallColumn == nullptr)
        return;

    const int W = getWidth();
    const int H = getHeight();
    if (W <= 0 || H <= 0)
        return;

    // ZONE 1 height tracks the active panel count — re-apply it here so block
    // selection (which calls layoutZones, not resized) resizes the strip.
    if (cisVisualizer)
        cisVisualizer->setBounds(kHPad, kVisY, W - 2 * kHPad, visHeight());

    // Keyboard ruler (M5): directly under zone 1, same x-extent as the
    // visualizer; it takes kRulerH px from the top of the zones row.
    if (keyboardRuler && keyboardRuler->isVisible())
        keyboardRuler->setBounds(kHPad, kVisY + visHeight(),
                                 juce::jmax(50, W - 2 * kHPad), kRulerH);

    // ZONE 5 (reserved — output / master / monitoring) is a collapsed strip
    // h = 0 at the bottom: when it materialises, give zone5H its height here.
    const int zone5H = 0;
    const int zonesY = zonesTopY();
    const int zonesH = juce::jmax(0, H - zonesY - zone5H);

    // ── Module catalogue rail (collapsible, far left; scrolls if tall) ────────
    // Collapsed → a thin grip with the expand button; expanded → a header band
    // (title + collapse ✕) above the scrolling catalogue viewport.
    const int catRailW = catalogCollapsed ? kCatGripW : kPaletteW;
    catalogViewport.setVisible(! catalogCollapsed);
    catalogCollapseBtn.setVisible(! catalogCollapsed);
    catalogExpandBtn  .setVisible(catalogCollapsed);

    if (catalogCollapsed)
    {
        // Full-width grip button, like ZONE 4's expand control.
        catalogExpandBtn.setBounds(2, zonesY + 4, kCatGripW - 4, 18);
    }
    else
    {
        const int btn = kCatHeaderH - 4;   // 18 — identical to ZONE 4's collapse ✕
        catalogCollapseBtn.setBounds(kPaletteW - btn - 2, zonesY + 2, btn, btn);

        const int catTop = zonesY + kCatHeaderH;
        const int catH   = juce::jmax(0, zonesH - kCatHeaderH);
        catalogViewport.setBounds(0, catTop, kPaletteW, catH);
        const int catW = juce::jmax(40, kPaletteW - catalogViewport.getScrollBarThickness());
        moduleCatalog.setSize(catW, juce::jmax(moduleCatalog.preferredHeight(), catH));
    }

    // ── Zone widths (clamped so zone 3 keeps at least kZone3MinW) ────────────
    const bool collapsed   = waterfallColumn->isCollapsed();
    const int  rightSplitW = collapsed ? 0 : kSplitterW;

    int z4w = VideoMixerColumn::kGripW;
    if (!collapsed)
    {
        const int z4Max = W - catRailW - kZone2MinW - kSplitterW - kZone3MinW - rightSplitW;
        zone4Width = juce::jlimit(kZone4MinW, juce::jmax(kZone4MinW, z4Max), zone4Width);
        z4w = zone4Width;
    }

    const int z2Max = W - catRailW - kSplitterW - kZone3MinW - rightSplitW - z4w;
    zone2Width = juce::jlimit(kZone2MinW, juce::jmax(kZone2MinW, z2Max), zone2Width);

    // ── Place the columns left → right (chain rack flush against the rail) ────
    int x = catRailW;

    rackViewport.setBounds(x, zonesY, zone2Width, zonesH);
    x += zone2Width;

    splitterLeft.setBounds(x, zonesY, kSplitterW, zonesH);
    x += kSplitterW;

    const int z3w = juce::jmax(50, W - x - rightSplitW - z4w);
    int z3y = zonesY;
    int z3h = zonesH;
    faceSwitch.setBounds(x, z3y, z3w, kFaceBarH);
    if (modulePowerButton.isVisible())   // power switch at the right end of the row
    {
        const int pw = 36;
        const int ph = kFaceBarH - 8;
        modulePowerButton.setBounds(x + z3w - pw - 8, z3y + (kFaceBarH - ph) / 2, pw, ph);
    }
    if (faceSwitch.isVisible())          // PLAY | SETUP bar shifts the viewport
    {
        z3y += kFaceBarH;
        z3h  = juce::jmax(0, z3h - kFaceBarH);
    }
    zone3Viewport.setBounds(x, z3y, z3w, z3h);
    x += z3w;

    splitterRight.setVisible(!collapsed);
    if (!collapsed)
    {
        splitterRight.setBounds(x, zonesY, kSplitterW, zonesH);
        x += kSplitterW;
    }

    waterfallColumn->setBounds(x, zonesY, z4w, zonesH);

    // ── Rack content sizing (viewport scrolls when the window is short) ──────
    const int rackW = juce::jmax(60, zone2Width - rackViewport.getScrollBarThickness());
    chainRack->setSize(rackW, juce::jmax(chainRack->preferredHeight(), zonesH));

    layoutZone3();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::layoutZone3()
{
    const int vpW = zone3Viewport.getWidth();
    const int vpH = zone3Viewport.getHeight();
    if (vpW <= 0 || vpH <= 0)
        return;

    const int cw = juce::jmax(120, vpW - zone3Viewport.getScrollBarThickness());

    // Resolve which page(s) the current selection + face hosts, with their
    // natural minimum heights (the viewport scrolls when the window is
    // shorter).
    juce::Component* top     = nullptr;
    juce::Component* bottom  = nullptr;
    int topMinH = 0, bottomH = 0;

    if (setupFace)
    {
        // ── SETUP face (M5): one settings panel per block ────────────────────
        switch (selectedBlock)
        {
            case ChainBlockId::Pitch:
                top = pitchSetup.get();   topMinH = PitchSetupPanel::kPreferredH;   break;
            case ChainBlockId::Mask:
                top = maskSetup.get();    topMinH = MaskSetupPanel::kPreferredH;    break;
            case ChainBlockId::Sampler:
                top = samplerSetup.get(); topMinH = SamplerSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxStral:
                top = stralSetup.get();   topMinH = LuxStralSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxSynth:
                top = synthSetup.get();   topMinH = LuxSynthSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxWave:
                top = waveSetup.get();    topMinH = LuxWaveSetupPanel::kPreferredH;  break;
            case ChainBlockId::Score:
                top = scoreSetup.get();   topMinH = ScoreSetupPanel::kPreferredH;    break;
            case ChainBlockId::Chain1Source:
            case ChainBlockId::Chain2Source:
                top = sourceSetup.get();  topMinH = SourceSetupPanel::kPreferredH;    break;
            case ChainBlockId::Sequencer:
            case ChainBlockId::VideoScroll:
                break;   // no SETUP face (blockHasSetup == false)
        }
    }
    else
    {
        switch (selectedBlock)
        {
            case ChainBlockId::Chain1Source:
            case ChainBlockId::Chain2Source:
                top = sourcesPage.get();     topMinH = 260; break;  // +acquisition-speed group
            case ChainBlockId::Pitch:
                top = pitchPage.get();       topMinH = 510; break;  // +100 env editor
            case ChainBlockId::Mask:
                top = maskPage.get();        topMinH = 570; break;  // +100 env editor
            case ChainBlockId::Sampler:
                top = samplerPage.get();     topMinH = 560; break;
            case ChainBlockId::Sequencer:
                top = sequencerPage.get();   topMinH = 360; break;  // grid + transport
            case ChainBlockId::LuxStral:
                top = imgLuxStralPage.get(); topMinH = LuxStralTabComponent::kPreferredH;
                break;
            case ChainBlockId::LuxSynth:
                top = imgLuxSynthPage.get(); topMinH = 400;
                bottom = audioSynthPanel.get(); bottomH = AudioSynthPanel::kPreferredH;
                break;
            case ChainBlockId::LuxWave:
                top = audioWavePanel.get();  topMinH = AudioWavePanel::kPreferredH; break;
            case ChainBlockId::Score:
                top = scorePage.get();       topMinH = 360; break;  // actions + transport only
            case ChainBlockId::VideoScroll:
                top = videoScrollPage.get(); topMinH = VideoScrollPage::kPreferredH; break;
        }
    }

    if (top == nullptr)
        return;

    int topH, contentH;
    if (bottom != nullptr)
    {
        topH     = juce::jmax(topMinH, vpH - bottomH - kStackGap);
        contentH = topH + kStackGap + bottomH;
    }
    else
    {
        topH     = juce::jmax(topMinH, vpH);
        contentH = topH;
    }

    zone3Content.setSize(cw, contentH);
    top->setBounds(0, 0, cw, topH);
    if (bottom != nullptr)
        bottom->setBounds(0, topH + kStackGap, cw, bottomH);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::persistLayoutProps()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    // Message-thread only: properties ride along with the APVTS session state
    // (getStateInformation serialises apvts.state including these).
    auto& state = audioProcessor.getAPVTS().state;
    state.setProperty("editorW", getWidth(),  nullptr);
    state.setProperty("editorH", getHeight(), nullptr);
    state.setProperty("zone2W",  zone2Width,  nullptr);
    state.setProperty("zone4W",  zone4Width,  nullptr);
    state.setProperty("scrollCollapsed",
                      waterfallColumn != nullptr && waterfallColumn->isCollapsed(),
                      nullptr);
    state.setProperty("catalogCollapsed", catalogCollapsed, nullptr);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::openSettings()
{
    if (!settingsWindow)
    {
        settingsWindow = std::make_unique<SettingsWindow>(audioProcessor);
        Sp3ctraUI::disableSliderScrollWheel(*settingsWindow);
    }
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
