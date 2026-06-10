#include "SlotEditorComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

static const char* kLoopLabels[4] = { "NONE", "LOOP", "INV", "PING" };
static const char* kNoteNamesEd[LuxSamplerConstants::NUM_SLOTS] = {
    "C1","C#1","D1","D#1","E1","F1","F#1","G1","G#1","A1","A#1","B1"
};

SlotEditorComponent::SlotEditorComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      timeline(proc) // SlotTimelineComponent ctor
{
    // ── Timeline ──────────────────────────────────────────────────────────────
    // Start/End handles are dragged directly on the timeline and update
    // LuxSampler atomics in SlotTimelineComponent::mouseDrag() — no sliders.
    // The onStartChanged / onEndChanged callbacks are not used here.
    addAndMakeVisible(timeline);

    // ── Action buttons ────────────────────────────────────────────────────────
    recBtn.onClick = [this]
    {
        if (auto* fs = processor.getLuxSampler())
        {
            fs->uiToggleRecord(selectedSlot);
            timeline.markDirty(); // thumbnail may have changed after record
        }
    };
    addAndMakeVisible(recBtn);

    playBtn.onClick = [this]
    {
        if (auto* fs = processor.getLuxSampler())
        {
            fs->uiPlaySlot(selectedSlot);
            // Auto-activate sampler transport PLAY so the pipeline processes
            // the injected frames.  Without this, the user would have to
            // manually set the IMAGE S–Sampler transport to PLAY first.
            if (auto* p = processor.getAPVTS().getParameter("samplerFreezeMode"))
                p->setValueNotifyingHost(0.0f); // 0 = PLAY
        }
    };
    addAndMakeVisible(playBtn);

    clearBtn.onClick = [this]
    {
        if (auto* fs = processor.getLuxSampler())
        {
            fs->uiClearSlot(selectedSlot);
            timeline.markDirty();
        }
    };
    addAndMakeVisible(clearBtn);

    // ── SAVE button — direct write (no dialog) ────────────────────────────────
    // Filename pattern: YYYYMMDD-HHMMSS_slotNN.fslot (+ optional .png/.jpg).
    saveBtn.onClick = [this]
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || !fs->slotHasContent(selectedSlot))
            return;

        const juce::File dir = resolveSaveDirectory();
        if (!dir.isDirectory())
            return;

        // Build timestamp prefix
        const juce::Time now = juce::Time::getCurrentTime();
        const juce::String stamp = juce::String::formatted(
            "%04d%02d%02d-%02d%02d%02d",
            now.getYear(),
            now.getMonth() + 1,
            now.getDayOfMonth(),
            now.getHours(),
            now.getMinutes(),
            now.getSeconds());

        const juce::String base = stamp + "_slot"
                                  + juce::String(selectedSlot).paddedLeft('0', 2);

        const juce::File slotFile = dir.getChildFile(base + ".fslot");
        fs->saveSlotToFile(selectedSlot, slotFile);

        // Optional image export — controlled by user settings (luxSamplerExportImages).
        auto& apvts = processor.getAPVTS();
        const bool exportImg =
            apvts.getRawParameterValue("luxSamplerExportImages") != nullptr
            && apvts.getRawParameterValue("luxSamplerExportImages")->load() > 0.5f;

        if (exportImg)
        {
            bool asPng = true; // 0 = PNG, 1 = JPEG
            if (auto* p = apvts.getRawParameterValue("luxSamplerExportFormat"))
                asPng = (p->load() < 0.5f);

            const juce::String ext = asPng ? ".png" : ".jpg";
            const juce::File imgFile = dir.getChildFile(base + ext);
            fs->exportSlotImage(selectedSlot, imgFile, asPng);
        }
    };
    addAndMakeVisible(saveBtn);

    // ── LOAD button — file chooser, loads into selected slot ──────────────────
    loadBtn.onClick = [this]
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr) return;

        const juce::File startDir = resolveSaveDirectory();
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load slot (.fslot)",
            startDir,
            "*.fslot");

        const int flags = juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles;

        fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
        {
            const juce::File picked = fc.getResult();
            if (picked == juce::File()) return;
            auto* sampler = processor.getLuxSampler();
            if (sampler == nullptr) return;

            if (sampler->loadSlotFromFile(selectedSlot, picked))
            {
                timeline.markDirty();
                refreshSliderValues();
                refreshLoopButtons();
            }
        });
    };
    addAndMakeVisible(loadBtn);

    // ── Labels ────────────────────────────────────────────────────────────────
    for (auto* lbl : { &speedLabel, &loopLabel })
    {
        lbl->setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        lbl->setColour(juce::Label::textColourId, juce::Colour(0xffb0b0c0));
        lbl->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(lbl);
    }

    // NOTE: MIX (blend) slider removed. Live/sampler opacity is now managed from
    // the IMAGE tab's opacity controls (darken-blend, see ImagePageComponent).

    // ── Brightness lift slider ───────────────────────────────────────────────
    // 0% = normal, 100% = fully white (all pixels → 255, silence).
    brightnessLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    brightnessLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b0c0));
    brightnessLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(brightnessLabel);

    brightnessSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    brightnessSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                      Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    brightnessSlider.setRange(0.0, 100.0, 1.0);
    brightnessSlider.setTextValueSuffix("%");
    brightnessSlider.setValue(100.0, juce::dontSendNotification); // 100 = full image
    brightnessSlider.onValueChange = [this]
    {
        // Slider 100% = full image (lift=0), 0% = white silence (lift=1)
        if (auto* fs = processor.getLuxSampler())
            fs->setSlotBrightnessLift(selectedSlot,
                1.0f - static_cast<float>(brightnessSlider.getValue()) * 0.01f);
    };
    addAndMakeVisible(brightnessSlider);

    // ── Speed slider ──────────────────────────────────────────────────────────
    // Range 0.01–32.0×; skewed so that 1.0× sits at the physical centre.
    speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    speedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                 Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    // Step 0.001 gives 10× finer resolution than 0.01 — especially important
    // at slow playback rates (0.01–0.10×) where the skewed slider compresses
    // the physical space. Display is clamped to 2 decimal places.
    speedSlider.setRange(0.01, 32.0, 0.001);
    speedSlider.setNumDecimalPlacesToDisplay(2);
    speedSlider.setTextValueSuffix("x");
    speedSlider.setSkewFactorFromMidPoint(1.0); // 1.0× at slider centre
    speedSlider.setValue(1.0, juce::dontSendNotification);
    speedSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getLuxSampler())
            fs->setSlotSpeed(selectedSlot,
                             static_cast<float>(speedSlider.getValue()));
    };
    addAndMakeVisible(speedSlider);

    // ── Loop mode buttons ─────────────────────────────────────────────────────
    for (int k = 0; k < 4; ++k)
    {
        loopBtns[k].setButtonText(kLoopLabels[k]);
        loopBtns[k].onClick = [this, k] { applyLoopMode(static_cast<LoopMode>(k)); };
        addAndMakeVisible(loopBtns[k]);
    }
    refreshLoopButtons();

    // ── Resume mode toggle ────────────────────────────────────────────────────
    resumeToggle.setColour(juce::ToggleButton::textColourId,
                           juce::Colour(0xffb0b0c0));
    resumeToggle.onStateChange = [this]
    {
        if (auto* fs = processor.getLuxSampler())
            fs->setSlotResumeMode(selectedSlot,
                                   resumeToggle.getToggleState());
    };
    addAndMakeVisible(resumeToggle);

    // ── Fade curve type selector ─────────────────────────────────────────────
    fadeCurveLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    fadeCurveLabel.setColour(juce::Label::textColourId,
                             juce::Colour(0xff888899));
    fadeCurveLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(fadeCurveLabel);

    fadeCurveTypeBox.addItem("LIN", 1);
    fadeCurveTypeBox.addItem("EXP", 2);
    fadeCurveTypeBox.addItem("LOG", 3);
    fadeCurveTypeBox.addItem("S",   4);
    fadeCurveTypeBox.setSelectedId(1, juce::dontSendNotification);
    fadeCurveTypeBox.onChange = [this]
    {
        if (auto* fs = processor.getLuxSampler())
            fs->setSlotFadeCurveType(selectedSlot,
                static_cast<FadeCurveType>(fadeCurveTypeBox.getSelectedId() - 1));
    };
    addAndMakeVisible(fadeCurveTypeBox);

    // ── Fade curve power slider ──────────────────────────────────────────────
    fadePowerLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    fadePowerLabel.setColour(juce::Label::textColourId,
                             juce::Colour(0xff888899));
    fadePowerLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(fadePowerLabel);

    fadePowerSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    fadePowerSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                    Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    fadePowerSlider.setRange(0.1, 10.0, 0.01);
    fadePowerSlider.setNumDecimalPlacesToDisplay(2);
    fadePowerSlider.setSkewFactorFromMidPoint(1.0);
    fadePowerSlider.setValue(1.0, juce::dontSendNotification);
    fadePowerSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getLuxSampler())
            fs->setSlotFadeCurvePower(selectedSlot,
                static_cast<float>(fadePowerSlider.getValue()));
    };
    addAndMakeVisible(fadePowerSlider);

    // 30 ms (~33 Hz) — frequent enough so MIDI press/release pulses get
    // consumed with low latency (worst-case ~30 ms after the MIDI event).
    // UI repaint cost is negligible at this rate.
    startTimer(30);
}

