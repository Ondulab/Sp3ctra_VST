#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "IconPaths.h"
#include "ui/ScrollWheelGuard.h"
#include "Sp3ctraVersion.h"
#include "Sp3ctraDialog.h"   // session-bar prompts (name input / confirm)
#include "AppUpdater.h"
#include "UpdateDialog.h"
#include "licensing/ActivationDialog.h"

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
    // A rack click on a synth block opens its OUT/send page (the engine page
    // is reached from the ZONE-5 dock card) — synth-split P2.
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
        if (videoScrollPage)  videoScrollPage ->setSlot(slot);
        if (videoScrollSetup) videoScrollSetup->setSlot(slot);
    };
    // Selecting a SAMPLER block binds the sampler page + setup to engine A/B
    // (slot 0 = A, 1 = B), fired just before onBlockSelected → selectBlock.
    chainRack->onSamplerBlockSelected = [this](int slot)
    {
        samplerEngineIndex_ = (slot == 1) ? 1 : 0;
        if (samplerPage)  samplerPage ->setSamplerIndex(slot);
        if (samplerSetup) samplerSetup->setSamplerIndex(slot);
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
        layoutZones();
        if (waterfallColumn) waterfallColumn->refreshActiveSlots();   // outputs added/removed
    };
    // State restore with the editor open (host preset change / project
    // reload): rebuild the rack from the NEW model — the audio follows the
    // restored topology immediately while the rack otherwise kept showing the
    // old blocks (ghost drops, stale LEDs) until the window was reopened.
    audioProcessor.onStateRestoredUi = [this]
    {
        if (chainRack)       chainRack->refreshFromModel();
        if (waterfallColumn) waterfallColumn->refreshActiveSlots();
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

    videoScrollPage = std::make_unique<VideoScrollPage>(audioProcessor);
    zone3Content.addChildComponent(videoScrollPage.get());

    // FX — REVERB / ECHO / EQ / SCALE insert pages (all controls on the PLAY face)
    reverbPage = std::make_unique<LuxReverbTabComponent>(audioProcessor);
    echoPage   = std::make_unique<LuxEchoTabComponent>(audioProcessor);
    eqPage     = std::make_unique<LuxEqTabComponent>(audioProcessor);
    harmoPage  = std::make_unique<LuxHarmoTabComponent>(audioProcessor);
    zone3Content.addChildComponent(reverbPage.get());
    zone3Content.addChildComponent(echoPage.get());
    zone3Content.addChildComponent(eqPage.get());
    zone3Content.addChildComponent(harmoPage.get());

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
    videoScrollSetup = std::make_unique<VideoScrollSetupPanel>(
        audioProcessor, ChainRackComponent::blockColour(ChainBlockId::VideoScroll));
    // M9 — media modules: source picking lives on the PLAY page now
    // (MediaSourcePage hosts LOAD/CLEAR/device combo); no SETUP face.
    zone3Content.addChildComponent(sourceSetup.get());
    zone3Content.addChildComponent(pitchSetup.get());
    zone3Content.addChildComponent(maskSetup.get());
    zone3Content.addChildComponent(stralSetup.get());
    zone3Content.addChildComponent(synthSetup.get());
    zone3Content.addChildComponent(waveSetup.get());
    zone3Content.addChildComponent(grainSetup.get());
    zone3Content.addChildComponent(samplerSetup.get());
    zone3Content.addChildComponent(scoreSetup.get());
    zone3Content.addChildComponent(midiScoreSetup.get());
    zone3Content.addChildComponent(timbreSetup.get());
    zone3Content.addChildComponent(voiceSetup.get());
    zone3Content.addChildComponent(videoScrollSetup.get());

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
    addChildComponent(faceSwitch);
    addChildComponent(modulePowerButton);

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

    menuAdvancedBtn_.setTooltip("Advanced: log level, worker threads.");
    menuAdvancedBtn_.onClick = [this] { showAdvancedMenu(); };
    addAndMakeVisible(menuAdvancedBtn_);

    menuAboutBtn_.setTooltip("About Sp3ctra, software update, license, donate.");
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

    // ── Restore persisted layout (survives session reload) ────────────────────
    auto& state = apvts.state;
    zone2Width = (int) state.getProperty("zone2W", kZone2DefaultW);
    zone4Width = (int) state.getProperty("zone4W", kZone4DefaultW);
    if ((bool) state.getProperty("scrollCollapsed", false))
        waterfallColumn->setCollapsed(true, false);
    if ((bool) state.getProperty("catalogCollapsed", false))
        setCatalogCollapsed(true, false);   // also locks the chain rack

    // ── Restore the zone-3 selection (block + face + engine bindings) ─────────
    // Bindings first, so the restored selection lands on the same engine /
    // video instance the user was editing (rack clicks set these callbacks-
    // first for the same reason).
    luxStralSendSlot_ = juce::jlimit(0, ChainModel::kMaxChains - 1,
        (int) state.getProperty("selLuxStralSend", 0));
    samplerEngineIndex_  = juce::jlimit(0, 1,
        (int) state.getProperty("selSamplerEngine", 0));
    videoSlotIndex_      = juce::jlimit(0, ChainModel::kMaxVideoSlots - 1,
        (int) state.getProperty("selVideoSlot", 0));
    if (samplerPage)     samplerPage    ->setSamplerIndex(samplerEngineIndex_);
    if (samplerSetup)    samplerSetup   ->setSamplerIndex(samplerEngineIndex_);
    if (videoScrollPage)  videoScrollPage ->setSlot(videoSlotIndex_);
    if (videoScrollSetup) videoScrollSetup->setSlot(videoSlotIndex_);

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
    engineView_   = (bool) state.getProperty("selEngineView", false);
    selectedBlock = sel;
    selectBlock(sel);

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
    if (auto* up = AppUpdater::getInstanceWithoutCreating())
        up->removeChangeListener(this);
    audioProcessor.onStateRestoredUi = nullptr;   // this editor is going away
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
                        p->setValueNotifyingHost(
                            p->getValue() >= 0.5f ? 0.0f : 1.0f);
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
                    ("Could not write\n" + file.getFullPathName()).toRawUTF8());
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
                    (file.getFileName()
                     + " is not a valid Sp3ctra MIDI mappings file.").toRawUTF8());
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
                    ("Importing replaces your current " + juce::String(current)
                     + " assignment(s).").toRawUTF8(),
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

    juce::PopupMenu m;
    m.addSectionHeader("ADVANCED");
    m.addSubMenu("Log level",      logMenu);
    m.addSubMenu("Worker threads", workersMenu);

    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&menuAdvancedBtn_),
        [safe = juce::Component::SafePointer<Sp3ctraAudioProcessorEditor>(this)]
        (int choice)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0) return;
            auto& ap = self->audioProcessor.getAPVTS();
            auto setDenorm = [&ap](const char* id, float denorm)
            {
                if (auto* p = ap.getParameter(id))
                    p->setValueNotifyingHost(p->convertTo0to1(denorm));
            };
            if (choice >= 100 && choice < 200)
                setDenorm("logLevel", (float) (choice - 100));
            else if (choice >= 200)
                setDenorm("luxstralNumWorkers", (float) (choice - 200));
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
    m.addItem(4, juce::String::fromUTF8("Donate ♥ (PayPal)"));
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
                case 4: open(OndulabLinks::kDonateUrl);    break;
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
    // configuration there (formerly the gear-wheel Network tab); the VIDEO SCROLL
    // output hosts its per-instance background/frame colour; MIDI SCORE hosts
    // its export prefs (PNG/JPEG, A4/A3/FULL, DPI) — EXCEPT the
    // REVERB / ECHO / EQ FX inserts (single PLAY page), and the
    // IMAGE / VIDEO / CAMERA media modules (source picking lives on PLAY).
    return id != ChainBlockId::RetiredSequencer
        && id != ChainBlockId::Reverb    && id != ChainBlockId::Echo
        && id != ChainBlockId::Equalizer && id != ChainBlockId::Harmonize
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
    const bool showSources = play && (id == ChainBlockId::Chain1Source
                                   || id == ChainBlockId::Chain2Source);
    if (sourcesPage)     sourcesPage    ->setVisible(showSources);
    if (pitchPage)       pitchPage      ->setVisible(play && id == ChainBlockId::Pitch);
    if (maskPage)        maskPage       ->setVisible(play && id == ChainBlockId::Mask);
    if (samplerPage)     samplerPage    ->setVisible(play && id == ChainBlockId::Sampler);
    // Synth blocks (P2): engine pages only in ENGINE view (dock); the rack
    // click shows the OUT/send page instead.
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
    if (videoScrollPage) videoScrollPage->setVisible(play && id == ChainBlockId::VideoScroll);
    if (imageSrcPage)    imageSrcPage   ->setVisible(play && id == ChainBlockId::ImageSrc);
    if (videoSrcPage)    videoSrcPage   ->setVisible(play && id == ChainBlockId::VideoSrc);
    if (cameraSrcPage)   cameraSrcPage  ->setVisible(play && id == ChainBlockId::CameraSrc);

    // ── SETUP face: the per-block settings panel ──────────────────────────────
    if (sourceSetup)  sourceSetup ->setVisible(setupFace && (id == ChainBlockId::Chain1Source
                                                          || id == ChainBlockId::Chain2Source));
    if (pitchSetup)   pitchSetup  ->setVisible(setupFace && id == ChainBlockId::Pitch);
    if (maskSetup)    maskSetup   ->setVisible(setupFace && id == ChainBlockId::Mask);
    if (samplerSetup) samplerSetup->setVisible(setupFace && id == ChainBlockId::Sampler);
    if (scoreSetup)   scoreSetup  ->setVisible(setupFace && id == ChainBlockId::Score);
    if (midiScoreSetup) midiScoreSetup->setVisible(setupFace && id == ChainBlockId::MidiScore);
    if (timbreSetup)  timbreSetup ->setVisible(setupFace && id == ChainBlockId::Timbre);
    if (voiceSetup)   voiceSetup  ->setVisible(setupFace && id == ChainBlockId::Voice);
    if (stralSetup)   stralSetup  ->setVisible(setupFace && id == ChainBlockId::LuxStral);
    if (synthSetup)   synthSetup  ->setVisible(setupFace && id == ChainBlockId::LuxSynth);
    if (waveSetup)    waveSetup   ->setVisible(setupFace && id == ChainBlockId::LuxWave);
    if (grainSetup)   grainSetup  ->setVisible(setupFace && id == ChainBlockId::LuxGrain);
    if (videoScrollSetup) videoScrollSetup->setVisible(setupFace && id == ChainBlockId::VideoScroll);
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
    juce::String paramId;
    // Always drain the touch flag so turning the setting on later never replays
    // a stale move; only navigate while the setting is enabled.
    const bool touched = audioProcessor.getMidiMap().takeLastTouchedParam(paramId);
    if (touched && midiFollowEnabled())
        followMidiParam(paramId);

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

