#include "SlotEditorComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// Loop-mode buttons are now pictograms (see LoopModeButton). Order matches the
// LoopMode enum: NONE / LOOP / INVERSE / PINGPONG.
static const LoopModeButton::Glyph kLoopGlyphs[4] = {
    LoopModeButton::Glyph::None,
    LoopModeButton::Glyph::Loop,
    LoopModeButton::Glyph::Inverse,
    LoopModeButton::Glyph::PingPong
};
static const char* kLoopTips[4] = {
    "No loop (play once, then stop)",
    "Loop forward",
    "Inverse (loop backward)",
    "Ping-pong (bounce forward / backward)"
};
static const char* kNoteNamesEd[LuxSamplerConstants::NUM_SLOTS] = {
    "C1","C#1","D1","D#1","E1","F1","F#1","G1","G#1","A1","A#1","B1"
};

SlotEditorComponent::SlotEditorComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      spectralEditor(proc) // merged image + time-handles + EQ editor
{
    // ── Merged editor ─────────────────────────────────────────────────────────
    // Start/End/fade handles are dragged directly on the editor and update
    // LuxSampler atomics in SlotSpectralEditorComponent::mouseDrag().
    addAndMakeVisible(spectralEditor);

    // ── Action buttons ────────────────────────────────────────────────────────
    recBtn.onClick = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
        {
            fs->uiToggleRecord(selectedSlot);
            spectralEditor.markDirty(); // image may have changed after record
        }
    };
    addAndMakeVisible(recBtn);

    playBtn.onClick = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
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
        if (auto* fs = processor.getSampler(samplerIndex_))
        {
            fs->uiClearSlot(selectedSlot);
            // CLEAR reset the EQ AND the edit handles (start/end/fades/floor) —
            // refresh every control so the UI reflects the fresh slot.
            refreshSliderValues();     // also calls refreshFreqCurve() + markDirty
            spectralEditor.markDirty();
        }
    };
    addAndMakeVisible(clearBtn);

    // ── CROP button — destructive trim to the current [start, end] region ─────
    // Cuts everything outside the green/orange bounds, then resets the bounds to
    // full so the kept part fills the timeline.
    cropBtn.onClick = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
        {
            fs->cropSlotToBounds(selectedSlot);
            spectralEditor.markDirty();   // image + bounds changed
            refreshSliderValues();
        }
    };
    addAndMakeVisible(cropBtn);

    // ── SAVE button — direct write (no dialog) ────────────────────────────────
    // Filename pattern: YYYYMMDD-HHMMSS_slotNN.fslot (+ optional .png/.jpg).
    saveBtn.onClick = [this]
    {
        auto* fs = processor.getSampler(samplerIndex_);
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
        auto* fs = processor.getSampler(samplerIndex_);
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
            auto* sampler = processor.getSampler(samplerIndex_);
            if (sampler == nullptr) return;

            if (sampler->loadSlotFromFile(selectedSlot, picked))
            {
                spectralEditor.markDirty();
                refreshSliderValues();
                refreshLoopButtons();
            }
        });
    };
    addAndMakeVisible(loadBtn);

    // ── Labels ────────────────────────────────────────────────────────────────
    for (auto* lbl : { &speedLabel, &loopLabel, &loopXfLabel })
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
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotBrightnessLift(selectedSlot,
                1.0f - static_cast<float>(brightnessSlider.getValue()) * 0.01f);
        spectralEditor.markDirty();
    };
    addAndMakeVisible(brightnessSlider);

    // ── Pre-EQ material floor slider (0% = off … 100% = total white mask) ─────
    floorLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    floorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb0b0c0));
    floorLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(floorLabel);

    floorSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    floorSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    floorSlider.setRange(0.0, 100.0, 1.0);
    floorSlider.setTextValueSuffix("%");
    floorSlider.setValue(0.0, juce::dontSendNotification);
    floorSlider.setTooltip("Remove material below this level BEFORE the EQ "
                           "(avoids black bands when boosting; 100% = full mask)");
    floorSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotEqFloor(selectedSlot,
                static_cast<float>(floorSlider.getValue()) * 0.01f);
        spectralEditor.markDirty();   // preview the floor on the image
    };
    addAndMakeVisible(floorSlider);

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
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotSpeed(selectedSlot,
                             static_cast<float>(speedSlider.getValue()));
    };
    addAndMakeVisible(speedSlider);

    // ── Loop mode buttons (pictograms) ────────────────────────────────────────
    for (int k = 0; k < 4; ++k)
    {
        loopBtns[k].setGlyph(kLoopGlyphs[k]);
        loopBtns[k].setTooltip(kLoopTips[k]);
        loopBtns[k].onClick = [this, k] { applyLoopMode(static_cast<LoopMode>(k)); };
        addAndMakeVisible(loopBtns[k]);
    }
    refreshLoopButtons();

    // ── Loop crossfade (overlap) slider — 0..50 % of the loop zone ────────────
    loopXfSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    loopXfSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                 Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    loopXfSlider.setRange(0.0, 50.0, 1.0);
    loopXfSlider.setTextValueSuffix("%");
    loopXfSlider.setValue(0.0, juce::dontSendNotification);
    loopXfSlider.setTooltip("Loop crossfade: fades tail into head at the wrap "
                            "(LOOP / INVERSE)");
    loopXfSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotLoopOverlap(selectedSlot,
                static_cast<float>(loopXfSlider.getValue()) * 0.01f);
    };
    addAndMakeVisible(loopXfSlider);

    // ── Image EQ (SCORE-style ±24 dB) — separate panel below the image ────────
    eqEditor.onChange = [this]
    {
        if (suppressEqPush_) return;               // silent refresh, don't write back
        if (auto* fs = processor.getSampler(samplerIndex_))
        {
            fs->setSlotEq(selectedSlot, eqEditor.encodeState());
            spectralEditor.markDirty();            // preview the EQ on the image
        }
    };
    addAndMakeVisible(eqEditor);

    // ── Resume mode toggle ────────────────────────────────────────────────────
    resumeToggle.setColour(juce::ToggleButton::textColourId,
                           juce::Colour(0xffb0b0c0));
    resumeToggle.onStateChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotResumeMode(selectedSlot,
                                   resumeToggle.getToggleState());
    };
    addAndMakeVisible(resumeToggle);

    // ── Overdub / extend toggle (engine-wide) ─────────────────────────────────
    overdubToggle.setColour(juce::ToggleButton::textColourId,
                            juce::Colour(0xffcc88ff)); // SAMPLER purple identity
    overdubToggle.onStateChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setOverdubMode(overdubToggle.getToggleState());
    };
    addAndMakeVisible(overdubToggle);

    // ── Per-fade curve controls (independent attack / decay) ──────────────────
    for (auto* lbl : { &fadeInLabel, &fadeOutLabel })
    {
        lbl->setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        lbl->setColour(juce::Label::textColourId, juce::Colour(0xff888899));
        lbl->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(lbl);
    }
    fillCurveBox(fadeInCurveBox);
    fillCurveBox(fadeOutCurveBox);

    fadeInCurveBox.onChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotAttackCurveType(selectedSlot,
                static_cast<FadeCurveType>(fadeInCurveBox.getSelectedId() - 1));
        spectralEditor.markDirty();
    };
    fadeOutCurveBox.onChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotDecayCurveType(selectedSlot,
                static_cast<FadeCurveType>(fadeOutCurveBox.getSelectedId() - 1));
        spectralEditor.markDirty();
    };
    addAndMakeVisible(fadeInCurveBox);
    addAndMakeVisible(fadeOutCurveBox);

    for (auto* s : { &fadeInPowerSlider, &fadeOutPowerSlider })
    {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false,
                           Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
        s->setRange(0.1, 10.0, 0.01);
        s->setNumDecimalPlacesToDisplay(2);
        s->setSkewFactorFromMidPoint(1.0);
        s->setValue(1.0, juce::dontSendNotification);
        addAndMakeVisible(s);
    }
    fadeInPowerSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotAttackCurvePower(selectedSlot,
                static_cast<float>(fadeInPowerSlider.getValue()));
        spectralEditor.markDirty();
    };
    fadeOutPowerSlider.onValueChange = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            fs->setSlotDecayCurvePower(selectedSlot,
                static_cast<float>(fadeOutPowerSlider.getValue()));
        spectralEditor.markDirty();
    };

    // Purge stale MIDI pulses latched while NO editor was open: processBlock
    // keeps setting them, nobody consumes them, and acting on a press latched
    // minutes ago would start a phantom recording the moment the editor opens.
    // Both engines: this editor rebinds A/B and either may hold stale pulses.
    for (int e = 0; e < 2; ++e)
    {
        (void) processor.consumeSamplerRecPressed  (e);
        (void) processor.consumeSamplerRecReleased (e);
        (void) processor.consumeSamplerPlayPressed (e);
        (void) processor.consumeSamplerPlayReleased(e);
        (void) processor.consumeSamplerSaveTrigger (e);
    }

    // 30 ms (~33 Hz) — frequent enough so MIDI press/release pulses get
    // consumed with low latency (worst-case ~30 ms after the MIDI event).
    // UI repaint cost is negligible at this rate.
    startTimer(30);
}

