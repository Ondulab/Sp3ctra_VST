#include "processing/synth_staging.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "video/VideoDisplaySettings.h"
#include "IconPaths.h"
#include "ui/ScrollWheelGuard.h"
#include "Sp3ctraVersion.h"
#include "Sp3ctraDialog.h"   // session-bar prompts (name input / confirm)
#include "ui/MidiCurveEditor.h"   // MIDI CURVE window (per-mapping transfer law)
#include "AppUpdater.h"
#include "UpdateDialog.h"
#include "licensing/ActivationDialog.h"
#include "session/MachinePrefs.h"   // machine-scoped settings (layout, LINK…)

//==============================================================================
Sp3ctraAudioProcessorEditor::Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    addAndMakeVisible(pipelineMetricsBar_);
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
    // A rack click on a synth block opens its OUT/send page (the engine page
    // is reached from the ZONE-5 dock card) — synth-split P2. Exception:
    // LUXSTRAL has no OUT page left, so selectBlock() coerces its tile onto
    // the engine view (same landing as its AUDIO MIX strip).
    chainRack->onBlockSelected = [this](ChainBlockId id)
    {
        engineView_ = false;
        selectBlock(id);
        persistLayoutProps();
    };
    // Selecting a VIDEO SCROLL output binds the contextual panel to that
    // instance's bank (fires just before onBlockSelected → selectBlock).
    chainRack->onVideoBlockSelected = [this](int slot)
    {
        videoSlotIndex_ = slot;
        videoAllView_   = false;   // an instance selection always lands on ITS chain tab
        if (videoScrollGridPage) videoScrollGridPage->setSelectedSlot(slot);
    };
    // Selecting a SAMPLER block binds the sampler page + setup to the engine
    // hosted by that pool slot (0..7), fired just before onBlockSelected →
    // selectBlock.
    chainRack->onSamplerBlockSelected = [this](int slot)
    {
        samplerEngineIndex_ = juce::jlimit(0, LuxSampler::kMaxEngines - 1, slot);
        if (samplerPage)  samplerPage ->setSamplerIndex(samplerEngineIndex_);
        if (samplerSetup) samplerSetup->setSamplerIndex(samplerEngineIndex_);
    };
    // Selecting a LUXSTRAL send tracks its slot (0..7) — it drives the OUT
    // page's conditioning bank and the per-send power button. The ENGINE
    // page/setup stay bound to the single LuxStral engine's parameter set.
    chainRack->onLuxStralBlockSelected = [this](int slot)
    {
        luxStralSendSlot_ = juce::jlimit(0, ChainModel::kMaxChains - 1, slot);
    };
    // A chain edit changes the rack's preferred height → re-run the zone layout.
    chainRack->onModelChanged  = [this]
    {
        // Refresh BEFORE layouting: the MIDI MIX section's very existence AND
        // its height are derived from its row count, so a stale refresh would
        // lay out one edit late (the panel would appear only on the NEXT edit).
        if (waterfallColumn) waterfallColumn->refreshActiveSlots();   // outputs added/removed
        if (midiMixPanel)    midiMixPanel   ->refreshActiveSlots();
        if (selectedBlock == ChainBlockId::VideoScroll)
            refreshVideoTabs();   // chain tabs + ALL view follow the outputs
        // A chain rename changes the zone-1 badge of the selected module
        // ("MASK - VOICE") — re-push it (selection-time push otherwise).
        if (cisVisualizer && chainRack)
        {
            int sc = -1, si = -1;
            if (const auto* m = audioProcessor.getChainModel().find(
                    chainRack->selectedInstanceId(), sc, si))
                cisVisualizer->setSelectedTapLabel(
                    moduleDisplayName(m->type).toUpperCase()
                    + " - " + audioProcessor.chainDisplayName(sc));
        }
        refreshChainBadge();   // the face-bar chain badge shows the name too
        layoutZones();
    };
    // State restore with the editor open (host preset change / project
    // reload): rebuild the rack from the NEW model — the audio follows the
    // restored topology immediately while the rack otherwise kept showing the
    // old blocks (ghost drops, stale LEDs) until the window was reopened.
    audioProcessor.onStateRestoredUi = [this]
    {
        if (chainRack)       chainRack->refreshFromModel();
        if (waterfallColumn) waterfallColumn->refreshActiveSlots();
        if (midiMixPanel)    midiMixPanel   ->refreshActiveSlots();
        // A host preset change can add/remove probes, so zone 4 must be
        // re-split too — this path never used to relayout at all.
        if (selectedBlock == ChainBlockId::VideoScroll)
            refreshVideoTabs();
        layoutZones();
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
    controlsPage    = std::make_unique<Sp3ctraControlsPage>(audioProcessor);
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
    zone3Content.addChildComponent(controlsPage.get());
    zone3Content.addChildComponent(pitchPage.get());
    zone3Content.addChildComponent(maskPage.get());
    zone3Content.addChildComponent(imgLuxStralPage.get());
    zone3Content.addChildComponent(imgLuxSynthPage.get());

    // Sampler page (reused as-is)
    samplerPage = std::make_unique<SamplerPageComponent>(audioProcessor);
    zone3Content.addChildComponent(samplerPage.get());

    // SEQUENCER — step sequencer extracted from the sampler page into its own
    // module page (grid + transport/config bar).

    // SCORE — offline printable-spectrogram export tool (no SETUP face)
    scorePage = std::make_unique<ScoreGenTabComponent>(audioProcessor);
    zone3Content.addChildComponent(scorePage.get());

    // TIMBRE — parametric instrument-spectrum generator (no SETUP face)
    timbrePage = std::make_unique<TimbreGenTabComponent>(audioProcessor);
    zone3Content.addChildComponent(timbrePage.get());

    // MIDI SCORE — MIDI-file → printable score generator (export prefs on
    // its SETUP face, created with the other setup panels below)
    midiScorePage = std::make_unique<MidiScoreGenTabComponent>(audioProcessor);
    zone3Content.addChildComponent(midiScorePage.get());
    voicePage = std::make_unique<VoiceGenTabComponent>(audioProcessor);
    zone3Content.addChildComponent(voicePage.get());

    // P7 — a score-family module left the rack: its player-pool slot is now up
    // for grabs by the next one placed ANYWHERE, so the four generator pages
    // must drop the document they kept for it. Without this, adding a SCORE to
    // chain 2 could inherit the take and settings of a SCORE deleted earlier.
    audioProcessor.onScoreSlotReleased =
        [this](int slot)
        {
            if (scorePage)     scorePage    ->forgetScoreSlot(slot);
            if (timbrePage)    timbrePage   ->forgetScoreSlot(slot);
            if (midiScorePage) midiScorePage->forgetScoreSlot(slot);
            if (voicePage)     voicePage    ->forgetScoreSlot(slot);
        };

    // ONE page for every VIDEO SCROLL output: a row each, a column per
    // setting. Clicking an output's name jumps to its chain tab through the
    // rack (block highlight + zone 1 follow), exactly as the ALL sections did.
    videoScrollGridPage = std::make_unique<VideoScrollGridPage>(audioProcessor);
    videoScrollGridPage->onSlotSelected = [this](int slot)
    {
        if (chainRack) chainRack->selectVideoSlot(slot);
    };
    zone3Content.addChildComponent(videoScrollGridPage.get());
    midiTapPage = std::make_unique<MidiTapPage>(audioProcessor);
    zone3Content.addChildComponent(midiTapPage.get());

    // FX — REVERB / ECHO / EQ / SCALE / CENTROID / LEVELS insert pages (all
    // controls on the PLAY face)
    reverbPage = std::make_unique<LuxReverbTabComponent>(audioProcessor);
    echoPage   = std::make_unique<LuxEchoTabComponent>(audioProcessor);
    eqPage     = std::make_unique<LuxEqTabComponent>(audioProcessor);
    harmoPage  = std::make_unique<LuxHarmoTabComponent>(audioProcessor);
    centroPage = std::make_unique<LuxCentroTabComponent>(audioProcessor);
    drivePage  = std::make_unique<LuxDriveTabComponent>(audioProcessor);
    dcBlockPage = std::make_unique<LuxDcBlockTabComponent>(audioProcessor);
    gainPage    = std::make_unique<LuxGainTabComponent>(audioProcessor);
    diffPage    = std::make_unique<LuxDiffTabComponent>(audioProcessor);
    zone3Content.addChildComponent(reverbPage.get());
    zone3Content.addChildComponent(echoPage.get());
    zone3Content.addChildComponent(eqPage.get());
    zone3Content.addChildComponent(harmoPage.get());
    zone3Content.addChildComponent(centroPage.get());
    zone3Content.addChildComponent(drivePage.get());
    zone3Content.addChildComponent(dcBlockPage.get());
    zone3Content.addChildComponent(gainPage.get());
    zone3Content.addChildComponent(diffPage.get());

    // M9 — IMAGE / VIDEO / CAMERA source pages (preview + movable line + transport)
    imageSrcPage  = std::make_unique<MediaSourcePage>(audioProcessor, MediaSourcePage::Kind::Image);
    videoSrcPage  = std::make_unique<MediaSourcePage>(audioProcessor, MediaSourcePage::Kind::Video);
    cameraSrcPage = std::make_unique<MediaSourcePage>(audioProcessor, MediaSourcePage::Kind::Camera);
    zone3Content.addChildComponent(imageSrcPage.get());
    zone3Content.addChildComponent(videoSrcPage.get());
    zone3Content.addChildComponent(cameraSrcPage.get());

    // Engine audio panel — the former SYNTH AUDIOWAVE sub-page, repackaged as
    // a component (same params & attachments).  AUDIOSTRAL and AUDIOSYNTH are
    // now part of their module pages (imgLuxStralPage / imgLuxSynthPage).
    audioWavePanel  = std::make_unique<AudioWavePanel>(audioProcessor);
    zone3Content.addChildComponent(audioWavePanel.get());
    luxGrainPanel   = std::make_unique<LuxGrainPanel>(audioProcessor);
    zone3Content.addChildComponent(luxGrainPanel.get());

    // OUT/send page (synth-split P2) — one instance, rebound per selection to
    // the selected send's conditioning bank (type + slot).
    synthOutPage = std::make_unique<SynthOutPageComponent>(audioProcessor);
    zone3Content.addChildComponent(synthOutPage.get());

    // SETUP faces (M5) — per-block settings migrated from the gear-wheel
    // window (same params & attachments), accent-matched to the chain rack.
    sourceSetup  = std::make_unique<SourceSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Chain1Source));
    pitchSetup   = std::make_unique<PitchSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Pitch));
    maskSetup    = std::make_unique<MaskSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Mask));
    centroSetup  = std::make_unique<CentroSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Centroid));
    stralSetup   = std::make_unique<LuxStralSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxStral));
    synthSetup   = std::make_unique<LuxSynthSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxSynth));
    waveSetup    = std::make_unique<LuxWaveSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxWave));
    grainSetup   = std::make_unique<LuxGrainSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::LuxGrain));
    samplerSetup = std::make_unique<SamplerSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Sampler));
    scoreSetup   = std::make_unique<ScoreSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::Score));
    // MIDI SCORE / TIMBRE export prefs live in each PLAY page's persisted
    // state — the panels edit the pages directly (created above).
    midiScoreSetup = std::make_unique<MidiScoreSetupPanel>(
        *midiScorePage, ChainRackComponent::blockColour(ChainBlockId::MidiScore));
    timbreSetup = std::make_unique<TimbreSetupPanel>(
        *timbrePage, ChainRackComponent::blockColour(ChainBlockId::Timbre));
    voiceSetup = std::make_unique<VoiceSetupPanel>(
        *voicePage, ChainRackComponent::blockColour(ChainBlockId::Voice));
    // M9 — media modules: source picking lives on the PLAY page now
    // (MediaSourcePage hosts LOAD/CLEAR/device combo); no SETUP face.
    zone3Content.addChildComponent(sourceSetup.get());
    zone3Content.addChildComponent(pitchSetup.get());
    zone3Content.addChildComponent(maskSetup.get());
    zone3Content.addChildComponent(centroSetup.get());
    zone3Content.addChildComponent(stralSetup.get());
    zone3Content.addChildComponent(synthSetup.get());
    zone3Content.addChildComponent(waveSetup.get());
    zone3Content.addChildComponent(grainSetup.get());
    zone3Content.addChildComponent(samplerSetup.get());
    zone3Content.addChildComponent(scoreSetup.get());
    zone3Content.addChildComponent(midiScoreSetup.get());
    zone3Content.addChildComponent(timbreSetup.get());
    zone3Content.addChildComponent(voiceSetup.get());

    // PLAY | SETUP face switcher (above the zone-3 viewport). Every block now
    // has a SETUP face — the SP3CTRA source hosts the network/CIS config there.
    faceSwitch.onFaceChanged = [this](bool setup)
    {
        setupFace = setup;
        applyZone3Visibility();
        layoutZone3();
        zone3Viewport.setViewPosition(0, 0);
        persistLayoutProps();   // face survives session reload
    };
    // VIDEO SCROLL chain tabs (custom-segment mode): ALL, then one segment per
    // patched output in rack order (same list as refreshVideoTabs()).
    faceSwitch.onSegmentSelected = [this](int idx)
    {
        // SP3CTRA source block: PLAY | CONTROLS | SETUP
        if (selectedBlock == ChainBlockId::Chain1Source || selectedBlock == ChainBlockId::Chain2Source)
        {
            sourceFace_ = juce::jlimit(0, 2, idx);
            setupFace   = (sourceFace_ == 2);
            applyZone3Visibility();
            layoutZone3();
            zone3Viewport.setViewPosition(0, 0);
            persistLayoutProps();
            return;
        }
        if (idx == 0) { showVideoAllView(); return; }
        const auto slots = audioProcessor.activeVideoSlots();
        if (idx - 1 < (int) slots.size() && chainRack)
            chainRack->selectVideoSlot(slots[(size_t) (idx - 1)].first);
        persistLayoutProps();
    };
    addChildComponent(faceSwitch);
    addChildComponent(modulePowerButton);
    // Engine-page header MUTE (replaces the on/off switch there): the engine's
    // enable shown inverted — muting an engine switches it OFF (anti-click
    // fade, then zero CPU), exactly what the AUDIO MIX strip's M does.
    moduleMuteButton.setClickingTogglesState(true);
    moduleMuteButton.setColour(juce::TextButton::buttonOnColourId,
                               juce::Colour(0xffe05548).withAlpha(0.9f));
    moduleMuteButton.setTooltip("Mute this engine (switches it off; its solo is ignored while muted)");
    moduleMuteButton.onClick = [this]
    {
        if (moduleMuteAttachment != nullptr)
            moduleMuteAttachment->setValueAsCompleteGesture(
                moduleMuteButton.getToggleState() ? 0.0f : 1.0f);
    };
    addChildComponent(moduleMuteButton);

    // ── AUDIO MIX (bottom of ZONE 4) — engines + MASTER, faders + VU (P2b) ────
    audioMixPanel = std::make_unique<AudioMixPanel>(audioProcessor);
    audioMixPanel->onEngineSelected = [this](ModuleType t)
    {
        engineView_ = true;
        selectBlock(t == ModuleType::LuxSynth ? ChainBlockId::LuxSynth
                  : t == ModuleType::LuxWave  ? ChainBlockId::LuxWave
                  : t == ModuleType::LuxGrain ? ChainBlockId::LuxGrain
                                              : ChainBlockId::LuxStral);
        persistLayoutProps();
    };
    addAndMakeVisible(audioMixPanel.get());

    // ── MIDI MIX (between VIDEO MIX and AUDIO MIX) — master of the MIDI TAP
    // probes. addChildComponent, NOT addAndMakeVisible: its visibility is owned
    // by layoutZones() (hidden while no probe is patched).
    midiMixPanel = std::make_unique<MidiMixPanel>(audioProcessor);
    midiMixPanel->onCollapseToggled = [this](bool) { layoutZones(); persistLayoutProps(); };
    midiMixPanel->onProbeSelected   = [this](int slot)
    {
        midiTapSlotIndex_ = slot;
        if (midiTapPage) midiTapPage->setSlot(slot);
        selectBlock(ChainBlockId::MidiTap);
        persistLayoutProps();
    };
    addChildComponent(midiMixPanel.get());

    // ── MIDI MAP (between MIDI MIX and AUDIO MIX) — the session's MIDI
    // mappings as an editable list (MIN/MAX range, remove, navigate). ─────────
    // ── ZONE 4 — LFO bank, above the MIDI MAP list that shows where it goes
    lfoPanel = std::make_unique<LfoPanel>(audioProcessor);
    lfoPanel->onCollapseToggled = [this](bool) { layoutZones(); persistLayoutProps(); };
    lfoPanel->onContentChanged  = [this] { layoutZones(); };
    addAndMakeVisible(lfoPanel.get());

    midiMapPanel = std::make_unique<MidiMapPanel>(audioProcessor);
    midiMapPanel->onCollapseToggled  = [this](bool) { layoutZones(); persistLayoutProps(); };
    midiMapPanel->onContentChanged   = [this] { layoutZones(); };
    midiMapPanel->onLfoSelected      = [this](int i)
    { if (lfoPanel) lfoPanel->selectLfo(i); };
    // A modulation was just posted from a control's menu: show its row, so the
    // two knobs that set how far the modulation reaches are seen at least once.
    audioProcessor.getMidiMap().onLfoMappingAdded = [this](const juce::String& id)
    {
        if (midiMapPanel) midiMapPanel->revealRow(id);
        // …and open the LFO that now drives it — a "New LFO…" is born here.
        int t = 0, c = 0, n = 0;
        if (lfoPanel && audioProcessor.getMidiMap().getMappingFor(id, t, c, n)
            && t == MidiMappingEngine::kTypeLfo)
            lfoPanel->selectLfo(n);
    };
    midiMapPanel->onMappingSelected  = [this](const juce::String& paramId)
    {
        // A row click ALWAYS lands on the control, whatever its F flag says,
        // and lights it (MidiTouch) so it is spotted at once on the page.
        followMidiParam(paramId);
        Sp3ctraControls::MidiTouch::note(paramId);
    };
    midiMapPanel->onEditRequested    = [this](const juce::String& paramId)
    { openMidiCurveEditor(paramId); };
    midiMapPanel->onCurveReset       = [this](const juce::String& paramId)
    {
        // The row's gear was HELD: its transfer law is neutral again. That
        // write does not broadcast, so a window already open on the same
        // mapping has to be told, or it would keep drawing the wiped law.
        if (midiCurveWindow_ != nullptr && midiCurveWindow_->paramId() == paramId)
            midiCurveWindow_->setTarget(paramId);
    };
    audioProcessor.getMidiMap().onEditRequested = [this](const juce::String& paramId)
    { openMidiCurveEditor(paramId); };   // "Edit MIDI mapping…" in any control's menu
    // How a parameter is NAMED in the right-click "Assign to CC…" list (what a
    // controller already drives). The engine knows ids; identities live here.
    audioProcessor.getMidiMap().describeTarget = [this](const juce::String& paramId)
    { return describeParam(audioProcessor, paramId).targetText(); };
    addAndMakeVisible(midiMapPanel.get());

    // ── ZONE 4: video scroll column (hosts the former VIDEO tab) ──────────────
    waterfallColumn = std::make_unique<VideoMixerColumn>(audioProcessor);
    waterfallColumn->onCollapseToggled = [this](bool)
    {
        layoutZones();
        persistLayoutProps();
    };
    // VIDEO MIX banner → the ALL tab; a "CHAIN n" strip row → that output's
    // chain tab (through the rack, so its block highlights and zone 1 follows).
    waterfallColumn->onHeaderClicked = [this] { showVideoAllView(); };
    waterfallColumn->onOutputClicked = [this](int slot)
    {
        if (chainRack) chainRack->selectVideoSlot(slot);
    };
    addAndMakeVisible(waterfallColumn.get());
    // The VIDEO SCROLL pages' VIEWPORT pad draws the mixer's composite as its
    // live thumbnail and mirrors the output view's aspect (polled by our timer).
    if (videoScrollGridPage) videoScrollGridPage->setPreviewSource(&waterfallColumn->mixer());

    // ── Splitters (zone2|zone3 and zone3|zone4) ───────────────────────────────
    // Drags anchor on the DISPLAYED width (zoneEff) so the handle tracks the
    // mouse even when the persisted intent is wider than the current window
    // allows; the drag then re-anchors the intent to what the user sees.
    splitterLeft.onDragStart = [this] { splitterDragStartW = zone2Eff_; };
    splitterLeft.onDragged   = [this](int dx)
    {
        zone2Width = splitterDragStartW + dx;   // clamped in layoutZones()
        layoutZones();
    };
    splitterLeft.onDragEnd   = [this] { persistLayoutProps(); };
    addAndMakeVisible(splitterLeft);

    splitterRight.onDragStart = [this] { splitterDragStartW = zone4Eff_; };
    splitterRight.onDragged   = [this](int dx)
    {
        zone4Width = splitterDragStartW - dx;   // clamped in layoutZones()
        layoutZones();
    };
    splitterRight.onDragEnd   = [this] { persistLayoutProps(); };
    addAndMakeVisible(splitterRight);

    // ── Header menu bar (right-aligned): SESSION · MIDI · ADVANCED · ABOUT ────
    // SESSION is Standalone-only (in a DAW the host project IS the session).
    if (auto* sessions = audioProcessor.sessions();
        sessions != nullptr && sessions->isStandalone())
    {
        menuSessionBtn_.setTooltip(
            "Working session: everything auto-saves into the session folder.");
        menuSessionBtn_.onClick = [this] { showSessionMenu(); };
        addAndMakeVisible(menuSessionBtn_);

        // Session switches refresh the menu label (name + saved dot).
        sessions->onSessionChanged =
            [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        {
            if (auto* self = safe.getComponent())
                self->refreshSessionBar();
        };
        refreshSessionBar();
    }

    menuMidiBtn_.setTooltip("MIDI: follow control, mappings, panic.");
    menuMidiBtn_.onClick = [this] { showMidiMenu(); };
    addAndMakeVisible(menuMidiBtn_);

    menuAdvancedBtn_.setTooltip("Advanced: video display FPS, log level, worker threads.");
    menuAdvancedBtn_.onClick = [this] { showAdvancedMenu(); };
    addAndMakeVisible(menuAdvancedBtn_);

    menuAboutBtn_.setTooltip("About Sp3ctra, software update, license.");
    menuAboutBtn_.onClick = [this] { showAboutMenu(); };
    addAndMakeVisible(menuAboutBtn_);

    // In-app update: the ABOUT dot lights up when a new build is available.
    // The startup check itself runs once per process, standalone only (a DAW
    // plugin must not fire network requests just because it was loaded).
    AppUpdater::getInstance()->addChangeListener(this);
    refreshUpdateBadge();
    if (auto* sessions = audioProcessor.sessions();
        sessions != nullptr && sessions->isStandalone())
        AppUpdater::getInstance()->startupCheck();

    // License: silent weekly revalidation (same standalone-only network policy
    // as the update check) + the once-per-process demo reminder, delayed so it
    // appears over a settled UI.
    if (auto* sessions = audioProcessor.sessions();
        sessions != nullptr && sessions->isStandalone())
        LicenseManager::getInstance()->startupValidate();
    // The nag flag is consumed INSIDE the lambda: if this editor is torn down
    // before the timer fires (session-restore rebuild), the next editor's
    // timer still shows the reminder instead of losing it to a dead pointer.
    if (! LicenseManager::isLicensed())
        juce::Timer::callAfterDelay(1500,
            [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
            {
                if (safe != nullptr && ! LicenseManager::isLicensed()
                    && LicenseManager::getInstance()->shouldShowStartupNag())
                    ActivationDialog::show(safe.getComponent());
            });

    // ── Restore persisted layout (SESSION-scoped — survives session reload) ───
    // The session property wins; the transient "layout.*" machine keys (a
    // short-lived scoping experiment) are only read when the session has
    // none, so nothing saved during that window is lost.
    auto& state = apvts.state;
    auto& prefs = MachinePrefs::file();
    auto layoutInt = [&](const char* legacy, const char* prefKey, int fallback)
    {
        if (state.hasProperty(legacy))
            return (int) state.getProperty(legacy, fallback);
        return prefs.containsKey(prefKey) ? prefs.getIntValue(prefKey, fallback)
                                          : fallback;
    };
    auto layoutBool = [&](const char* legacy, const char* prefKey)
    {
        if (state.hasProperty(legacy))
            return (bool) state.getProperty(legacy, false);
        return prefs.containsKey(prefKey) && prefs.getBoolValue(prefKey, false);
    };
    zone2Width = layoutInt("zone2W", "layout.zone2W", kZone2DefaultW);
    zone4Width = layoutInt("zone4W", "layout.zone4W", kZone4DefaultW);
    if (layoutBool("scrollCollapsed", "layout.scrollCollapsed"))
        waterfallColumn->setCollapsed(true, false);
    if (midiMixPanel != nullptr
        && layoutBool("midiMixCollapsed", "layout.midiMixCollapsed"))
        midiMixPanel->setCollapsed(true, false);
    if (midiMapPanel != nullptr
        && layoutBool("midiMapCollapsed", "layout.midiMapCollapsed"))
        midiMapPanel->setCollapsed(true, false);
    if (lfoPanel != nullptr && layoutBool("lfoCollapsed", "layout.lfoCollapsed"))
        lfoPanel->setCollapsed(true, false);
    // Re-open the persisted real-time MIDI destination (machine-scoped, by
    // name; a vanished device silently degrades to "no port").
    if (const auto dest = prefs.containsKey("midiTapDest")
                              ? prefs.getValue("midiTapDest")
                              : state.getProperty("midiTapDest", juce::var()).toString();
        dest.isNotEmpty())
        audioProcessor.setMidiTapDestination(dest);
    if (layoutBool("catalogCollapsed", "layout.catalogCollapsed"))
        setCatalogCollapsed(true, false);   // also locks the chain rack

    // ── Restore the zone-3 selection (block + face + engine bindings) ─────────
    // Bindings first, so the restored selection lands on the same engine /
    // video instance the user was editing (rack clicks set these callbacks-
    // first for the same reason).
    luxStralSendSlot_ = juce::jlimit(0, ChainModel::kMaxChains - 1,
        (int) state.getProperty("selLuxStralSend", 0));
    samplerEngineIndex_  = juce::jlimit(0, LuxSampler::kMaxEngines - 1,
        (int) state.getProperty("selSamplerEngine", 0));
    videoSlotIndex_      = juce::jlimit(0, ChainModel::kMaxVideoSlots - 1,
        (int) state.getProperty("selVideoSlot", 0));
    videoAllView_        = (bool) state.getProperty("selVideoAll", false);
    if (samplerPage)     samplerPage    ->setSamplerIndex(samplerEngineIndex_);
    if (samplerSetup)    samplerSetup   ->setSamplerIndex(samplerEngineIndex_);
    // Selected bank inside the sampler page (persisted alongside the engine).
    {
        const int bank = juce::jlimit(0, LuxSamplerConstants::NUM_SLOTS - 1,
            (int) state.getProperty("selSamplerBank",
                                    audioProcessor.getSamplerSelectedSlot()));
        audioProcessor.setSamplerSelectedSlot(bank);
        if (samplerPage) samplerPage->selectSlot(bank);
    }
    if (videoScrollGridPage) videoScrollGridPage->setSelectedSlot(videoSlotIndex_);
    midiTapSlotIndex_ = juce::jlimit(0, ChainModel::kMaxMidiTaps - 1,
        (int) state.getProperty("selMidiTapSlot", 0));
    if (midiTapPage) midiTapPage->setSlot(midiTapSlotIndex_);

    // Selected block: fall back to the default when out of range or when its
    // module was deleted since the save (the rack can't highlight a ghost).
    // An entirely empty rack restores as "no selection" (zone 1/3 blank).
    auto sel = chainRack->firstBlockId();
    {
        const int raw = (int) state.getProperty("selBlock", (int) sel);
        if (raw >= (int) ChainBlockId::Chain1Source
            && raw < (int) ChainBlockId::None
            && chainRack->hasBlock((ChainBlockId) raw))
            sel = (ChainBlockId) raw;
    }
    // Pre-seed selectedBlock so selectBlock() keeps the restored face (it
    // resets to PLAY on a block CHANGE); blockHasSetup is re-checked inside.
    setupFace     = (bool) state.getProperty("selSetupFace", false);
    sourceFace_   = juce::jlimit(0, 2, (int) state.getProperty("selSourceFace", 0));
    engineView_   = (bool) state.getProperty("selEngineView", false);
    selectedBlock = sel;
    selectBlock(sel);

    juce::LookAndFeel::setDefaultLookAndFeel(&sp3ctraLaf);

    // ── Resizable editor + persisted size ─────────────────────────────────────
    // Read the persisted size BEFORE setResizeLimits: that call snaps the
    // still-0×0 editor to the MINIMUM size, whose resized() used to run
    // persistLayoutProps() and overwrite the saved editorW/H in the state —
    // the read right after then only ever saw kMinW×kMinH. This is why every
    // session reopened at the minimum window size. persistLayoutProps() is
    // additionally gated on layoutRestoreDone_ so no constructor-time layout
    // pass can stomp restored values again.
    const int w = juce::jlimit(kMinW, kMaxW,
        layoutInt("editorW", "layout.editorW", kDefaultW));
    const int h = juce::jlimit(kMinH, kMaxH,
        layoutInt("editorH", "layout.editorH", kDefaultH));
    setResizable(true, true);
    setResizeLimits(kMinW, kMinH, kMaxW, kMaxH);
    layoutRestoreDone_ = true;   // from here, resized() persists for real
    setSize(w, h);

    // Scrolling a panel should never nudge the knob/slider under the cursor:
    // disable wheel-driven value changes on every Slider in the editor tree.
    Sp3ctraUI::disableSliderScrollWheel(*this);

    // ── MIDI-follow poll ──────────────────────────────────────────────────────
    // Sync the baseline so a controller move that happened while the editor was
    // closed doesn't jump on open, then poll the mapping engine for the next
    // touched parameter (auto-navigate when the setting is on).
    audioProcessor.getMidiMap().resetTouchBaseline();
    startTimerHz(20);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    stopTimer();
    audioProcessor.getMidiMap().onEditRequested = nullptr;   // captured `this`
    audioProcessor.getMidiMap().describeTarget  = nullptr;   // captured `this`
    midiCurveWindow_.reset();   // before the default LookAndFeel goes away
    // The mixer (zone 4) dies before the VIDEO SCROLL pages (member order):
    // detach their preview source first.
    if (videoScrollGridPage) videoScrollGridPage->setPreviewSource(nullptr);
    if (auto* up = AppUpdater::getInstanceWithoutCreating())
        up->removeChangeListener(this);
    audioProcessor.onStateRestoredUi   = nullptr; // this editor is going away
    audioProcessor.onScoreSlotReleased = nullptr; // (its pages die with it)
    if (auto* s = audioProcessor.sessions())
        s->onSessionChanged = nullptr;            // ditto for the session bar
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

//==============================================================================
// Header menu bar (right-aligned): SESSION · MIDI · ADVANCED · ABOUT
//==============================================================================
void Sp3ctraAudioProcessorEditor::layoutHeaderMenus()
{
    const int bh = kTitleRowH - 16;
    const int by = (kTitleRowH - bh) / 2;
    int x = getWidth() - 10;
    auto place = [&](HeaderMenuButton& b)
    {
        if (! b.isVisible()) return;
        const int w = b.idealWidth();
        x -= w;
        b.setBounds(x, by, w, bh);
        x -= 4;
    };
    // Right-to-left so the visual order is SESSION · MIDI · ADVANCED · ABOUT.
    place(menuAboutBtn_);
    place(menuAdvancedBtn_);
    place(menuMidiBtn_);
    place(menuSessionBtn_);
}

void Sp3ctraAudioProcessorEditor::refreshSessionBar()
{
    auto* s = audioProcessor.sessions();
    if (s == nullptr || ! s->isStandalone())
        return;
    // The SESSION menu button IS the status display: name + saved/unsaved dot.
    menuSessionBtn_.setLabel("SESSION : " + s->sessionName());
    menuSessionBtn_.setDot(true, s->hasUnsavedChanges()
                                     ? juce::Colour(0xffe0a030)    // autosave pending
                                     : juce::Colour(0xff3fae5a));  // saved
    shownSessionLabel_ = s->sessionName()
                       + (s->hasUnsavedChanges() ? "*" : "");
    layoutHeaderMenus();   // width follows the label
}

void Sp3ctraAudioProcessorEditor::showSessionMenu()
{
    auto* s = audioProcessor.sessions();
    if (s == nullptr) return;

    juce::PopupMenu m;
    m.addSectionHeader("SESSION : " + s->sessionName());
    m.addItem(1, juce::String::fromUTF8("New session…"));
    m.addItem(2, juce::String::fromUTF8("Open session…"));
    m.addItem(3, juce::String::fromUTF8("Save session as…"));
    m.addItem(4, "Reveal session folder");
    m.addSeparator();
    m.addItem(5, juce::String::fromUTF8("Close session (back to Global)"),
              ! s->isGlobal());

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&menuSessionBtn_),
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (int choice)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0) return;
            auto* mgr = self->audioProcessor.sessions();
            switch (choice)
            {
                case 1: self->runSessionCreateFlow(false); break;
                case 2:
                {
                    self->sessionChooser = std::make_unique<juce::FileChooser>(
                        "Open a session folder",
                        mgr->startDirFor(PathKeys::sessionParent,
                                         SessionManager::appSupportRoot()
                                             .getChildFile("Sessions")));
                    self->sessionChooser->launchAsync(
                        juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
                        [safe](const juce::FileChooser& fc)
                        {
                            const auto dir = fc.getResult();
                            auto* s2 = safe.getComponent();
                            if (s2 == nullptr || ! dir.isDirectory()) return;
                            auto* m2 = s2->audioProcessor.sessions();
                            m2->rememberDirFor(PathKeys::sessionParent,
                                               dir.getParentDirectory());
                            if (! m2->openSession(dir))
                                Sp3ctraDialog::showWarning(
                                    s2, "Open session",
                                    "This folder is not a Sp3ctra session "
                                    "(no project.sp3ctra file found).");
                        });
                    break;
                }
                case 3: self->runSessionCreateFlow(true); break;
                case 4: mgr->sessionDir().revealToUser(); break;
                case 5:
                    if (! mgr->isGlobal())
                        Sp3ctraDialog::showConfirm(
                            self, "Close session",
                            "Return to the Global session?\n"
                            "(Everything is already saved in the session folder.)",
                            "Close", "Cancel",
                            [safe](bool ok)
                            {
                                if (! ok) return;
                                if (auto* s3 = safe.getComponent())
                                    s3->audioProcessor.sessions()->closeSession();
                            });
                    break;
            }
        });
}