SlotEditorComponent::~SlotEditorComponent()
{
    stopTimer();
}

void SlotEditorComponent::setSelectedSlot(int idx)
{
    selectedSlot = idx;
    // Mirror selected slot to the audio processor so RT-triggered
    // REC/PLAY/SAVE bindings act on the slot the user is looking at.
    processor.setSamplerSelectedSlot(selectedSlot);
    timeline.setSelectedSlot(selectedSlot);
    // Refresh UI state from new slot
    refreshSliderValues();
    refreshLoopButtons();
    repaint();
}

void SlotEditorComponent::refreshSliderValues()

{
    auto* fs = processor.getLuxSampler();
    if (fs == nullptr) return;

    // Start/End are no longer exposed as sliders — they are edited directly
    // on the timeline. Only Speed and Resume need refreshing here.
    speedSlider.setValue(
        static_cast<double>(fs->getSlotSpeed(selectedSlot)),
        juce::dontSendNotification);
    // Blend (MIX) removed — opacity managed from IMAGE tab.
    // Invert: slider shows image intensity (100=full, 0=white)
    brightnessSlider.setValue(
        (1.0 - static_cast<double>(fs->getSlotBrightnessLift(selectedSlot))) * 100.0,
        juce::dontSendNotification);
    resumeToggle.setToggleState(fs->getSlotResumeMode(selectedSlot),
                                juce::dontSendNotification);
    // Fade curve controls
    fadeCurveTypeBox.setSelectedId(
        static_cast<int>(fs->getSlotFadeCurveType(selectedSlot)) + 1,
        juce::dontSendNotification);
    fadePowerSlider.setValue(
        static_cast<double>(fs->getSlotFadeCurvePower(selectedSlot)),
        juce::dontSendNotification);
}