SlotEditorComponent::~SlotEditorComponent()
{
    stopTimer();
}

void SlotEditorComponent::setSamplerIndex(int i)
{
    samplerIndex_ = i;
    spectralEditor.setSamplerIndex(i);
    // Purge pulses the newly-bound engine latched while it had no editor —
    // acting on a stale press would start a phantom recording right away.
    (void) processor.consumeSamplerRecPressed  (samplerIndex_);
    (void) processor.consumeSamplerRecReleased (samplerIndex_);
    (void) processor.consumeSamplerPlayPressed (samplerIndex_);
    (void) processor.consumeSamplerPlayReleased(samplerIndex_);
    (void) processor.consumeSamplerSaveTrigger (samplerIndex_);
    setSelectedSlot(selectedSlot);   // refresh controls from the new engine
}

void SlotEditorComponent::setSelectedSlot(int idx)
{
    selectedSlot = idx;
    // Mirror selected slot to the audio processor so RT-triggered
    // REC/PLAY/SAVE bindings act on the slot the user is looking at.
    processor.setSamplerSelectedSlot(selectedSlot);
    spectralEditor.setSelectedSlot(selectedSlot);
    // Refresh UI state from new slot
    refreshSliderValues();
    refreshLoopButtons();
    // Re-seed the content latch for the new slot so switching slots never fires
    // the "external CLEAR" freq-curve refresh spuriously (setSelectedSlot →
    // refreshSliderValues already reloaded the curve).
    if (auto* fs = processor.getSampler(samplerIndex_))
        prevHasContent_ = fs->slotHasContent(selectedSlot);
    repaint();
}