void Sp3ctraAudioProcessorEditor::showMidiMenu()
{
    const bool follow      = midiFollowEnabled();
    const int  numMappings = audioProcessor.getMidiMap().numMappings();

    juce::PopupMenu m;
    m.addSectionHeader("MIDI");
    // Label mirrors the state (ON ⇄ OFF) — selecting it toggles.
    m.addItem(1, juce::String("Follow control : ") + (follow ? "ON" : "OFF"),
              true, follow);
    m.addSeparator();
    m.addItem(4, juce::String::fromUTF8("Import MIDI mappings…"));
    m.addItem(5, juce::String::fromUTF8("Export MIDI mappings…")
                 + (numMappings > 0 ? " (" + juce::String(numMappings) + ")"
                                    : juce::String()),
              numMappings > 0);
    m.addItem(2, juce::String::fromUTF8("Clear all MIDI mappings…"),
              numMappings > 0);
    m.addSeparator();
    m.addItem(3, "PANIC (all notes off)");

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&menuMidiBtn_),
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (int choice)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0) return;
            switch (choice)
            {
                case 1:   // toggle midiFollowParam
                    if (auto* p = self->audioProcessor.getAPVTS()
                                      .getParameter("midiFollowParam"))
                    {
                        p->setValueNotifyingHost(
                            p->getValue() >= 0.5f ? 0.0f : 1.0f);
                        MachinePrefs::saveParam(self->audioProcessor.getAPVTS(),
                                                "midiFollowParam");
                    }
                    break;
                case 2:   // destructive → confirm
                    Sp3ctraDialog::showConfirm(
                        self, "Clear MIDI mappings",
                        "Remove ALL MIDI CC/Note assignments?\n"
                        "This cannot be undone.",
                        "Clear all", "Cancel",
                        [safe](bool ok)
                        {
                            if (! ok) return;
                            if (auto* s2 = safe.getComponent())
                                s2->audioProcessor.getMidiMap().clearAll();
                        });
                    break;
                case 3:   // release every held/stuck note next audio block
                    self->audioProcessor.requestAllNotesOff();
                    break;
                case 4: self->importMidiMappingsFlow(); break;
                case 5: self->exportMidiMappingsFlow(); break;
            }
        });
}