void SlotEditorComponent::refreshLoopButtons()
{
    auto* fs = processor.getLuxSampler();
    const int curMode = (fs != nullptr)
                        ? static_cast<int>(fs->getSlotLoopMode(selectedSlot))
                        : 1; // default LOOP

    for (int k = 0; k < 4; ++k)
    {
        const bool active = (k == curMode);
        loopBtns[k].setColour(juce::TextButton::buttonColourId,
                               active ? juce::Colour(0xff1a5a9a)
                                      : juce::Colour(0xff2a2a2a));
        loopBtns[k].setColour(juce::TextButton::textColourOffId,
                               active ? juce::Colours::white
                                      : juce::Colour(0xff888888));
    }
}

void SlotEditorComponent::applyLoopMode(LoopMode m)
{
    if (auto* fs = processor.getLuxSampler())
        fs->setSlotLoopMode(selectedSlot, m);
    refreshLoopButtons();
}

// ─────────────────────────────────────────────────────────────────────────────
// paint
//
// Full-width zone: title badge (top) then two columns side by side.
//   Left  (~63 %): REC/PLAY/CLEAR buttons + large timeline
//   Right (~37 %): Speed / Loop / Resume controls
// A subtle vertical separator is drawn between the two panels.
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::paint(juce::Graphics& g)
{
    const int W   = getWidth();
    const int H   = getHeight();
    const int pad = 4;
    const int gap = 6;

    // ── Background ───────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff1a1a2a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // ── Title badge (full width) ──────────────────────────────────────────────
    g.setColour(juce::Colour(0xff2a1a3a));
    g.fillRoundedRectangle(
        juce::Rectangle<float>(4.0f, 4.0f, (float)(W - 8), 22.0f), 3.0f);

    g.setColour(juce::Colour(0xffcc88ff));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText(juce::String("SLOT > ") + kNoteNamesEd[selectedSlot],
               juce::Rectangle<int>(8, 4, W - 16, 22),
               juce::Justification::centredLeft, false);

    // State indicator (right side of title badge)
    auto* fs = processor.getLuxSampler();
    const SlotState st = (fs != nullptr) ? fs->getSlotState(selectedSlot)
                                         : SlotState::IDLE;
    juce::String stateStr;
    juce::Colour stateCol = juce::Colour(0xff666666);
    switch (st)
    {
        case SlotState::RECORDING:
            stateStr = "* REC";
            stateCol = juce::Colour(0xffff4444);
            break;
        case SlotState::ARMED:
            stateStr = "ARM";
            stateCol = juce::Colour(0xffffcc00);
            break;
        case SlotState::PLAYING:
            stateStr = "PLAY";
            stateCol = juce::Colour(0xff44ff44);
            break;
        default:
            stateStr = (fs && fs->slotHasContent(selectedSlot)) ? "IDLE" : "EMPTY";
            break;
    }
    g.setColour(stateCol);
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    g.drawText(stateStr, juce::Rectangle<int>(8, 4, W - 16, 22),
               juce::Justification::centredRight, false);

    // ── Vertical separator between left and right panels ─────────────────────
    const int leftW  = (W - 3 * pad - gap) * 63 / 100;
    const int sepX   = pad + leftW + gap / 2;
    g.setColour(juce::Colour(0xff2a2a3a));
    g.fillRect(sepX, 30, 1, H - 34);

    // ── Right panel subtle background ─────────────────────────────────────────
    const int rightX = pad + leftW + gap + pad;
    g.setColour(juce::Colour(0xff141422));
    g.fillRoundedRectangle(
        juce::Rectangle<float>((float)rightX - 2.0f, 28.0f,
                                (float)(W - rightX - pad + 2), (float)(H - 32)),
        3.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// resized
//
// Two-column layout below the title badge (y=30 downward):
//
//   Left  (~63 %):
//     y=30 : [REC] [PLAY] [CLEAR]       (h=30)
//     y=64 : Timeline                   (fills remaining height)
//
//   Right (~37 %):
//     y=30 : Speed  label + slider      (rowH=27)
//     y=61 : Loop   label + 4 buttons   (rowH=27)
//     y=92 : Resume toggle              (h=26)
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::resized()
{
    const int W   = getWidth();
    const int H   = getHeight();
    const int pad = 4;
    const int gap = 6;

    // Column split
    const int leftW  = (W - 3 * pad - gap) * 63 / 100;
    const int rightW = W - 3 * pad - gap - leftW;
    const int leftX  = pad;
    const int rightX = leftX + leftW + gap + pad;

    // ── Left: REC / PLAY / CLEAR / SAVE / LOAD (y=30, 5 buttons in one row) ──
    {
        const int btnY   = 30;
        constexpr int btnH   = Sp3ctraTheme::kControlH;
        const int btnGap = Sp3ctraTheme::kGap;
        const int bW     = (leftW - 4 * btnGap) / 5;
        recBtn  .setBounds(leftX,                     btnY, bW, btnH);
        playBtn .setBounds(leftX + 1 * (bW + btnGap), btnY, bW, btnH);
        clearBtn.setBounds(leftX + 2 * (bW + btnGap), btnY, bW, btnH);
        saveBtn .setBounds(leftX + 3 * (bW + btnGap), btnY, bW, btnH);
        loadBtn .setBounds(leftX + 4 * (bW + btnGap), btnY, bW, btnH);
    }

    // ── Left: Timeline (y=64, fills remaining height) ─────────────────────────
    {
        const int tlY = 64;
        const int tlH = H - tlY - pad;
        timeline.setBounds(leftX, tlY, leftW, juce::jmax(40, tlH));
    }

    // ── Right panel controls ─────────────────────────────────────────────────
    constexpr int rowH = Sp3ctraTheme::kControlH; // unified control height
    const int step = rowH + 4;
    const int lW   = 46; // label column width
    const int ctrlX = rightX + lW + 4;
    const int ctrlW = rightW - lW - 4;
    int ry = 32; // slight top padding inside right panel

    // Brightness lift
    brightnessLabel .setBounds(rightX, ry, lW, rowH);
    brightnessSlider.setBounds(ctrlX,   ry, ctrlW, rowH);
    ry += step;

    // Speed
    speedLabel .setBounds(rightX, ry, lW, rowH);
    speedSlider.setBounds(ctrlX,   ry, ctrlW, rowH);
    ry += step;

    // Loop
    {
        const int bGap   = 3;
        const int availW = rightW - lW - 4;
        const int bW     = (availW - 3 * bGap) / 4;
        loopLabel.setBounds(rightX, ry, lW, rowH);
        for (int k = 0; k < 4; ++k)
            loopBtns[k].setBounds(ctrlX + k * (bW + bGap), ry, bW, rowH);
    }
    ry += step;

    // Resume toggle
    resumeToggle.setBounds(rightX, ry, rightW, 26);
    ry += step;

    // Fade curve type
    fadeCurveLabel  .setBounds(rightX, ry, lW, rowH);
    fadeCurveTypeBox.setBounds(ctrlX,  ry, ctrlW, rowH);
    ry += step;

    // Fade curve power
    fadePowerLabel .setBounds(rightX, ry, lW, rowH);
    fadePowerSlider.setBounds(ctrlX,  ry, ctrlW, rowH);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer callback (~5 Hz)
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::timerCallback()
{
    blinkOn = !blinkOn;

    // ── Consume MIDI-triggered REC / PLAY / SAVE pulses ──────────────────────
    // REC / PLAY now follow a momentary (press-and-hold) semantic:
    //   press   → start the action (only if not already running on this slot)
    //   release → stop  the action (only if currently running on this slot)
    // This avoids re-triggering the UI toggle in the wrong direction (which
    // would otherwise stop the action on release, or restart it on press if
    // it was already running).
    //
    // We rely on the LuxSampler's idempotent toggle behaviour: calling
    // uiToggleRecord() / uiPlaySlot() while in the matching state stops the
    // action, calling it while in any other state starts it.  We therefore
    // check the current slot state before invoking the toggle.
    auto* fs = processor.getLuxSampler();

    const bool recPressed   = processor.consumeSamplerRecPressed();
    const bool recReleased  = processor.consumeSamplerRecReleased();
    const bool playPressed  = processor.consumeSamplerPlayPressed();
    const bool playReleased = processor.consumeSamplerPlayReleased();

    if (fs != nullptr)
    {
        const SlotState midiSt = fs->getSlotState(selectedSlot);

        // REC press → start recording if not already in RECORDING state.
        if (recPressed && midiSt != SlotState::RECORDING && recBtn.onClick)
            recBtn.onClick();
        // REC release → stop recording if currently in RECORDING state.
        else if (recReleased && midiSt == SlotState::RECORDING && recBtn.onClick)
            recBtn.onClick();

        // Re-read state because REC may have just changed it.
        const SlotState midiSt2 = fs->getSlotState(selectedSlot);

        // PLAY press → start playback if not already in PLAYING state.
        if (playPressed && midiSt2 != SlotState::PLAYING && playBtn.onClick)
            playBtn.onClick();
        // PLAY release → stop playback if currently in PLAYING state.
        else if (playReleased && midiSt2 == SlotState::PLAYING && playBtn.onClick)
            playBtn.onClick();
    }

    if (processor.consumeSamplerSaveTrigger() && saveBtn.onClick)
        saveBtn.onClick();

    if (fs == nullptr) return;


    const SlotState st         = fs->getSlotState(selectedSlot);
    const bool      hasContent = fs->slotHasContent(selectedSlot);

    // Invalidate timeline thumbnail when recording stops
    if (st == SlotState::IDLE && hasContent)
        timeline.markDirty(); // markDirty is idempotent (NOP if already clean)

    // ── REC button ───────────────────────────────────────────────────────────
    switch (st)
    {
        case SlotState::RECORDING:
            recBtn.setButtonText("STOP");
            // Solid red (no blink) — visual stability is preferred over flashing.
            recBtn.setColour(juce::TextButton::buttonColourId,
                             juce::Colour(0xffcc2222));
            recBtn.setColour(juce::TextButton::textColourOffId,
                             juce::Colours::white);
            break;
        case SlotState::ARMED:
            recBtn.setButtonText("ARM");
            recBtn.setColour(juce::TextButton::buttonColourId,
                             juce::Colour(0xff7a3300));
            recBtn.setColour(juce::TextButton::textColourOffId,
                             juce::Colour(0xffffcc66));
            break;
        default:
            recBtn.setButtonText("REC");
            recBtn.setColour(juce::TextButton::buttonColourId,
                             juce::Colour(0xff3a3a3a));
            recBtn.setColour(juce::TextButton::textColourOffId,
                             juce::Colour(0xffcc6666));
            break;
    }

    // ── PLAY button ──────────────────────────────────────────────────────────
    const bool isPlaying = (st == SlotState::PLAYING);
    playBtn.setButtonText(isPlaying ? "STOP" : "PLAY");
    playBtn.setEnabled(hasContent || isPlaying);
    playBtn.setColour(juce::TextButton::buttonColourId,
                      isPlaying ? juce::Colour(0xff1a5a1a)
                                : juce::Colour(0xff3a3a3a));
    playBtn.setColour(juce::TextButton::textColourOffId,
                      isPlaying ? juce::Colour(0xff88ff88)
                                : juce::Colour(0xff888888));

    clearBtn.setEnabled(hasContent);

    // ── SAVE / LOAD buttons ──────────────────────────────────────────────────
    saveBtn.setEnabled(hasContent);
    saveBtn.setColour(juce::TextButton::buttonColourId,
                      juce::Colour(0xff2a3a4a));
    saveBtn.setColour(juce::TextButton::textColourOffId,
                      hasContent ? juce::Colour(0xff88ccff)
                                 : juce::Colour(0xff556677));
    // LOAD is always enabled — even an empty slot can be filled.
    loadBtn.setColour(juce::TextButton::buttonColourId,
                      juce::Colour(0xff2a3a4a));
    loadBtn.setColour(juce::TextButton::textColourOffId,
                      juce::Colour(0xff88ccff));

    repaint(); // refresh title state indicator
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveSaveDirectory — build the destination folder for SAVE.
// Order of resolution:
//   1. processor.getSamplerOutputDir() (user-configured Sampler Output Dir)
//   2. ~/Documents
// Creates the directory if it does not yet exist.
// ─────────────────────────────────────────────────────────────────────────────
juce::File SlotEditorComponent::resolveSaveDirectory() const
{
    juce::File dir;

    const juce::String configured = processor.getSamplerOutputDir();
    if (configured.isNotEmpty())
        dir = juce::File(configured);

    if (dir == juce::File())
        dir = juce::File::getSpecialLocation(
                  juce::File::userDocumentsDirectory);

    if (!dir.isDirectory())
        dir.createDirectory();

    return dir;
}