void SlotEditorComponent::refreshSliderValues()

{
    auto* fs = processor.getSampler(samplerIndex_);
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
    floorSlider.setValue(
        static_cast<double>(fs->getSlotEqFloor(selectedSlot)) * 100.0,
        juce::dontSendNotification);
    resumeToggle.setToggleState(fs->getSlotResumeMode(selectedSlot),
                                juce::dontSendNotification);
    // Overdub is engine-wide (not per-slot) — mirror the engine flag.
    overdubToggle.setToggleState(fs->getOverdubMode(),
                                 juce::dontSendNotification);
    // Per-fade curve controls (independent attack / decay).
    fadeInCurveBox.setSelectedId(
        static_cast<int>(fs->getSlotAttackCurveType(selectedSlot)) + 1,
        juce::dontSendNotification);
    fadeInPowerSlider.setValue(
        static_cast<double>(fs->getSlotAttackCurvePower(selectedSlot)),
        juce::dontSendNotification);
    fadeOutCurveBox.setSelectedId(
        static_cast<int>(fs->getSlotDecayCurveType(selectedSlot)) + 1,
        juce::dontSendNotification);
    fadeOutPowerSlider.setValue(
        static_cast<double>(fs->getSlotDecayCurvePower(selectedSlot)),
        juce::dontSendNotification);
    loopXfSlider.setValue(
        static_cast<double>(fs->getSlotLoopOverlap(selectedSlot)) * 100.0,
        juce::dontSendNotification);
    refreshFreqCurve();
}