//==============================================================================
// MIDI mappings ↔ .sp3midi files. Reusable assets across sessions (like the
// .sp3chain presets): the chooser remembers its own directory, it never
// defaults into the session folder.
//==============================================================================
void Sp3ctraAudioProcessorEditor::exportMidiMappingsFlow()
{
    if (LicenseGate::blockIfDemo(this, "Export MIDI mappings"))
        return;
    auto* s = audioProcessor.sessions();
    const auto dir = s->startDirFor(
        PathKeys::midiMap,
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("Sp3ctra MIDI Mappings"));
    dir.createDirectory();

    // Suggested name carries the session so exports stay identifiable.
    const juce::String base = s->isStandalone()
        ? s->sessionName() + " mappings" : juce::String("Sp3ctra mappings");

    sessionChooser = std::make_unique<juce::FileChooser>(
        "Export MIDI mappings", dir.getChildFile(base + ".sp3midi"), "*.sp3midi");
    sessionChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (const juce::FileChooser& fc)
        {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            auto file = fc.getResult();
            if (file == juce::File{}) return;
            file = file.withFileExtension("sp3midi");
            self->audioProcessor.sessions()->rememberDirFor(PathKeys::midiMap, file);

            const auto tree = self->audioProcessor.getMidiMap().toValueTree();
            const auto xml  = tree.createXml();
            if (xml == nullptr || ! xml->writeTo(file))
                Sp3ctraDialog::showWarning(
                    self, "Export MIDI mappings",
                    "Could not write\n" + file.getFullPathName());
        });
}

void Sp3ctraAudioProcessorEditor::importMidiMappingsFlow()
{
    auto* s = audioProcessor.sessions();
    sessionChooser = std::make_unique<juce::FileChooser>(
        "Import MIDI mappings",
        s->startDirFor(
            PathKeys::midiMap,
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("Sp3ctra MIDI Mappings")),
        "*.sp3midi");
    sessionChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (const juce::FileChooser& fc)
        {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            const auto file = fc.getResult();
            if (file == juce::File{}) return;
            self->audioProcessor.sessions()->rememberDirFor(PathKeys::midiMap, file);

            const auto xml = juce::XmlDocument::parse(file);
            const auto tree = xml != nullptr ? juce::ValueTree::fromXml(*xml)
                                             : juce::ValueTree();
            if (! tree.isValid() || ! tree.hasType("MIDI_MAPPINGS"))
            {
                Sp3ctraDialog::showWarning(
                    self, "Import MIDI mappings",
                    file.getFileName()
                     + " is not a valid Sp3ctra MIDI mappings file.");
                return;
            }

            auto apply = [safe, tree]
            {
                if (auto* s2 = safe.getComponent())
                    // Replace semantics: clears the table then re-adds each MAP
                    // (assignments for absent modules are silently dropped).
                    s2->audioProcessor.getMidiMap().restoreFromValueTree(tree);
            };

            // Importing REPLACES the current table — confirm when non-empty.
            const int current = self->audioProcessor.getMidiMap().numMappings();
            if (current > 0)
                Sp3ctraDialog::showConfirm(
                    self, "Import MIDI mappings",
                    "Importing replaces your current " + juce::String(current)
                     + " assignment(s).",
                    "Import", "Cancel",
                    [apply](bool ok) { if (ok) apply(); });
            else
                apply();
        });
}