void Sp3ctraAudioProcessorEditor::followMidiParam(const juce::String& paramId)
{
    const auto tgt = audioProcessor.navTargetForParam(paramId);
    if (! tgt.valid || chainRack == nullptr)
        return;   // param not tied to a rack module, or the module isn't present

    // Already showing this exact target? Don't re-select — a CC sweep fires many
    // events and re-selecting would reset the zone-3 scroll position each tick.
    if (chainRack->selectedInstanceId() == tgt.instanceId
        && ! setupFace && engineView_ == tgt.engineView)
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
    if (engineView_ != tgt.engineView) { engineView_ = tgt.engineView;    reselect = true; }
    if (reselect)
        selectBlock(selectedBlock);
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

    selectedBlock = id;
    if (chainRack)
        chainRack->setSelectedBlock(id);

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
            luxStralSendSlot_ = sendSlot;
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
                     + " - CHAIN " + juce::String(sc + 1);
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
            // P5-M5 — the generator pages follow the selected score-family
            // instance (its own player slot: transport, scrub, LOAD target).
            if (m->type == ModuleType::Score && scorePage != nullptr)
                scorePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
            if (m->type == ModuleType::Timbre && timbrePage != nullptr)
                timbrePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
            if (m->type == ModuleType::MidiScore && midiScorePage != nullptr)
                midiScorePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
            if (m->type == ModuleType::Voice && voicePage != nullptr)
                voicePage->setScoreSlot(m->slot >= 0 ? m->slot : 0);
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
        // VIDEO SCROLL probe — zone 1 shows the stream AT the probe's position
        // in its chain (what the probe captures), not the global Modulated bus.
        case ChainBlockId::VideoScroll:
            sources = { VisualizerMode::SELECTED_TAP };
            break;
        // FX inserts — contextual: their output at their position in THEIR
        // chain (a REVERB in chain 2 must never show chain 1's stream).
        case ChainBlockId::Reverb:
        case ChainBlockId::Echo:
        case ChainBlockId::Equalizer:
        case ChainBlockId::Harmonize:
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

    // Module power toggle (right of the face row) — rebind to this block's enable
    // param, or hide for blocks without a power switch (SOURCE CIS, SCORE).
    {
        juce::String enableId = ChainRackComponent::enableParamId(id);
        // Engine sends power through THEIR bank's enable (per-send LED, M6);
        // the type-level ids (deviceEnabled/luxsynthEnabled/luxwaveEnabled)
        // are the ENGINE enables, which live on the AUDIO MIX strips.
        if (id == ChainBlockId::LuxStral)
            enableId = lsOutParam(sendSlot, "enabled");
        else if (id == ChainBlockId::LuxSynth)
            enableId = lxOutParam(sendSlot, "enabled");
        else if (id == ChainBlockId::LuxWave)
            enableId = lwOutParam(sendSlot, "enabled");
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
        // Power follows the enable param alone — blocks without a SETUP face
        // (the FX inserts) still need their on/off switch here.
        const bool showPower = enableId.isNotEmpty();
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
    const int zonesH = juce::jmax(0, H - zonesY);

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
    // The face row shifts the viewport when it hosts the PLAY | SETUP bar OR
    // just the power switch (blocks with an enable param but no SETUP face).
    if (faceSwitch.isVisible() || modulePowerButton.isVisible())
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

    // ── ZONE 4 — VIDEO MIX above, AUDIO MIX below (P2b) ──────────────────────
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
            // AUDIO MIX takes its preferred height, VIDEO MIX keeps at least
            // 120 px — shrink the mixer when the window gets very short.
            const int amH = juce::jmin(AudioMixPanel::kPreferredH,
                                       juce::jmax(120, zonesH - 120));
            waterfallColumn->setBounds(x, zonesY, z4w, juce::jmax(0, zonesH - amH));
            audioMixPanel  ->setBounds(x, zonesY + zonesH - amH, z4w, amH);
        }
    }
    else
    {
        waterfallColumn->setBounds(x, zonesY, z4w, zonesH);
    }

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
            case ChainBlockId::VideoScroll:
                top = videoScrollSetup.get(); topMinH = VideoScrollSetupPanel::kPreferredH; break;
            case ChainBlockId::ImageSrc:
            case ChainBlockId::VideoSrc:
            case ChainBlockId::CameraSrc:   // M9 — picking moved to the PLAY page
            case ChainBlockId::RetiredSequencer:
            case ChainBlockId::Reverb:
            case ChainBlockId::Echo:
            case ChainBlockId::Equalizer:
            case ChainBlockId::Harmonize:
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
                top = sourcesPage.get();     topMinH = 260; break;  // +acquisition-speed group
            case ChainBlockId::Pitch:
                top = pitchPage.get();       topMinH = 510; break;  // +100 env editor
            case ChainBlockId::Mask:
                top = maskPage.get();        topMinH = 570; break;  // +100 env editor
            case ChainBlockId::Sampler:
                top = samplerPage.get();     topMinH = SamplerPageComponent::kPreferredH; break;
            // Synth blocks (P2): OUT/send page from the rack, engine page from
            // the dock — same slot in zone 3, view picked by engineView_.
            case ChainBlockId::LuxStral:
                if (engineView_) { top = imgLuxStralPage.get(); topMinH = LuxStralTabComponent::kPreferredH; }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxSynth:
                if (engineView_) { top = imgLuxSynthPage.get(); topMinH = LuxSynthTabComponent::kPreferredH; }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxWave:
                if (engineView_) { top = audioWavePanel.get();  topMinH = AudioWavePanel::kPreferredH; }
                else             { top = synthOutPage.get();    topMinH = SynthOutPageComponent::kPreferredH; }
                break;
            case ChainBlockId::LuxGrain:
                if (engineView_) { top = luxGrainPanel.get(); topMinH = LuxGrainPanel::kPreferredH; }
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
                top = videoScrollPage.get(); topMinH = VideoScrollPage::kPreferredH; break;
            case ChainBlockId::Reverb:
                top = reverbPage.get();      topMinH = LuxReverbTabComponent::kPreferredH; break;
            case ChainBlockId::Echo:
                top = echoPage.get();        topMinH = LuxEchoTabComponent::kPreferredH; break;
            case ChainBlockId::Equalizer:
                top = eqPage.get();          topMinH = LuxEqTabComponent::kPreferredH; break;
            case ChainBlockId::Harmonize:
                top = harmoPage.get();       topMinH = LuxHarmoTabComponent::kPreferredH; break;
            case ChainBlockId::ImageSrc:
                top = imageSrcPage.get();    topMinH = MediaSourcePage::kPreferredH; break;
            case ChainBlockId::VideoSrc:
                top = videoSrcPage.get();    topMinH = MediaSourcePage::kPreferredH; break;
            case ChainBlockId::CameraSrc:
                top = cameraSrcPage.get();   topMinH = MediaSourcePage::kPreferredH; break;
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

    // Zone-3 selection — which block/page the user was editing, its PLAY/SETUP
    // face and the engine/instance bindings behind it. Restored in the ctor.
    state.setProperty("selBlock",          (int) selectedBlock,  nullptr);
    state.setProperty("selSetupFace",      setupFace,            nullptr);
    state.setProperty("selEngineView",     engineView_,          nullptr);
    state.setProperty("selLuxStralSend", luxStralSendSlot_, nullptr);
    state.setProperty("selSamplerEngine",  samplerEngineIndex_,  nullptr);
    state.setProperty("selVideoSlot",      videoSlotIndex_,      nullptr);
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