void SlotEditorComponent::refreshFreqCurve()
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;
    // Load the slot's EQ into eqEditor WITHOUT writing it back to the sampler.
    suppressEqPush_ = true;
    const juce::String enc = fs->getSlotEq(selectedSlot);
    if (! eqEditor.decodeState(enc))
        eqEditor.reset();          // flat when the slot has no EQ
    suppressEqPush_ = false;
    // Preview EQ + fades on the authentic image.
    spectralEditor.markDirty();
}

void SlotEditorComponent::fillCurveBox(juce::ComboBox& box)
{
    box.addItem("LIN", 1);
    box.addItem("EXP", 2);
    box.addItem("LOG", 3);
    box.addItem("S",   4);
    box.setSelectedId(1, juce::dontSendNotification);
}

void SlotEditorComponent::refreshLoopButtons()
{
    auto* fs = processor.getSampler(samplerIndex_);
    const int curMode = (fs != nullptr)
                        ? static_cast<int>(fs->getSlotLoopMode(selectedSlot))
                        : 1; // default LOOP

    for (int k = 0; k < 4; ++k)
        loopBtns[k].setActive(k == curMode);
}

void SlotEditorComponent::applyLoopMode(LoopMode m)
{
    if (auto* fs = processor.getSampler(samplerIndex_))
        fs->setSlotLoopMode(selectedSlot, m);
    refreshLoopButtons();
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared layout metrics — keeps paint() and resized() in lock-step.
// Parameters occupy two columns of 5 rows below the title badge; the merged
// image + time + EQ editor fills everything under them.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int kEdPad      = 4;
static constexpr int kEdGap      = 6;
static constexpr int kEdColGap   = 8;
static constexpr int kEdTitleH   = 22;
static constexpr int kEdParamTop = 30;   // first param row Y
static constexpr int kEdRows     = 6;    // rows per column (incl. button row)

static int edStep()        { return Sp3ctraTheme::kControlH + 4; }
static int edParamBottom() { return kEdParamTop + kEdRows * edStep(); }

// ─────────────────────────────────────────────────────────────────────────────
// paint — title badge + two-column separator. The editor paints its own zone.
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::paint(juce::Graphics& g)
{
    const int W = getWidth();

    // ── Background ───────────────────────────────────────────────────────────
    g.setColour(juce::Colour(0xff1a1a2a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // ── Title badge (full width) ──────────────────────────────────────────────
    g.setColour(juce::Colour(0xff2a1a3a));
    g.fillRoundedRectangle(
        juce::Rectangle<float>(4.0f, 4.0f, (float)(W - 8), (float)kEdTitleH), 3.0f);

    g.setColour(juce::Colour(0xffcc88ff));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText(juce::String("SLOT > ") + kNoteNamesEd[selectedSlot],
               juce::Rectangle<int>(8, 4, W - 16, kEdTitleH),
               juce::Justification::centredLeft, false);

    // State indicator (right side of title badge)
    auto* fs = processor.getSampler(samplerIndex_);
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
    g.drawText(stateStr, juce::Rectangle<int>(8, 4, W - 16, kEdTitleH),
               juce::Justification::centredRight, false);

    // ── Vertical separator between the two parameter columns ──────────────────
    const int sepX = kEdPad + (W - 2 * kEdPad - kEdColGap) / 2 + kEdColGap / 2;
    g.setColour(juce::Colour(0xff2a2a3a));
    g.fillRect(sepX, kEdParamTop, 1, edParamBottom() - kEdParamTop);
}

// ─────────────────────────────────────────────────────────────────────────────
// resized
//
// Two parameter columns below the title badge, then the merged editor:
//   Left column  : [REC][PLAY][CLEAR] · Speed · Loop · Loop XF · IMG
//   Right column : [CROP][SAVE][LOAD] · Resume · Overdub · Curve · Power
//   Bottom       : SlotSpectralEditorComponent (fills the remaining height)
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::resized()
{
    const int W = getWidth();
    const int H = getHeight();

    const int colW  = (W - 2 * kEdPad - kEdColGap) / 2;
    const int leftX = kEdPad;
    const int rightX = kEdPad + colW + kEdColGap;

    const int rowH = Sp3ctraTheme::kControlH;
    const int step = edStep();
    const int lW   = 46; // label column width
    const int btnGap = Sp3ctraTheme::kGap;

    // Helper: a 3-button row spanning a column.
    const auto layoutButtonRow = [&](int x, int y,
                                     juce::Button& a, juce::Button& b, juce::Button& c)
    {
        const int bW = (colW - 2 * btnGap) / 3;
        a.setBounds(x,                     y, bW, rowH);
        b.setBounds(x + 1 * (bW + btnGap), y, bW, rowH);
        c.setBounds(x + 2 * (bW + btnGap), y, colW - 2 * (bW + btnGap), rowH);
    };

    // ── Left column ───────────────────────────────────────────────────────────
    {
        const int ctrlX = leftX + lW + 4;
        const int ctrlW = colW - lW - 4;
        int ry = kEdParamTop;

        layoutButtonRow(leftX, ry, recBtn, playBtn, clearBtn);
        ry += step;

        speedLabel .setBounds(leftX, ry, lW, rowH);
        speedSlider.setBounds(ctrlX, ry, ctrlW, rowH);
        ry += step;

        {
            const int bGap = 3;
            const int bW   = (ctrlW - 3 * bGap) / 4;
            loopLabel.setBounds(leftX, ry, lW, rowH);
            for (int k = 0; k < 4; ++k)
                loopBtns[k].setBounds(ctrlX + k * (bW + bGap), ry, bW, rowH);
        }
        ry += step;

        loopXfLabel .setBounds(leftX, ry, lW, rowH);
        loopXfSlider.setBounds(ctrlX, ry, ctrlW, rowH);
        ry += step;

        brightnessLabel .setBounds(leftX, ry, lW, rowH);
        brightnessSlider.setBounds(ctrlX, ry, ctrlW, rowH);
        ry += step;

        floorLabel .setBounds(leftX, ry, lW, rowH);
        floorSlider.setBounds(ctrlX, ry, ctrlW, rowH);
    }

    // ── Right column ──────────────────────────────────────────────────────────
    {
        const int ctrlX = rightX + lW + 4;
        const int ctrlW = colW - lW - 4;
        int ry = kEdParamTop;

        layoutButtonRow(rightX, ry, cropBtn, saveBtn, loadBtn);
        ry += step;

        resumeToggle.setBounds(rightX, ry, colW, rowH);
        ry += step;

        overdubToggle.setBounds(rightX, ry, colW, rowH);
        ry += step;

        // Per-fade rows: label · curve combo · power slider.
        const int curveW = 74;
        const int powerX = ctrlX + curveW + 4;
        const int powerW = ctrlW - curveW - 4;
        fadeInLabel      .setBounds(rightX, ry, lW, rowH);
        fadeInCurveBox   .setBounds(ctrlX,  ry, curveW, rowH);
        fadeInPowerSlider.setBounds(powerX, ry, powerW, rowH);
        ry += step;

        fadeOutLabel      .setBounds(rightX, ry, lW, rowH);
        fadeOutCurveBox   .setBounds(ctrlX,  ry, curveW, rowH);
        fadeOutPowerSlider.setBounds(powerX, ry, powerW, rowH);
    }

    // ── Image editor (middle) + SCORE-style EQ panel (bottom) ─────────────────
    const int edY  = edParamBottom() + kEdGap;
    const int eqH  = juce::jmin(ScoreEqComponent::kPreferredH, (H - edY) / 2);
    const int eqY  = H - kEdPad - eqH;
    const int imgH = juce::jmax(60, eqY - kEdGap - edY);
    spectralEditor.setBounds(kEdPad, edY, W - 2 * kEdPad, imgH);
    eqEditor      .setBounds(kEdPad, eqY, W - 2 * kEdPad, eqH);
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
    auto* fs = processor.getSampler(samplerIndex_);

    const bool recPressed   = processor.consumeSamplerRecPressed  (samplerIndex_);
    const bool recReleased  = processor.consumeSamplerRecReleased (samplerIndex_);
    const bool playPressed  = processor.consumeSamplerPlayPressed (samplerIndex_);
    const bool playReleased = processor.consumeSamplerPlayReleased(samplerIndex_);

    if (fs != nullptr)
    {
        const SlotState midiSt = fs->getSlotState(selectedSlot);

        // REC press → start recording if not already in RECORDING state.
        if (recPressed && midiSt != SlotState::RECORDING && recBtn.onClick)
            recBtn.onClick();
        // REC release → stop recording if currently in RECORDING state.
        // Sequential `if` (NOT else-if): press and release often land in the
        // SAME timer tick — the release must still close the take, otherwise a
        // quick tap leaves the recording running (momentary semantic violated).
        if (recReleased && recBtn.onClick
            && fs->getSlotState(selectedSlot) == SlotState::RECORDING)
            recBtn.onClick();

        // Re-read state because REC may have just changed it.
        const SlotState midiSt2 = fs->getSlotState(selectedSlot);

        // PLAY press → start playback if not already in PLAYING state.
        if (playPressed && midiSt2 != SlotState::PLAYING && playBtn.onClick)
            playBtn.onClick();
        // PLAY release → stop playback if currently in PLAYING state (same
        // sequential handling as REC above).
        if (playReleased && playBtn.onClick
            && fs->getSlotState(selectedSlot) == SlotState::PLAYING)
            playBtn.onClick();
    }

    if (processor.consumeSamplerSaveTrigger(samplerIndex_) && saveBtn.onClick)
        saveBtn.onClick();

    if (fs == nullptr) return;


    const SlotState st         = fs->getSlotState(selectedSlot);
    const bool      hasContent = fs->slotHasContent(selectedSlot);

    // Rebuild the authentic-image backdrop when recording stops
    if (st == SlotState::IDLE && hasContent)
        spectralEditor.markDirty(); // markDirty is idempotent (NOP if already clean)

    // Refresh the spectral-curve backdrop once when a recording finishes.
    const bool nowRecording = (st == SlotState::RECORDING);
    if (prevRecording_ && !nowRecording)
        refreshFreqCurve();
    prevRecording_ = nowRecording;

    // A CLEAR from elsewhere (slot grid, setup panel) resets this slot's EQ AND
    // edit handles — mirror the whole state when the slot transitions to empty.
    if (prevHasContent_ && !hasContent)
        refreshSliderValues();
    prevHasContent_ = hasContent;

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

    // ── CROP button ──────────────────────────────────────────────────────────
    // Only meaningful when there is content AND the bounds select a sub-region.
    const bool canCrop = hasContent
        && (fs->getSlotStartFrac(selectedSlot) > 0.001f
            || fs->getSlotEndFrac(selectedSlot) < 0.999f);
    cropBtn.setEnabled(canCrop);
    cropBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a2a4a));
    cropBtn.setColour(juce::TextButton::textColourOffId,
                      canCrop ? juce::Colour(0xffcc88ff) : juce::Colour(0xff665577));

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