void Sp3ctraAudioProcessorEditor::showAdvancedMenu()
{
    auto& apvts = audioProcessor.getAPVTS();

    // Current values (denormalised) for the check-marks.
    const int curLog = (int) std::round(
        apvts.getRawParameterValue("logLevel")->load());
    const int curWorkers = (int) std::round(
        apvts.getRawParameterValue("luxstralNumWorkers")->load());

    juce::PopupMenu logMenu;
    static const char* kLogNames[] = { "Error", "Warning", "Info", "Debug" };
    for (int i = 0; i < 4; ++i)
        logMenu.addItem(100 + i, kLogNames[i], true, curLog == i);

    juce::PopupMenu workersMenu;
    for (const int n : { 1, 2, 4, 6, 8, 12, 16 })
        workersMenu.addItem(200 + n, juce::String(n), true, curWorkers == n);

    juce::PopupMenu fpsMenu;
    for(int fps:VideoDisplaySettings::kFpsChoices)
        fpsMenu.addItem(400+fps,juce::String(fps)+" FPS",true,VideoDisplaySettings::fps()==fps);
    fpsMenu.addSeparator();
    fpsMenu.addItem(499,"Display only - chain/audio rates unchanged",false);
    fpsMenu.addItem(498,"Recording keeps its render cadence",false);

    juce::PopupMenu m;
    m.addSectionHeader("ADVANCED");
    m.addSubMenu("Video display FPS",fpsMenu);
    m.addSubMenu("Log level",      logMenu);
    m.addSubMenu("Worker threads", workersMenu);
    m.addSeparator();
    // P9 — machine-scoped opt-in: re-arm the transports a session saved
    // running when it reopens (never-auto-run stays the default).
    m.addItem(300, "Resume playback on session load", true,
              MachinePrefs::resumePlaybackOnLoad());

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&menuAdvancedBtn_),
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (int choice)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0) return;
            if(choice>=410 && choice<=460)
            {
                VideoDisplaySettings::setFps(choice-400);
                return;
            }
            if (choice == 300)
            {
                MachinePrefs::file().setValue(
                    MachinePrefs::kResumePlaybackKey,
                    ! MachinePrefs::resumePlaybackOnLoad());
                return;
            }
            auto& ap = self->audioProcessor.getAPVTS();
            auto setDenorm = [&ap](const char* id, float denorm)
            {
                if (auto* p = ap.getParameter(id))
                    p->setValueNotifyingHost(p->convertTo0to1(denorm));
            };
            if (choice >= 100 && choice < 200)
                setDenorm("logLevel", (float) (choice - 100));
            else if (choice >= 200 && choice < 300)
                setDenorm("luxstralNumWorkers", (float) (choice - 200));
            // Diagnostics are machine-scoped — persist outside the session.
            MachinePrefs::saveParam(ap, "logLevel");
            MachinePrefs::saveParam(ap, "luxstralNumWorkers");
        });
}

void Sp3ctraAudioProcessorEditor::showAboutMenu()
{
    // The update entry replaces the old "Downloads" web link: label follows
    // the AppUpdater state so a startup-detected update is one click away.
    juce::String updateLabel = juce::String::fromUTF8("Check for updates…");
    switch (AppUpdater::getInstance()->state())
    {
        case AppUpdater::State::updateAvailable:
            updateLabel = juce::String::fromUTF8("Update to v")
                        + AppUpdater::getInstance()->latestVersion()
                        + juce::String::fromUTF8("…");
            break;
        case AppUpdater::State::readyToRestart:
            updateLabel = juce::String::fromUTF8("Restart to finish update…");
            break;
        default: break;
    }

    juce::PopupMenu m;
    m.addSectionHeader("Sp3ctra v" SP3CTRA_VERSION_STRING);
    m.addItem(1, juce::String::fromUTF8("About Sp3ctra…"));
    m.addItem(8, LicenseManager::isLicensed()
                     ? juce::String::fromUTF8("License — Studio mode…")
                     : juce::String::fromUTF8("Activate Studio mode…"));
    m.addSeparator();
    m.addItem(2, juce::String::fromUTF8("Website — ondulab.com"));
    m.addItem(3, updateLabel);
    // Donate (id 4) retiré tant que le don Ondulab n'est pas actif : le lien
    // paypal.me aboutissait à un montant nul. Voir OndulabLinks::kDonateUrl.
    m.addSeparator();
    m.addItem(5, juce::String::fromUTF8("Report an issue — GitHub"));
    m.addItem(7, juce::String::fromUTF8("Contact — contact@ondulab.com"));
    m.addItem(6, "License (GNU GPL v3)");

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&menuAboutBtn_),
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (int choice)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0) return;
            auto open = [](const juce::String& url)
            { juce::URL(url).launchInDefaultBrowser(); };
            switch (choice)
            {
                case 1: AboutDialog::show(self); break;
                case 2: open(OndulabLinks::kWebsiteUrl);   break;
                case 3: UpdateDialog::show(self); break;
                // case 4 : Donate — désactivé (voir addItem plus haut).
                case 5: open(OndulabLinks::kIssuesUrl);    break;
                case 6: open(OndulabLinks::kLicenseUrl);   break;
                case 7: open(OndulabLinks::contactUrl());  break;
                case 8: ActivationDialog::show(self);      break;
            }
        });
}

void Sp3ctraAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == AppUpdater::getInstanceWithoutCreating())
        refreshUpdateBadge();
}

void Sp3ctraAudioProcessorEditor::refreshUpdateBadge()
{
    const auto st = AppUpdater::getInstance()->state();
    const bool pending = st == AppUpdater::State::updateAvailable
                      || st == AppUpdater::State::readyToRestart;
    menuAboutBtn_.setDot(pending, juce::Colour(0xff7aade0));
    layoutHeaderMenus();   // dot changes the button's ideal width
}

void Sp3ctraAudioProcessorEditor::runSessionCreateFlow(bool saveAs)
{
    if (LicenseGate::blockIfDemo(this, saveAs ? "Save session as" : "New session"))
        return;
    auto* s = audioProcessor.sessions();
    if (s == nullptr) return;

    // 1) Pick the PARENT folder the session directory will be created in…
    sessionChooser = std::make_unique<juce::FileChooser>(
        saveAs ? "Choose where to save the session copy"
               : "Choose where to create the new session",
        s->startDirFor(PathKeys::sessionParent,
                       SessionManager::appSupportRoot().getChildFile("Sessions")));
    sessionChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [this, saveAs](const juce::FileChooser& fc)
        {
            const auto parent = fc.getResult();
            if (! parent.isDirectory()) return;
            auto* mgr = audioProcessor.sessions();
            mgr->rememberDirFor(PathKeys::sessionParent, parent);

            // 2) …then name it. The session starts from the CURRENT state.
            const juce::String defaultName =
                saveAs ? (mgr->sessionName() + " copy")
                       : ("Session "
                          + juce::Time::getCurrentTime().formatted("%Y-%m-%d"));
            Sp3ctraDialog::showInput(
                this,
                saveAs ? "Save session as" : "New session",
                "Session name:",
                defaultName,
                "Create", "Cancel",
                [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this),
                 parent, saveAs](const juce::String& name)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr || name.trim().isEmpty()) return;
                    auto* m = self->audioProcessor.sessions();
                    const bool ok = saveAs ? m->saveAs(parent, name.trim())
                                           : m->newSession(parent, name.trim());
                    if (! ok)
                        Sp3ctraDialog::showWarning(
                            self, saveAs ? "Save session as" : "New session",
                            "Could not create the session folder "
                            "(name already used, or the location is not writable).");
                });
        });
}

//==============================================================================
bool Sp3ctraAudioProcessorEditor::blockHasSetup(ChainBlockId id) noexcept
{
    // Every block has a SETUP face — the SP3CTRA source hosts the network/CIS
    // configuration there (formerly the gear-wheel Network tab); MIDI SCORE hosts
    // its export prefs (PNG/JPEG, A4/A3/FULL, DPI); CENTROID hosts its width
    // law (PX / ERB) — EXCEPT the
    // REVERB / ECHO / EQ FX inserts (single PLAY page), the
    // IMAGE / VIDEO / CAMERA media modules (source picking lives on PLAY), and
    // MIDI TAP (the MIDI MIX master strip owns every settings-shaped control:
    // timebase, destination, file — the probe only owns "what is a note"), and
    // VIDEO SCROLL (its face bar hosts the chain tabs ALL | CHAIN n instead;
    // the frame colour moved onto the page — 2026-08-28).
    return id != ChainBlockId::RetiredSequencer
        && id != ChainBlockId::VideoScroll
        && id != ChainBlockId::Reverb    && id != ChainBlockId::Echo
        && id != ChainBlockId::Equalizer && id != ChainBlockId::Harmonize
        && id != ChainBlockId::Drive
        && id != ChainBlockId::DcBlock   && id != ChainBlockId::Gain
        && id != ChainBlockId::Diff
        && id != ChainBlockId::MidiTap
        && id != ChainBlockId::None
        && id != ChainBlockId::ImageSrc  && id != ChainBlockId::VideoSrc
        && id != ChainBlockId::CameraSrc;
}

//==============================================================================
// Synth-split P2 — the three synth blocks host two zone-3 views: the OUT/send
// page (rack click) and the engine page (dock click).
static bool isSynthBlock(ChainBlockId id) noexcept
{
    return id == ChainBlockId::LuxStral || id == ChainBlockId::LuxSynth
        || id == ChainBlockId::LuxWave  || id == ChainBlockId::LuxGrain;
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::applyZone3Visibility()
{
    const auto id   = selectedBlock;
    const bool play = !setupFace;

    // ── PLAY face: the page (or stacked pages) for this block ────────────────
    const bool isSource    = (id == ChainBlockId::Chain1Source || id == ChainBlockId::Chain2Source);
    const bool showSources = play && isSource && sourceFace_ != 1;
    if (sourcesPage)     sourcesPage    ->setVisible(showSources);
    if (controlsPage)    controlsPage   ->setVisible(play && isSource && sourceFace_ == 1);
    if (pitchPage)       pitchPage      ->setVisible(play && id == ChainBlockId::Pitch);
    if (maskPage)        maskPage       ->setVisible(play && id == ChainBlockId::Mask);
    if (samplerPage)     samplerPage    ->setVisible(play && id == ChainBlockId::Sampler);
    // Synth blocks (P2): engine pages only in ENGINE view (dock); the rack
    // click shows the OUT/send page instead — except LUXSTRAL, whose
    // selection is always coerced to the engine view (empty OUT page).
    if (imgLuxStralPage) imgLuxStralPage->setVisible(play && engineView_ && id == ChainBlockId::LuxStral);
    if (imgLuxSynthPage) imgLuxSynthPage->setVisible(play && engineView_ && id == ChainBlockId::LuxSynth);
    if (audioWavePanel)  audioWavePanel ->setVisible(play && engineView_ && id == ChainBlockId::LuxWave);
    if (luxGrainPanel)   luxGrainPanel  ->setVisible(play && engineView_ && id == ChainBlockId::LuxGrain);
    if (synthOutPage)    synthOutPage   ->setVisible(play && !engineView_ && isSynthBlock(id));
    if (scorePage)       scorePage      ->setVisible(play && id == ChainBlockId::Score);
    if (timbrePage)      timbrePage     ->setVisible(play && id == ChainBlockId::Timbre);
    if (midiScorePage)   midiScorePage  ->setVisible(play && id == ChainBlockId::MidiScore);
    if (voicePage)       voicePage      ->setVisible(play && id == ChainBlockId::Voice);
    if (reverbPage)      reverbPage     ->setVisible(play && id == ChainBlockId::Reverb);
    if (echoPage)        echoPage       ->setVisible(play && id == ChainBlockId::Echo);
    if (eqPage)          eqPage         ->setVisible(play && id == ChainBlockId::Equalizer);
    if (harmoPage)       harmoPage      ->setVisible(play && id == ChainBlockId::Harmonize);
    if (centroPage)      centroPage     ->setVisible(play && id == ChainBlockId::Centroid);
    if (drivePage)       drivePage      ->setVisible(play && id == ChainBlockId::Drive);
    if (dcBlockPage)     dcBlockPage    ->setVisible(play && id == ChainBlockId::DcBlock);
    if (gainPage)        gainPage       ->setVisible(play && id == ChainBlockId::Gain);
    if (diffPage)        diffPage       ->setVisible(play && id == ChainBlockId::Diff);
    // ALL and CHAIN n are the SAME page now — the tab only moves the pad.
    if (videoScrollGridPage) videoScrollGridPage->setVisible(play && id == ChainBlockId::VideoScroll);
    if (midiTapPage)     midiTapPage    ->setVisible(play && id == ChainBlockId::MidiTap);
    if (imageSrcPage)    imageSrcPage   ->setVisible(play && id == ChainBlockId::ImageSrc);
    if (videoSrcPage)    videoSrcPage   ->setVisible(play && id == ChainBlockId::VideoSrc);
    if (cameraSrcPage)   cameraSrcPage  ->setVisible(play && id == ChainBlockId::CameraSrc);

    // ── SETUP face: the per-block settings panel ──────────────────────────────
    if (sourceSetup)  sourceSetup ->setVisible(setupFace && (id == ChainBlockId::Chain1Source
                                                          || id == ChainBlockId::Chain2Source));
    if (pitchSetup)   pitchSetup  ->setVisible(setupFace && id == ChainBlockId::Pitch);
    if (maskSetup)    maskSetup   ->setVisible(setupFace && id == ChainBlockId::Mask);
    if (centroSetup)  centroSetup ->setVisible(setupFace && id == ChainBlockId::Centroid);
    if (samplerSetup) samplerSetup->setVisible(setupFace && id == ChainBlockId::Sampler);
    if (scoreSetup)   scoreSetup  ->setVisible(setupFace && id == ChainBlockId::Score);
    if (midiScoreSetup) midiScoreSetup->setVisible(setupFace && id == ChainBlockId::MidiScore);
    if (timbreSetup)  timbreSetup ->setVisible(setupFace && id == ChainBlockId::Timbre);
    if (voiceSetup)   voiceSetup  ->setVisible(setupFace && id == ChainBlockId::Voice);
    if (stralSetup)   stralSetup  ->setVisible(setupFace && id == ChainBlockId::LuxStral);
    if (synthSetup)   synthSetup  ->setVisible(setupFace && id == ChainBlockId::LuxSynth);
    if (waveSetup)    waveSetup   ->setVisible(setupFace && id == ChainBlockId::LuxWave);
    if (grainSetup)   grainSetup  ->setVisible(setupFace && id == ChainBlockId::LuxGrain);
}

//==============================================================================
// MIDI-follow — jump to the module whose parameter a controller just moved.
//==============================================================================
bool Sp3ctraAudioProcessorEditor::midiFollowEnabled() const
{
    if (auto* v = audioProcessor.getAPVTS().getRawParameterValue("midiFollowParam"))
        return v->load() > 0.5f;
    return false;
}

void Sp3ctraAudioProcessorEditor::timerCallback()
{
    const auto& chainModel = audioProcessor.getChainModel();
    const int chainCount = juce::jlimit(0,8,chainModel.numChains());
    // Engines with at least one send placed in a chain (AUDIO MIX rule): the
    // metrics bar hides the feed counter of an engine nothing feeds.
    unsigned engineMask = 0;
    {
        const ModuleType engines[] = { ModuleType::LuxStral, ModuleType::LuxSynth,
                                       ModuleType::LuxWave,  ModuleType::LuxGrain };
        for (const auto& chain : chainModel.chains)
            for (const auto& m : chain.modules)
                for (unsigned e = 0; e < 4; ++e)
                    if (m.type == engines[e] && m.slot >= 0 && m.slot < 8)
                        engineMask |= 1u << e;
    }
    pipelineMetricsBar_.sample(juce::Time::getMillisecondCounterHiRes(),
        (1u << chainCount)-1, engineMask, audioProcessor.getSampleRate(), audioProcessor.getBlockSize(),
        audioProcessor.monitorAudioBlocks(), audioProcessor.monitorLinkChanges(),
        waterfallColumn ? waterfallColumn->mixer().frameCounter() : 0,
        synth_staging_contention_holds());
    // VIDEO SCROLL pages: refresh the VIEWPORT pads' live thumbnail (only the
    // showing ones repaint, and only when the mixer published a new frame).
    if (videoScrollGridPage) videoScrollGridPage->previewTick();

    // SP3CTRA header power: mirrors the 3-state transport (no attachment).
    if (sp3HeaderPower_)
        if (auto* raw = audioProcessor.getAPVTS().getRawParameterValue("imageFreezeMode"))
            modulePowerButton.setToggleState(juce::roundToInt(raw->load()) != 2,
                                             juce::dontSendNotification);

    juce::String paramId;
    // Always drain the touch flag so turning the setting on later never replays
    // a stale move; only navigate while the setting is enabled.
    const bool touched = audioProcessor.getMidiMap().takeLastTouchedParam(paramId);
    if (touched)
    {
        // Light whatever control that parameter belongs to — "being edited"
        // is the same state whether the mouse or a CC is moving it
        // (ui/Sp3ctraControls.h; the sinks are the MidiLearnAttachments).
        Sp3ctraControls::MidiTouch::note(paramId);
        // Navigation honours the global MIDI menu toggle AND the per-mapping
        // follow flag (MIDI MAP panel) — the edit glow above always fires.
        if (midiFollowEnabled()
            && audioProcessor.getMidiMap().mappingFollows(paramId))
            followMidiParam(paramId);
    }

    // Session bar dot (saved / autosave-pending) — repaint only on change.
    if (auto* s = audioProcessor.sessions();
        s != nullptr && s->isStandalone())
    {
        const juce::String label = s->sessionName()
                                 + (s->hasUnsavedChanges() ? "*" : "");
        if (label != shownSessionLabel_)
            refreshSessionBar();
    }
}

void Sp3ctraAudioProcessorEditor::openMidiCurveEditor(const juce::String& paramId)
{
    if (paramId.isEmpty()) return;
    if (midiCurveWindow_ == nullptr)
    {
        midiCurveWindow_ = std::make_unique<MidiCurveWindow>(audioProcessor, paramId);
        // Close box, or the mapping vanished (fired from inside the engine's
        // broadcast) — delete on the next message-loop turn, never in the
        // window's own call chain.
        midiCurveWindow_->onCloseRequested = [this]
        {
            juce::MessageManager::callAsync(
                [sp = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
                { if (sp != nullptr) sp->midiCurveWindow_.reset(); });
        };
        midiCurveWindow_->onEdited = [this]
        { if (midiMapPanel != nullptr) midiMapPanel->syncFromEngine(); };
    }
    else
        midiCurveWindow_->setTarget(paramId);
    midiCurveWindow_->toFront(true);
}

void Sp3ctraAudioProcessorEditor::followMidiParam(const juce::String& paramId)
{
    const auto tgt = audioProcessor.navTargetForParam(paramId);
    if (! tgt.valid || chainRack == nullptr)
        return;   // param not tied to a rack module, or the module isn't present

    // VIDEO SCROLL (any output's videoScroll*/videoMix* param): a controller
    // lands on the ALL view — every output's page at once — rather than on
    // that output's chain tab (user request 2026-08-29). Already on ALL →
    // nothing to do (a CC sweep must not reset zone 3 every tick), whichever
    // output is bound. Otherwise bind the touched instance first (rack
    // selection + page bank, via onVideoBlockSelected) so leaving ALL later
    // lands on ITS chain, then flip to ALL.
    if (tgt.type == ModuleType::VideoScroll)
    {
        if (videoAllView_ && selectedBlock == ChainBlockId::VideoScroll && ! setupFace)
            return;
        chainRack->selectInstanceById(tgt.instanceId);
        setupFace = false;
        showVideoAllView();
        return;
    }

    // Already showing this exact target? Don't re-select — a CC sweep fires many
    // events and re-selecting would reset the zone-3 scroll position each tick.
    if (chainRack->selectedInstanceId() == tgt.instanceId
        && ! setupFace && engineView_ == tgt.engineView
        && (! tgt.controlsFace || sourceFace_ == 1))
        return;

    // Drive the rack like a user click: it rebinds the per-instance page/setup
    // bindings via its pre-callbacks and runs selectBlock() through our
    // onBlockSelected handler (which lands on the OUT view for synths).
    chainRack->selectInstanceById(tgt.instanceId);

    // Land on the face/view where the mapped control actually lives. selectBlock
    // only auto-resets the face on a block CHANGE, so force it for a same-block
    // re-target (e.g. a PLAY param touched while the SETUP face was showing, or
    // a synth ENGINE param that needs the engine page rather than the OUT page).
    bool reselect = false;
    if (setupFace)                     { setupFace = false;               reselect = true; }
    if (tgt.controlsFace && sourceFace_ != 1) { sourceFace_ = 1;         reselect = true; }
    else if (! tgt.controlsFace && sourceFace_ == 1
             && (selectedBlock == ChainBlockId::Chain1Source || selectedBlock == ChainBlockId::Chain2Source))
                                       { sourceFace_ = 0;                 reselect = true; }
    if (engineView_ != tgt.engineView) { engineView_ = tgt.engineView;    reselect = true; }
    if (reselect)
        selectBlock(selectedBlock);
}

//==============================================================================
// VIDEO SCROLL navigation — docs/PLAN_VIDEO_SCROLL_CHAIN_PAGES_ZOOM.md (D1–D3).
//==============================================================================
void Sp3ctraAudioProcessorEditor::refreshVideoTabs()
{
    const auto slots = audioProcessor.activeVideoSlots();   // {slot, chain}, rack order

    // A bound slot that left the model (module deleted / preset loaded) falls
    // back on the first output so the page never shows a ghost bank.
    int sel = 0;
    if (! videoAllView_ && ! slots.empty())
    {
        int found = -1;
        for (int i = 0; i < (int) slots.size(); ++i)
            if (slots[(size_t) i].first == videoSlotIndex_) { found = i; break; }
        if (found < 0)
        {
            found = 0;
            videoSlotIndex_ = slots.front().first;
            if (videoScrollGridPage) videoScrollGridPage->setSelectedSlot(videoSlotIndex_);
        }
        sel = found + 1;
    }

    juce::StringArray labels;
    labels.add("ALL");
    labels.addArray(videoScrollOutputLabels(slots, audioProcessor.chainNames()));
    faceSwitch.setCustomSegments(labels, sel);
    if (videoScrollGridPage) videoScrollGridPage->refresh(slots);
}

void Sp3ctraAudioProcessorEditor::showVideoAllView()
{
    if (chainRack == nullptr || ! chainRack->hasBlock(ChainBlockId::VideoScroll))
        return;
    videoAllView_ = true;
    engineView_   = false;
    // Keep the single page bound to a VIDEO SCROLL instance (kept when one
    // already is selected), so leaving ALL lands on that chain; selectBlock
    // below then highlights EVERY VIDEO SCROLL block for the ALL view.
    chainRack->setSelectedBlock(ChainBlockId::VideoScroll);
    {
        int c = -1, i = -1;
        if (const auto* m = audioProcessor.getChainModel().find(chainRack->selectedInstanceId(), c, i))
            if (m->type == ModuleType::VideoScroll && m->slot >= 0)
            {
                videoSlotIndex_ = m->slot;
                if (videoScrollGridPage) videoScrollGridPage->setSelectedSlot(videoSlotIndex_);
            }
    }
    selectBlock(ChainBlockId::VideoScroll);
    zone3Viewport.setViewPosition(0, 0);
    persistLayoutProps();
}

//==============================================================================
// Chain identity badge — centred in the face bar: the number + name of the
// chain owning the edited module, on every module edit page. Engine views
// (shared across chains) and the VIDEO SCROLL ALL view are chain-independent
// → no badge. Re-run on selection AND on model edits (chain rename).
void Sp3ctraAudioProcessorEditor::refreshChainBadge()
{
    const juce::Uuid uid = chainRack ? chainRack->selectedInstanceId()
                                     : juce::Uuid::null();
    int sc = -1, si = -1;
    const auto* m = audioProcessor.getChainModel().find(uid, sc, si);
    if (m == nullptr || (isSynthBlock(selectedBlock) && engineView_)
        || (selectedBlock == ChainBlockId::VideoScroll && videoAllView_))
    {
        faceSwitch.setChainIdentity(0, {}, {});
        return;
    }
    const auto& name = audioProcessor.getChainModel().chains[(size_t) sc].name;
    faceSwitch.setChainIdentity(sc + 1, ChainIdentity::label(name),
                                ChainIdentity::colour(sc));
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::selectBlock(ChainBlockId id)
{
    // Selecting another block always lands on the PLAY face (M5).
    if (id != selectedBlock)
        setupFace = false;

    // Engine view only exists for the synth blocks.
    if (!isSynthBlock(id))
        engineView_ = false;
    // LUXSTRAL: its OUT page has been empty since the 08-13 purge, so every
    // selection path (rack tile, session restore, MIDI-follow) lands on the
    // ENGINE page — the same view as its AUDIO MIX strip.
    else if (id == ChainBlockId::LuxStral)
        engineView_ = true;

    selectedBlock = id;
    if (chainRack)
    {
        chainRack->setSelectedBlock(id);
        // VIDEO SCROLL "ALL" page = every output at once → every VIDEO SCROLL
        // block reads selected in the rack (none singled out). Any other
        // view goes back to the one selected instance. Runs on every path
        // (ALL entry, chain-tab / rack-click exit, session restore).
        chainRack->setHighlightAllOfType(
            (videoAllView_ && id == ChainBlockId::VideoScroll)
                ? std::optional<ModuleType>(ModuleType::VideoScroll) : std::nullopt);
    }

    // Zone-3 toggles are CONTROLS: they take the handle colour like the bar
    // sliders and the graphic handles (Sp3ctraTheme::kColHandle — see
    // ui/Sp3ctraHandles.h), never the module colour, which is reserved for
    // the page's display chrome. Set explicitly on the host (drawToggleButton
    // reads tickColourId with parent inheritance) so no ancestor tint leaks
    // in. The repaint covers pages that stay visible across a selection
    // change (e.g. the shared OUT/send page when hopping between engine sends).
    zone3Content.setColour(juce::ToggleButton::tickColourId,
                           juce::Colour(Sp3ctraTheme::kColHandle));
    zone3Content.repaint();

    // ── Contextual selection tap ──────────────────────────────────────────────
    // Tell the processor WHICH INSTANCE is selected: the chain executor then
    // publishes the stream at that module's position in ITS chain (selection-
    // tap bus), and the SELECTED_TAP panel badge names the module + chain —
    // clicking a module in chain 2 must never show chain 1's stream.
    const juce::Uuid selUid = chainRack ? chainRack->selectedInstanceId()
                                        : juce::Uuid::null();
    audioProcessor.setVisualizerTapModule(selUid);

    // Pooled inserts (Pitch/Mask/Reverb/Echo) are per-instance: rebind the
    // zone-3 pages / setup panels to the SELECTED instance's param bank (pool
    // slot keyed by its UUID) — two Pitch modules on two chains edit two
    // independent parameter sets.
    const int insertSlot = audioProcessor.poolSlotForInstance(selUid);
    if (id == ChainBlockId::Pitch)
    {
        if (pitchPage)  pitchPage ->setSlot(insertSlot);
        if (pitchSetup) pitchSetup->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Mask)
    {
        if (maskPage)  maskPage ->setSlot(insertSlot);
        if (maskSetup) maskSetup->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Reverb)
    {
        if (reverbPage) reverbPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Echo)
    {
        if (echoPage) echoPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Equalizer)
    {
        if (eqPage) eqPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Harmonize)
    {
        if (harmoPage) harmoPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Centroid)
    {
        if (centroPage)  centroPage ->setSlot(insertSlot);
        if (centroSetup) centroSetup->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Drive)
    {
        if (drivePage) drivePage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::DcBlock)
    {
        if (dcBlockPage) dcBlockPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Gain)
    {
        if (gainPage) gainPage->setSlot(insertSlot);
    }
    else if (id == ChainBlockId::Diff)
    {
        if (diffPage) diffPage->setSlot(insertSlot);
    }
    // Synth blocks: rebind the OUT/send page to this send's conditioning bank.
    // The LuxStral slot is resolved from the SELECTED INSTANCE (not from the
    // restored engine index — a session restore may highlight another send).
    // M6 — every send type is instance-pooled: resolve the selected send's
    // bank slot from the INSTANCE (not from a restored index — a session
    // restore may highlight another send).
    int sendSlot = 0;
    if (isSynthBlock(id))
    {
        int sc = -1, si = -1;
        if (const auto* m = audioProcessor.getChainModel().find(selUid, sc, si))
            if (ChainModel::isEngineSend(m->type))
                sendSlot = juce::jlimit(0, ChainModel::kMaxChains - 1,
                                        m->slot >= 0 ? m->slot : 0);
        if (id == ChainBlockId::LuxStral)
        {
            luxStralSendSlot_ = sendSlot;
            // 2026-08-20: LUXSTRAL always selects in engine view (its OUT
            // page is gone), so the head panels always show the engine MIX
            // view — the per-send stream view (A+B, 2026-08-15) is retired.
            if (cisVisualizer)
                cisVisualizer->setSpctrViewChain(-1);
        }
    }
    if (synthOutPage != nullptr && isSynthBlock(id))
    {
        if (id == ChainBlockId::LuxStral)
            synthOutPage->setTarget(ModuleType::LuxStral, sendSlot);
        else if (id == ChainBlockId::LuxSynth)
            synthOutPage->setTarget(ModuleType::LuxSynth, sendSlot);
        else if (id == ChainBlockId::LuxGrain)
            synthOutPage->setTarget(ModuleType::LuxGrain, sendSlot);
        else
            synthOutPage->setTarget(ModuleType::LuxWave, sendSlot);
    }
    if (audioMixPanel != nullptr)
    {
        if (isSynthBlock(id) && engineView_)
            audioMixPanel->setSelectedEngine(id == ChainBlockId::LuxSynth ? ModuleType::LuxSynth
                                           : id == ChainBlockId::LuxWave  ? ModuleType::LuxWave
                                           : id == ChainBlockId::LuxGrain ? ModuleType::LuxGrain
                                                                          : ModuleType::LuxStral,
                                             true);
        else
            audioMixPanel->clearSelection();
    }
    if (cisVisualizer)
    {
        juce::String tapLabel;
        int sc = -1, si = -1;
        if (const auto* m = audioProcessor.getChainModel().find(selUid, sc, si))
        {
            tapLabel = moduleDisplayName(m->type).toUpperCase()
                     + " - " + audioProcessor.chainDisplayName(sc);
            // P5-M2 — SRC_* views read the SELECTED instance's own pool slot.
            if (ChainModel::isMediaSource(m->type))
                cisVisualizer->setSelectedSourceSlot(m->slot >= 0 ? m->slot : 0);
            // P5-M3 — the media play faces follow the selected instance.
            if (m->type == ModuleType::Image && imageSrcPage != nullptr)
                imageSrcPage->setSlot(m->slot >= 0 ? m->slot : 0);
            if (m->type == ModuleType::Video && videoSrcPage != nullptr)
                videoSrcPage->setSlot(m->slot >= 0 ? m->slot : 0);
            if (m->type == ModuleType::Camera && cameraSrcPage != nullptr)
                cameraSrcPage->setSlot(m->slot >= 0 ? m->slot : 0);
            // MIDI TAP follows the SELECTED instance's slot. Bound here rather
            // than through a rack pre-selection callback: setSelectedBlock()
            // calls selectInstance(id, notify=false), so those callbacks do NOT
            // fire on the session-restore path.
            if (m->type == ModuleType::MidiTap)
            {
                midiTapSlotIndex_ = m->slot >= 0 ? m->slot : 0;
                if (midiTapPage != nullptr)
                    midiTapPage->setSlot(midiTapSlotIndex_);
            }
            // P5-M5 — the generator pages follow the selected score-family
            // instance (its own player slot: transport, scrub, LOAD target).
            if (m->type == ModuleType::Score)
            {
                // P7 — the SCORE settings block the whole UI edits (PLAY page
                // AND SETUP panel) is the selected instance's. Push it FIRST:
                // both refreshes below read through it.
                audioProcessor.setScoreUiSlot(m->slot >= 0 ? m->slot : 0);
                if (scorePage != nullptr)
                    scorePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
                if (scoreSetup != nullptr)
                    scoreSetup->refreshFromSettings();
            }
            // P7 — the SETUP faces read their PLAY page's per-instance state:
            // refresh them right after the page swapped documents.
            if (m->type == ModuleType::Timbre && timbrePage != nullptr)
            {
                timbrePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
                if (timbreSetup != nullptr) timbreSetup->refresh();
            }
            if (m->type == ModuleType::MidiScore && midiScorePage != nullptr)
            {
                midiScorePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
                if (midiScoreSetup != nullptr) midiScoreSetup->refresh();
            }
            if (m->type == ModuleType::Voice && voicePage != nullptr)
            {
                voicePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
                if (voiceSetup != nullptr) voiceSetup->refresh();
            }
        }
        cisVisualizer->setSelectedTapLabel(tapLabel);
    }

    // ── ZONE 1: stacked visualizer panels — ALL outputs of this block ─────────
    // The visualizer shows every output of the selected module simultaneously,
    // one stacked panel each (contextual to the module).  The FIRST entry is
    // the primary output and drives the synthesis side-effects.  The panel
    // count is resolved here, BEFORE layoutZones(), because it sets ZONE 1's
    // height (and therefore the top of zones 2/3/4).
    std::vector<VisualizerMode> sources;
    switch (id)
    {
        // (P4-M3, D2) Every module selection is CONTEXTUAL: zone 1 shows the
        // stream AT the selected module's position in ITS chain (selection
        // tap, badge "MODULE - CHAIN n"). The legacy global views (RAW/LIVE/
        // MODULATED buses) are gone — a SOURCE CIS shows its chain's base.
        case ChainBlockId::Chain1Source:
        case ChainBlockId::Chain2Source:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // Mid-chain inserts are CONTEXTUAL: zone 1 shows the stream at the
        // selected module's output IN ITS OWN CHAIN (selection tap) — the old
        // global LUXPITCH/LUXMASK taps only reflected chain 1's executor.
        case ChainBlockId::Pitch:
        case ChainBlockId::Mask:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // The SAMPLER module is contextual too: its output at its position
        // in its own chain (input pass-through in idle, playback when it
        // drives — published by the walker/player at the exact position).
        case ChainBlockId::Sampler:
            sources = { VisualizerMode::SELECTED_TAP };
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
        // LUXGRAIN send — engine-input panels like the other OUT synths:
        // GRAY = the conditioned mix the cloud folds, COLOR = the colour
        // temperature that drives the per-grain pan.
        case ChainBlockId::LuxGrain:
            sources = { VisualizerMode::GRAIN_GRAY,
                        VisualizerMode::GRAIN_COLOR };
            break;
        // Score family: contextual like every other module — the stream at
        // their position in their chain (playback included, published by the
        // player's walk).
        case ChainBlockId::Score:
        case ChainBlockId::Timbre:
        case ChainBlockId::MidiScore:
        case ChainBlockId::Voice:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // Pass-through PROBES (VIDEO SCROLL, MIDI TAP) — zone 1 shows the
        // stream AT the probe's position in its chain (what the probe reads),
        // not the global Modulated bus.
        case ChainBlockId::VideoScroll:
        case ChainBlockId::MidiTap:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // FX inserts — contextual: their output at their position in THEIR
        // chain (a REVERB in chain 2 must never show chain 1's stream).
        case ChainBlockId::Reverb:
        case ChainBlockId::Echo:
        case ChainBlockId::Equalizer:
        case ChainBlockId::Harmonize:
        case ChainBlockId::Centroid:
        case ChainBlockId::Drive:
        case ChainBlockId::DcBlock:
        case ChainBlockId::Gain:
        case ChainBlockId::Diff:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // M9 — media sources: zone 1 shows the MODULE'S OWN line (internal
        // source pool tap) — contextual to the selected module, never the
        // global SP3CTRA live feed.
        case ChainBlockId::ImageSrc:
            sources = { VisualizerMode::SRC_IMAGE };
            break;
        case ChainBlockId::VideoSrc:
            sources = { VisualizerMode::SRC_VIDEO };
            break;
        case ChainBlockId::CameraSrc:
            sources = { VisualizerMode::SRC_CAMERA };
            break;
        // Empty rack — nothing selected, zone 1 shows its idle state.
        case ChainBlockId::RetiredSequencer:   // unreachable — retired ordinal
        case ChainBlockId::None:
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
        const bool imageMode = (id != ChainBlockId::Sampler && id != ChainBlockId::LuxWave
                             && id != ChainBlockId::None);
        cisVisualizer->setBlobOverlayVisible(imageMode);
    }

    // ── PLAY | SETUP switcher: visibility + accent follow the selection ──────
    // Blocks without a SETUP face still show the bar with the single PLAY
    // segment (uniform header across all modules); only the empty rack hides it.
    // The synth OUT/send view is PLAY-only — the SETUP face (engine config)
    // belongs to the engine view reached from the dock.
    const bool outView = isSynthBlock(id) && !engineView_;
    if (!blockHasSetup(id) || outView)
        setupFace = false;
    faceSwitch.setPlayOnly(!blockHasSetup(id) || outView);
    faceSwitch.setVisible(id != ChainBlockId::None);
    faceSwitch.setAccent(ChainRackComponent::blockColour(id));
    faceSwitch.setFace(setupFace, false);
    // VIDEO SCROLL: the face bar hosts the chain tabs (ALL | CHAIN n) instead
    // of PLAY | SETUP — refreshed here so a rack click / MIDI-follow / restore
    // always shows the tab of the bound instance.
    if (id == ChainBlockId::VideoScroll) refreshVideoTabs();
    else if (id == ChainBlockId::Chain1Source || id == ChainBlockId::Chain2Source)
    {
        // Three faces: the SETUP flag stays the truth for the third one.
        if (setupFace)                sourceFace_ = 2;
        else if (sourceFace_ == 2)    sourceFace_ = 0;
        faceSwitch.setCustomSegments({ "PLAY", "CONTROLS", "SETUP" }, sourceFace_);
    }
    else                                 faceSwitch.setCustomSegments({}, 0);

    refreshChainBadge();

    // Module power toggle (right of the face row) — rebind to this block's enable
    // param, or hide for blocks without a power switch (SOURCE CIS).
    {
        juce::String enableId = ChainRackComponent::enableParamId(id);
        // Engine sends power through THEIR bank's enable (per-send LED, M6);
        // the type-level ids (deviceEnabled/luxsynthEnabled/luxwaveEnabled/
        // luxgrainEnabled) are the ENGINE enables, which live on the AUDIO
        // MIX strips.
        if (id == ChainBlockId::LuxStral)
            enableId = lsOutParam(sendSlot, "enabled");
        else if (id == ChainBlockId::LuxSynth)
            enableId = lxOutParam(sendSlot, "enabled");
        else if (id == ChainBlockId::LuxWave)
            enableId = lwOutParam(sendSlot, "enabled");
        else if (id == ChainBlockId::LuxGrain)
            enableId = lgOutParam(sendSlot, "enabled");
        // Pooled inserts: the enable lives in the selected INSTANCE's bank
        // (the catalog's type-level id is empty for them).
        else if (id == ChainBlockId::Pitch)
            enableId = lpParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Mask)
            enableId = lmParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Reverb)
            enableId = rvParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Echo)
            enableId = ecParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Equalizer)
            enableId = eqParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Harmonize)
            enableId = hmParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Centroid)
            enableId = ctParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Drive)
            enableId = dvParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::DcBlock)
            enableId = dcbParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Gain)
            enableId = gnParam(insertSlot, "Enabled");
        else if (id == ChainBlockId::Diff)
            enableId = dfParam(insertSlot, "Enabled");
        // MIDI TAP is per-instance on its OWN pool (midiTapSlotIndex_, not the
        // shared insertSlot), so the header switch drives the same param as the
        // rack LED.
        else if (id == ChainBlockId::MidiTap)
            enableId = mtParam(midiTapSlotIndex_, "enabled");
        // P6 sampler engines are per-slot too: the catalog id is engine 0's
        // legacy global ("luxSamplerEnabled") — resolve the SELECTED instance's
        // engine so the switch drives the same param as its rack LED.
        else if (id == ChainBlockId::Sampler)
        {
            int sc = -1, si = -1;
            const auto* m = audioProcessor.getChainModel().find(selUid, sc, si);
            enableId = fsEngineParam(m != nullptr && m->slot >= 0 ? m->slot : 0,
                                     "Enabled");
        }
        // P5-M3 media sources are per-slot engines: resolve the SELECTED
        // instance's bank — the catalog id is slot 0's legacy global, and
        // binding it here would flip Chain 1's source from Chain 2's header
        // (the same trap the rack LED override fixes in ChainRackComponent).
        else if (id == ChainBlockId::ImageSrc || id == ChainBlockId::VideoSrc
              || id == ChainBlockId::CameraSrc)
        {
            int sc = -1, si = -1;
            const auto* m = audioProcessor.getChainModel().find(selUid, sc, si);
            const int mediaSlot = m != nullptr && m->slot >= 0 ? m->slot : 0;
            enableId = id == ChainBlockId::ImageSrc ? imgSrcParam(mediaSlot, "Enabled")
                     : id == ChainBlockId::VideoSrc ? vidSrcParam(mediaSlot, "Enabled")
                                                    : camSrcParam(mediaSlot, "Enabled");
        }
        // Score family (SCORE / TIMBRE / MIDI SCORE / VOICE): the switch is the
        // module ACTIVE toggle of the SELECTED instance's player slot — the very
        // param its rack LED drives, so header and LED always agree. Decoupled
        // from the page transport: deactivating pauses the reading where it is,
        // reactivating resumes it there (no re-press of PLAY).
        else if (isScoreFamily(chainBlockToModuleType(id)))
        {
            int sc = -1, si = -1;
            const auto* m = audioProcessor.getChainModel().find(selUid, sc, si);
            enableId = scoreActiveParam(m != nullptr && m->slot >= 0 ? m->slot : 0);
        }
        // Engine PLAY pages: the per-send on/off lives in the AUDIO MIX send
        // columns and on the rack tile LED — the header shows the ENGINE MUTE
        // instead: the engine's own enable param
        // (deviceEnabled / luxsynthEnabled / …) shown INVERTED (lit = off).
        // One truth with the AUDIO MIX strip's M and the rack: muting an
        // engine switches it off — anti-click fade, then zero CPU. A MIDI
        // mapping learned here speaks "power" (ON = audible), like the LEDs.
        juce::String engineMuteId;
        if (isSynthBlock(id) && engineView_)
            engineMuteId = moduleEnableParam(chainBlockToModuleType(id));
        moduleMuteAttachment.reset();
        moduleMuteLearn.reset();
        if (engineMuteId.isNotEmpty())
        {
            if (auto* param = audioProcessor.getAPVTS().getParameter(engineMuteId))
            {
                moduleMuteAttachment = std::make_unique<juce::ParameterAttachment>(
                    *param,
                    [this](float v)
                    {
                        moduleMuteButton.setToggleState(v < 0.5f,
                                                        juce::dontSendNotification);
                    },
                    audioProcessor.getAPVTS().undoManager);
                moduleMuteAttachment->sendInitialUpdate();
            }
            moduleMuteLearn = std::make_unique<MidiLearnAttachment>(
                audioProcessor.getMidiMap(), moduleMuteButton, engineMuteId);
        }
        moduleMuteButton.setVisible(engineMuteId.isNotEmpty());

        // Power follows the enable param alone — blocks without a SETUP face
        // (the FX inserts) still need their on/off switch here.
        const bool showPower = enableId.isNotEmpty() && engineMuteId.isEmpty();
        modulePowerAttachment.reset();   // detach before rebinding to a new param
        modulePowerLearn.reset();
        modulePowerButton.onClick = nullptr;   // owned by the SP3CTRA wiring below
        sp3HeaderPower_ = (id == ChainBlockId::Chain1Source
                        || id == ChainBlockId::Chain2Source);
        if (sp3HeaderPower_)
        {
            // SP3CTRA source: the module power is the 3-state imageFreezeMode
            // (0 play / 1 hold / 2 stop), not a bool — no ButtonAttachment.
            // ON = acquisition running (mode 0), OFF = stop (mode 2), exactly
            // what the rack LED shows; state mirrored in timerCallback().
            modulePowerButton.setAccent(ChainRackComponent::blockColour(id));
            modulePowerButton.onClick = [this]
            {
                if (auto* p = audioProcessor.getAPVTS().getParameter("imageFreezeMode"))
                    p->setValueNotifyingHost(modulePowerButton.getToggleState() ? 0.0f : 1.0f);
            };
            modulePowerLearn = std::make_unique<MidiLearnAttachment>(
                audioProcessor.getMidiMap(), modulePowerButton, "imageFreezeMode");
            if (auto* raw = audioProcessor.getAPVTS().getRawParameterValue("imageFreezeMode"))
                modulePowerButton.setToggleState(juce::roundToInt(raw->load()) != 2,
                                                 juce::dontSendNotification);
        }
        else if (showPower)
        {
            modulePowerButton.setAccent(ChainRackComponent::blockColour(id));
            modulePowerAttachment = std::make_unique<
                juce::AudioProcessorValueTreeState::ButtonAttachment>(
                    audioProcessor.getAPVTS(), enableId, modulePowerButton);
            modulePowerLearn = std::make_unique<MidiLearnAttachment>(
                audioProcessor.getMidiMap(), modulePowerButton, enableId);
        }
        modulePowerButton.setVisible(showPower || sp3HeaderPower_);
    }

    // ── Keyboard ruler under zone 1 (M5): PITCH / MASK only ──────────────────
    if (keyboardRuler)
    {
        keyboardRuler->setSlot(insertSlot);   // mirror the SELECTED instance
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

    // Selection (+ bindings captured by the rack callbacks just before this)
    // survives session reload. No-op during construction (width still 0).
    persistLayoutProps();
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
    if (apvts.getRawParameterValue("luxstralStereoEnable")->load() > 0.5f)
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

    // ── Header (single row): logo + title left, menu bar right ────────────────
    g.setGradientFill(juce::ColourGradient(
        juce::Colour(0xff383838), 0.f, 0.f,
        juce::Colour(0xff262626), 0.f, (float)kTitleRowH, false));
    g.fillRect(0, 0, getWidth(), kTitleRowH);

    // Logo picto (5 coloured bars) — slightly reduced, left side of header
    constexpr float pictoW = 28.f;
    constexpr float pictoH = 32.f;
    const float pictoX = 10.f;
    const float pictoY = ((float)kTitleRowH - pictoH) * 0.5f;
    Icons::drawSp3ctraLogoPicto(g, { pictoX, pictoY, pictoW, pictoH });

    // "Sp3ctra" text — right of the picto (version now lives in ABOUT)
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTitle)).boldened());
    const int textX = (int)(pictoX + pictoW + 6.f);
    g.drawText("Sp3ctra", juce::Rectangle<int>(textX, 0, getWidth() - textX - 24, kTitleRowH),
               juce::Justification::centredLeft, true);

    g.setColour(juce::Colour(0xff444444));
    g.fillRect(0, headerH(), getWidth(), 1);

    // ── Separator between zone 1 (visualizer [+ keyboard ruler]) and zones ───
    g.setColour(juce::Colour(0xff333333));
    g.fillRect(0, zonesTopY() - 1, getWidth(), 1);

    // ── Module catalogue rail — mirrors ZONE 4 (header band / collapsed grip).
    //    The rack sits flush to the right; its own left border is the divider. ─
    {
        const int ry    = zonesTopY();
        const int rh    = juce::jmax(0, getHeight() - ry - PipelineMetricsBar::kHeight);

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

    // The bottom monitoring bar is a child with its own lightweight repaint.
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::resized()
{
    // ── Header menu bar (right-aligned, single row) ───────────────────────────
    layoutHeaderMenus();

    // ── ZONE 1: CIS Visualizer — full window width; height = panel count ─────
    if (cisVisualizer)
        cisVisualizer->setBounds(kHPad, visY(), getWidth() - 2 * kHPad, visHeight());

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
        cisVisualizer->setBounds(kHPad, visY(), W - 2 * kHPad, visHeight());

    // Keyboard ruler (M5): directly under zone 1, same x-extent as the
    // visualizer; it takes kRulerH px from the top of the zones row.
    if (keyboardRuler && keyboardRuler->isVisible())
        keyboardRuler->setBounds(kHPad, visY() + visHeight(),
                                 juce::jmax(50, W - 2 * kHPad), kRulerH);

    // (Former ZONE 5 dock removed — the engines + MASTER live in the AUDIO MIX
    //  half of ZONE 4, laid out with the column below.)
    const int zonesY = zonesTopY();
    const int zonesH = juce::jmax(0, H - zonesY - PipelineMetricsBar::kHeight);
    pipelineMetricsBar_.setBounds(0,H-PipelineMetricsBar::kHeight,W,PipelineMetricsBar::kHeight);

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
    // The clamp lands in zone2Eff_/zone4Eff_ (what is DISPLAYED), never back
    // in zone2Width/zone4Width (the persisted USER INTENT): a narrow-window
    // launch must not permanently shrink the widths saved in the session.
    const bool collapsed   = waterfallColumn->isCollapsed();
    const int  rightSplitW = collapsed ? 0 : kSplitterW;

    // ── ZONE-4 section heights, resolved BEFORE the width clamp ──────────────
    // The MIDI MIX section only EXISTS while at least one MIDI TAP sits in a
    // chain: hasProbes() comes from activeMidiTapSlots() via refreshActiveSlots(),
    // which is why both of its call sites must run BEFORE layoutZones().
    const bool midiShown = midiMixPanel != nullptr
                        && midiMixPanel->hasProbes() && ! collapsed;
    if (midiMixPanel != nullptr) midiMixPanel->setVisible(midiShown);

    int mmH = 0;
    if (midiShown)
        mmH = midiMixPanel->isCollapsed()
            ? MidiMixPanel::kHeaderH
            : juce::jlimit(MidiMixPanel::kHeaderH,
                           juce::jmax(MidiMixPanel::kHeaderH, zonesH - 240),
                           midiMixPanel->preferredHeight());

    // LFO (the modulation bank) — same contract: always there, folds to its
    // header. It sits ABOVE the mapping list: a source over its destinations.
    const bool lfoShown = lfoPanel != nullptr && ! collapsed;
    if (lfoPanel != nullptr) lfoPanel->setVisible(lfoShown);
    int lfoH = 0;
    if (lfoShown)
        lfoH = juce::jlimit(LfoPanel::kHeaderH,
                            juce::jmax(LfoPanel::kHeaderH, zonesH - 240 - mmH),
                            lfoPanel->preferredHeight());

    // MIDI MAP (mapping list) — always present in the expanded band, its own
    // collapse chevron folds it to the header. Same clamp family as MIDI MIX.
    const bool mapShown = midiMapPanel != nullptr && ! collapsed;
    if (midiMapPanel != nullptr) midiMapPanel->setVisible(mapShown);
    int mapH = 0;
    if (mapShown)
        mapH = juce::jlimit(MidiMapPanel::kHeaderH,
                            juce::jmax(MidiMapPanel::kHeaderH, zonesH - 240 - mmH - lfoH),
                            midiMapPanel->preferredHeight());

    // AUDIO MIX takes its preferred height, VIDEO MIX keeps at least
    // 120 px — shrink the mixer when the window gets very short.
    const int amH = (audioMixPanel != nullptr && ! collapsed)
                  ? juce::jmin(AudioMixPanel::kPreferredH,
                               juce::jmax(120, zonesH - 120 - mmH - lfoH - mapH))
                  : 0;
    const int wfH = juce::jmax(0, zonesH - amH - mmH - lfoH - mapH);

    int z4w = VideoMixerColumn::kGripW;
    if (!collapsed)
    {
        const int z4Max = W - catRailW - kZone2MinW - kSplitterW - kZone3MinW - rightSplitW;
        // Past the point where the square video preview becomes height-limited,
        // extra zone width is dead space (every strip/panel in the column caps
        // at kMaxContentW) — the splitter stops there. Judged on the FULL zone
        // height, not wfH: collapsing MIDI MIX / MIDI MAP changes wfH and a
        // wfH-based cap made the whole column jump in width on every fold.
        const int z4Useful = juce::jmax(Sp3ctraTheme::kMaxContentW,
                                        waterfallColumn->maxUsefulWidth(zonesH));
        zone4Eff_ = juce::jlimit(kZone4MinW,
                                 juce::jmax(kZone4MinW, juce::jmin(z4Max, z4Useful)),
                                 zone4Width);
        z4w = zone4Eff_;
    }

    // The rack column is capped at kMaxContentW: past that the blocks would
    // just stretch (nothing else lives in zone 2), so the splitter stops.
    const int z2Max = juce::jmin(Sp3ctraTheme::kMaxContentW,
                                 W - catRailW - kSplitterW - kZone3MinW - rightSplitW - z4w);
    zone2Eff_ = juce::jlimit(kZone2MinW, juce::jmax(kZone2MinW, z2Max), zone2Width);

    // ── Place the columns left → right (chain rack flush against the rail) ────
    int x = catRailW;

    rackViewport.setBounds(x, zonesY, zone2Eff_, zonesH);
    x += zone2Eff_;

    splitterLeft.setBounds(x, zonesY, kSplitterW, zonesH);
    x += kSplitterW;

    const int z3w = juce::jmax(50, W - x - rightSplitW - z4w);
    int z3y = zonesY;
    int z3h = zonesH;
    faceSwitch.setBounds(x, z3y, z3w, kFaceBarH);
    if (modulePowerButton.isVisible())   // power switch at the right end of the row
    {
        // Right end of the CONTENT, not of a very wide zone — the page below
        // is capped at kMaxPageW (layoutZone3), the switch stays above it.
        const int pw = 36;
        const int ph = kFaceBarH - 8;
        const int faceW = juce::jmin(z3w, Sp3ctraTheme::kMaxPageW);
        modulePowerButton.setBounds(x + faceW - pw - 8, z3y + (kFaceBarH - ph) / 2, pw, ph);
    }
    if (moduleMuteButton.isVisible())    // engine MUTE takes the power's spot
    {
        const int pw = 44;
        const int ph = kFaceBarH - 8;
        const int faceW = juce::jmin(z3w, Sp3ctraTheme::kMaxPageW);
        moduleMuteButton.setBounds(x + faceW - pw - 8, z3y + (kFaceBarH - ph) / 2, pw, ph);
    }
    // The face row shifts the viewport when it hosts the PLAY | SETUP bar OR
    // just the power switch (blocks with an enable param but no SETUP face).
    if (faceSwitch.isVisible() || modulePowerButton.isVisible()
        || moduleMuteButton.isVisible())
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

    // ── ZONE 4 — VIDEO MIX above, MIDI MIX (only when a probe is patched),
    //    AUDIO MIX below (mmH / amH / wfH resolved above, with the clamp) ─────
    if (audioMixPanel != nullptr)
    {
        audioMixPanel->setMini(collapsed);
        if (collapsed)
        {
            // 24 px band: video grip (expand + ▶ ⏸/⏹ transport) on top, the
            // mini MASTER fader anchored at the bottom of the rest.
            const int gripH = juce::jmin(zonesH, 100);
            waterfallColumn->setBounds(x, zonesY, z4w, gripH);
            audioMixPanel  ->setBounds(x, zonesY + gripH, z4w,
                                       juce::jmax(0, zonesH - gripH));
        }
        else
        {
            waterfallColumn->setBounds(x, zonesY, z4w, wfH);
            if (midiShown)
                midiMixPanel->setBounds(x, zonesY + wfH, z4w, mmH);
            if (lfoShown)
                lfoPanel->setBounds(x, zonesY + wfH + mmH, z4w, lfoH);
            if (mapShown)
                midiMapPanel->setBounds(x, zonesY + wfH + mmH + lfoH, z4w, mapH);
            audioMixPanel  ->setBounds(x, zonesY + wfH + mmH + lfoH + mapH, z4w, amH);
        }
    }
    else
    {
        waterfallColumn->setBounds(x, zonesY, z4w, zonesH);
    }

    // ── Rack content sizing (viewport scrolls when the window is short) ──────
    const int rackW = juce::jmax(60, zone2Eff_ - rackViewport.getScrollBarThickness());
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

    // Pages stop stretching past kMaxPageW (two content columns): on a very
    // wide window the page keeps its natural width, left-aligned in the zone.
    const int cw = juce::jmax(120, juce::jmin(vpW - zone3Viewport.getScrollBarThickness(),
                                              Sp3ctraTheme::kMaxPageW));

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
            case ChainBlockId::Centroid:
                top = centroSetup.get();  topMinH = CentroSetupPanel::kPreferredH;  break;
            case ChainBlockId::Sampler:
                top = samplerSetup.get(); topMinH = SamplerSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxStral:
                top = stralSetup.get();   topMinH = LuxStralSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxSynth:
                top = synthSetup.get();   topMinH = LuxSynthSetupPanel::kPreferredH; break;
            case ChainBlockId::LuxWave:
                top = waveSetup.get();    topMinH = LuxWaveSetupPanel::kPreferredH;  break;
            case ChainBlockId::LuxGrain:
                top = grainSetup.get();   topMinH = LuxGrainSetupPanel::kPreferredH; break;
            case ChainBlockId::Score:
                top = scoreSetup.get();   topMinH = ScoreSetupPanel::kPreferredH;    break;
            case ChainBlockId::MidiScore:
                top = midiScoreSetup.get(); topMinH = MidiScoreSetupPanel::kPreferredH; break;
            case ChainBlockId::Timbre:
                top = timbreSetup.get();  topMinH = TimbreSetupPanel::kPreferredH;   break;
            case ChainBlockId::Voice:
                top = voiceSetup.get();   topMinH = VoiceSetupPanel::kPreferredH;    break;
            case ChainBlockId::Chain1Source:
            case ChainBlockId::Chain2Source:
                top = sourceSetup.get();  topMinH = SourceSetupPanel::kPreferredH;    break;
            case ChainBlockId::VideoScroll:   // chain tabs instead of SETUP (2026-08-28)
            case ChainBlockId::ImageSrc:
            case ChainBlockId::VideoSrc:
            case ChainBlockId::CameraSrc:   // M9 — picking moved to the PLAY page
            case ChainBlockId::RetiredSequencer:
            case ChainBlockId::Reverb:
            case ChainBlockId::Echo:
            case ChainBlockId::Equalizer:
            case ChainBlockId::Harmonize:
            case ChainBlockId::Drive:
            case ChainBlockId::DcBlock:
            case ChainBlockId::Gain:
            case ChainBlockId::Diff:
            case ChainBlockId::MidiTap:   // the MIDI MIX master owns the settings
            case ChainBlockId::None:
                break;   // no SETUP face (blockHasSetup == false)
        }
    }
    else
    {
        switch (selectedBlock)
        {
            case ChainBlockId::Chain1Source:
            case ChainBlockId::Chain2Source:
                if (sourceFace_ == 1) { top = controlsPage.get(); topMinH = Sp3ctraControlsPage::kPreferredH; }
                else                  { top = sourcesPage.get();  topMinH = 260; }   // +acquisition-speed group
                break;
            case ChainBlockId::Pitch:
                top = pitchPage.get();       topMinH = LuxPitchTabComponent::kPreferredH; break;
            case ChainBlockId::Mask:
                top = maskPage.get();        topMinH = LuxMaskTabComponent::kPreferredH;  break;
            case ChainBlockId::Sampler:
                top = samplerPage.get();     topMinH = SamplerPageComponent::kPreferredH; break;
            // Synth blocks (P2): OUT/send page from the rack, engine page from
            // the dock — same slot in zone 3, view picked by engineView_.
            case ChainBlockId::LuxStral:
                if (engineView_) { top = imgLuxStralPage.get(); topMinH = imgLuxStralPage->preferredHeight(); }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxSynth:
                if (engineView_) { top = imgLuxSynthPage.get(); topMinH = imgLuxSynthPage->preferredHeight(); }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxWave:
                if (engineView_) { top = audioWavePanel.get();  topMinH = audioWavePanel->preferredHeight(); }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxGrain:
                if (engineView_) { top = luxGrainPanel.get(); topMinH = luxGrainPanel->preferredHeight(); }
                else             { top = synthOutPage.get();  topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::Score:
                top = scorePage.get();       topMinH = 360; break;  // actions + transport only
            case ChainBlockId::Timbre:
                top = timbrePage.get();      topMinH = TimbreGenTabComponent::kPreferredH; break;
            case ChainBlockId::MidiScore:
                top = midiScorePage.get();   topMinH = MidiScoreGenTabComponent::kPreferredH; break;
            case ChainBlockId::Voice:
                top = voicePage.get();       topMinH = VoiceGenTabComponent::kPreferredH; break;
            case ChainBlockId::VideoScroll:
                top = videoScrollGridPage.get();
                topMinH = videoScrollGridPage->preferredHeight(cw);
                break;
            case ChainBlockId::Reverb:
                top = reverbPage.get();      topMinH = LuxReverbTabComponent::kPreferredH; break;
            case ChainBlockId::Echo:
                top = echoPage.get();        topMinH = LuxEchoTabComponent::kPreferredH; break;
            case ChainBlockId::Equalizer:
                top = eqPage.get();          topMinH = LuxEqTabComponent::kPreferredH; break;
            case ChainBlockId::Harmonize:
                top = harmoPage.get();       topMinH = LuxHarmoTabComponent::kPreferredH; break;
            case ChainBlockId::Centroid:
                top = centroPage.get();      topMinH = LuxCentroTabComponent::kPreferredH; break;
            case ChainBlockId::Drive:
                top = drivePage.get();       topMinH = LuxDriveTabComponent::kPreferredH; break;
            case ChainBlockId::DcBlock:
                top = dcBlockPage.get();     topMinH = LuxDcBlockTabComponent::kPreferredH; break;
            case ChainBlockId::Gain:
                top = gainPage.get();        topMinH = LuxGainTabComponent::kPreferredH; break;
            case ChainBlockId::Diff:
                top = diffPage.get();        topMinH = LuxDiffTabComponent::kPreferredH; break;
            case ChainBlockId::ImageSrc:
                top = imageSrcPage.get();    topMinH = MediaSourcePage::kPreferredH; break;
            case ChainBlockId::VideoSrc:
                top = videoSrcPage.get();    topMinH = MediaSourcePage::kPreferredH; break;
            case ChainBlockId::CameraSrc:
                top = cameraSrcPage.get();   topMinH = MediaSourcePage::kPreferredH; break;
            case ChainBlockId::MidiTap:
                top = midiTapPage.get();     topMinH = MidiTapPage::preferredHeight(); break;
            case ChainBlockId::RetiredSequencer:   // unreachable — retired ordinal
            case ChainBlockId::None:
                break;   // empty rack — zone 3 stays blank
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
    // Constructor-time layout passes (setResizeLimits snapping 0×0 to the
    // minimum, early selectBlock) must never overwrite the restored layout.
    if (! layoutRestoreDone_)
        return;

    // Message-thread only: properties ride along with the APVTS session state
    // (getStateInformation serialises apvts.state including these). The window
    // layout is SESSION-scoped by design: each project carries its own
    // workspace arrangement (size, zone widths, collapsed bands).
    auto& state = audioProcessor.getAPVTS().state;
    state.setProperty("editorW", getWidth(),  nullptr);
    state.setProperty("editorH", getHeight(), nullptr);
    state.setProperty("zone2W",  zone2Width,  nullptr);
    state.setProperty("zone4W",  zone4Width,  nullptr);
    state.setProperty("scrollCollapsed",
                      waterfallColumn != nullptr && waterfallColumn->isCollapsed(),
                      nullptr);
    state.setProperty("catalogCollapsed", catalogCollapsed, nullptr);
    state.setProperty("midiMixCollapsed",
                      midiMixPanel != nullptr && midiMixPanel->isCollapsed(),
                      nullptr);
    state.setProperty("midiMapCollapsed",
                      midiMapPanel != nullptr && midiMapPanel->isCollapsed(),
                      nullptr);
    state.setProperty("lfoCollapsed",
                      lfoPanel != nullptr && lfoPanel->isCollapsed(),
                      nullptr);
    state.setProperty("selMidiTapSlot", midiTapSlotIndex_, nullptr);

    // Zone-3 selection — which block/page the user was editing, its PLAY/SETUP
    // face and the engine/instance bindings behind it. Restored in the ctor.
    state.setProperty("selBlock",          (int) selectedBlock,  nullptr);
    state.setProperty("selSetupFace",      setupFace,            nullptr);
    state.setProperty("selSourceFace",     sourceFace_,          nullptr);
    state.setProperty("selEngineView",     engineView_,          nullptr);
    state.setProperty("selLuxStralSend", luxStralSendSlot_, nullptr);
    state.setProperty("selSamplerEngine",  samplerEngineIndex_,  nullptr);
    state.setProperty("selSamplerBank",
                      audioProcessor.getSamplerSelectedSlot(),   nullptr);
    state.setProperty("selVideoSlot",      videoSlotIndex_,      nullptr);
    state.setProperty("selVideoAll",       videoAllView_,        nullptr);
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
