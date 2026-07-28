#include "SlotEditorComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../Sp3ctraDialog.h"     // destructive-action confirmations
#include "../licensing/ActivationDialog.h"
#include "SamplerMidiTargets.h"   // synthetic target ids for MIDI-Learn

// Loop row = THREE composable pictogram toggles (forward / backward / repeat),
// combined into the persisted LoopMode enum by composeLoopMode(). Both arrows
// lit = round trip; + repeat = infinite ping-pong.
static const LoopModeButton::Glyph kLoopGlyphs[3] = {
    LoopModeButton::Glyph::None,       // straight → arrow (play forward)
    LoopModeButton::Glyph::ArrowLeft,  // mirrored ← arrow (play backward)
    LoopModeButton::Glyph::Loop        // racetrack (repeat)
};
static const char* kLoopTips[3] = {
    "Play forward (left to right)",
    "Play backward (right to left)",
    "Repeat - both arrows lit = infinite ping-pong"
};
// (Note names removed — banks are numbered, no more note addressing.)

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
    // Momentary mode: press starts recording, release stops it. uiToggleRecord
    // flips state, so gate on the current state (mirrors the MIDI drain).
    recBtn.onPress = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            if (fs->getSlotState(selectedSlot) != SlotState::RECORDING)
            {
                fs->uiToggleRecord(selectedSlot);
                spectralEditor.markDirty();
            }
    };
    recBtn.onRelease = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            if (fs->getSlotState(selectedSlot) == SlotState::RECORDING)
            {
                fs->uiToggleRecord(selectedSlot);
                spectralEditor.markDirty();
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
    // Momentary mode: press starts playback (+ arms the transport), release stops.
    playBtn.onPress = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            if (fs->getSlotState(selectedSlot) != SlotState::PLAYING)
            {
                fs->uiPlaySlot(selectedSlot);
                if (auto* p = processor.getAPVTS().getParameter("samplerFreezeMode"))
                    p->setValueNotifyingHost(0.0f); // 0 = PLAY
            }
    };
    playBtn.onRelease = [this]
    {
        if (auto* fs = processor.getSampler(samplerIndex_))
            if (fs->getSlotState(selectedSlot) == SlotState::PLAYING)
                fs->uiPlaySlot(selectedSlot);
    };
    addAndMakeVisible(playBtn);

    clearBtn.onClick = [this]
    {
        auto* fs = processor.getSampler(samplerIndex_);
        if (fs == nullptr) return;
        if (! fs->slotHasContent(selectedSlot))
            return;   // nothing to lose — no dialog
        Sp3ctraDialog::showConfirm(
            this, "Clear bank",
            ("Clear bank " + juce::String(selectedSlot + 1)
             + "? The recorded audio is discarded.").toRawUTF8(),
            "Clear", "Cancel",
            [safe = juce::Component::SafePointer<SlotEditorComponent>(this)](bool ok)
            {
                if (! ok) return;
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                if (auto* s = self->processor.getSampler(self->samplerIndex_))
                {
                    s->uiClearSlot(self->selectedSlot);
                    // CLEAR reset the EQ AND the edit handles — refresh every
                    // control so the UI reflects the fresh slot.
                    self->refreshSliderValues();   // + refreshFreqCurve + markDirty
                    self->spectralEditor.markDirty();
                    self->processor.sessions()->markBanksDirty();
                }
            });
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
            // Crop rewrites the recorded frames — persist the banks.
            processor.sessions()->markBanksDirty();
        }
    };
    addAndMakeVisible(cropBtn);

    // ── SAVE button — direct write (no dialog) ────────────────────────────────
    // Filename pattern: YYYYMMDD-HHMMSS_slotNN.fslot (+ optional .png/.jpg).
    // Shared with the MIDI-mapped SAVE trigger via saveSlotToDisk().
    saveBtn.onClick = [this] { saveSlotToDisk(selectedSlot); };
    addAndMakeVisible(saveBtn);

    // ── LOAD button — file chooser, loads into selected slot ──────────────────
    // Accepts BOTH the app's own .fslot format and plain images: a picture
    // becomes the bank's spectral content (one image row per frame, 5 s span),
    // re-rotatable afterwards from the bank tile's ↺ ↻ arrows.
    loadBtn.onClick = [this]
    {
        auto* fs = processor.getSampler(samplerIndex_);
        if (fs == nullptr) return;

        // Import: seed from the last slot-import directory used.
        const juce::File startDir = processor.sessions()->startDirFor(
            PathKeys::slotImport,
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load slot (.fslot) or image",
            startDir,
            "*.fslot;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tiff;*.tif");

        const int flags = juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles;

        fileChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
        {
            const juce::File picked = fc.getResult();
            if (picked == juce::File()) return;
            auto* sampler = processor.getSampler(samplerIndex_);
            if (sampler == nullptr) return;

            processor.sessions()->rememberDirFor(PathKeys::slotImport, picked);
            const bool ok = picked.hasFileExtension("fslot")
                ? sampler->loadSlotFromFile(selectedSlot, picked)
                : sampler->loadSlotFromImageFile(selectedSlot, picked, 0);
            if (ok)
            {
                spectralEditor.markDirty();
                refreshSliderValues();
                refreshLoopButtons();
                // Imported audio is session content — persist the banks.
                processor.sessions()->markBanksDirty();
            }
        });
    };
    addAndMakeVisible(loadBtn);

    // ↺ / ↻ in the spectral view rewrote the frames — mirror the LOAD refresh
    // (the spectral editor already marked itself dirty).
    spectralEditor.onContentRotated = [this]
    {
        refreshSliderValues();
        refreshLoopButtons();
    };

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

    // (IMG brightness-lift slider removed — the bank level fader in the grid's
    //  per-bank mixer drives the same engine param, inverted.)

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
    // Range 0–32.0×; skewed so that 1.0× sits at the physical centre.
    // 0 freezes the play head (the current frame sustains as a drone) — move
    // it by dragging the playhead on the timeline while the bank plays.
    speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    speedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                 Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    // Step 0.001 gives 10× finer resolution than 0.01 — especially important
    // at slow playback rates (0.01–0.10×) where the skewed slider compresses
    // the physical space. Display is clamped to 2 decimal places.
    speedSlider.setRange(0.0, 32.0, 0.001);
    speedSlider.setTooltip("Playback speed; 0 freezes the play head (drag it "
                           "on the timeline while playing)");
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

    // ── Loop toggles (pictograms: → / ← / repeat) ─────────────────────────────
    // Each click flips ONE bit of the current mode; the last lit direction
    // cannot be turned off (the click is ignored — no invalid combo).
    for (int k = 0; k < 3; ++k)
    {
        loopBtns[k].setGlyph(kLoopGlyphs[k]);
        loopBtns[k].setTooltip(kLoopTips[k]);
        loopBtns[k].onClick = [this, k]
        {
            auto* fs = processor.getSampler(samplerIndex_);
            if (fs == nullptr) return;
            bool f, b, r;
            decomposeLoopMode(fs->getSlotLoopMode(selectedSlot), f, b, r);
            if      (k == 0) f = !f;
            else if (k == 1) b = !b;
            else             r = !r;
            if (!f && !b) return;   // keep at least one direction lit
            applyLoopMode(composeLoopMode(f, b, r));
        };
        addAndMakeVisible(loopBtns[k]);
    }
    refreshLoopButtons();

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

    // ── Param boxes — two-row strip under the image view ──────────────────────
    // Crop bounds + fade width/type/power chips. Each box edits ONE
    // SamplerMidiTargets Kind through the SAME read/apply path a mapped MIDI CC
    // uses, always addressing the CURRENT (engine, slot) — no rebind needed.
    {
        using K = SamplerMidiTargets::Kind;
        const auto pct = [](float n)
        { return juce::String(juce::roundToInt(n * 100.0f)) + "%"; };
        const auto pow2 = [](float n)
        { return juce::String(SamplerMidiTargets::powerRange().convertFrom0to1(n), 2); };
        const auto curve = [](float n)
        {
            static const char* kNames[] = { "LIN", "EXP", "LOG", "S" };
            return juce::String(kNames[juce::jlimit(0, kNumFadeCurveTypes - 1,
                                                    (int) std::lround(n * 3.0f))]);
        };

        auto setup = [this](SamplerValueBox& b, K kind,
                            std::function<juce::String(float)> fmt)
        {
            b.readNorm = [this, kind]
            {
                auto* fs = processor.getSampler(samplerIndex_);
                return fs != nullptr
                     ? SamplerMidiTargets::read(*fs, selectedSlot, kind) : 0.0f;
            };
            b.applyNorm = [this, kind](float n)
            {
                if (auto* fs = processor.getSampler(samplerIndex_))
                {
                    SamplerMidiTargets::apply(*fs, selectedSlot, kind, n);
                    spectralEditor.markDirty();   // curves/crop live on the image
                    refreshParamBoxes();          // crop chips are coupled (min span)
                }
            };
            b.format = std::move(fmt);
            addAndMakeVisible(b);
        };
        setup(cropStartBox_,   K::CropStart,   pct);
        setup(cropEndBox_,     K::CropEnd,     pct);
        setup(fadeInLenBox_,   K::FadeInLen,   pct);
        setup(fadeInTypeBox_,  K::FadeInType,  curve);
        setup(fadeInPowBox_,   K::FadeInPow,   pow2);
        setup(fadeOutLenBox_,  K::FadeOutLen,  pct);
        setup(fadeOutTypeBox_, K::FadeOutType, curve);
        setup(fadeOutPowBox_,  K::FadeOutPow,  pow2);
        fadeInTypeBox_ .setChoices({ "LIN", "EXP", "LOG", "S" });
        fadeOutTypeBox_.setChoices({ "LIN", "EXP", "LOG", "S" });
    }

    // Handles dragged on the image (crop bars / fades) → keep the chips live.
    spectralEditor.onFadeChanged = [this] { refreshParamBoxes(); };

    // Purge stale MIDI action pulses latched while NO editor was open: the MIDI
    // engine keeps latching them, nobody drains them, and acting on a press
    // latched minutes ago would start a phantom recording the moment the editor
    // opens. Every engine × every slot.
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        for (int s = 0; s < LuxSamplerConstants::NUM_SLOTS; ++s)
        {
            (void) processor.consumeSmpRecPressed  (e, s);
            (void) processor.consumeSmpRecReleased (e, s);
            (void) processor.consumeSmpPlayPressed (e, s);
            (void) processor.consumeSmpPlayReleased(e, s);
            (void) processor.consumeSmpSaveTrigger (e, s);
        }
    lastValueTouchGen_ = processor.smpValueTouchGen();

    // Bind right-click MIDI-Learn to every play control / action button (targets
    // engine A · slot 0 until the first setSelectedSlot/setSamplerIndex).
    rebindMidiLearn();

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
    for (int s = 0; s < LuxSamplerConstants::NUM_SLOTS; ++s)
    {
        (void) processor.consumeSmpRecPressed  (samplerIndex_, s);
        (void) processor.consumeSmpRecReleased (samplerIndex_, s);
        (void) processor.consumeSmpPlayPressed (samplerIndex_, s);
        (void) processor.consumeSmpPlayReleased(samplerIndex_, s);
        (void) processor.consumeSmpSaveTrigger (samplerIndex_, s);
    }
    setSelectedSlot(selectedSlot);   // refresh controls + rebind MIDI-Learn
}

void SlotEditorComponent::setSelectedSlot(int idx)
{
    selectedSlot = idx;
    // Mirror selected slot to the audio processor (kept for other consumers).
    processor.setSamplerSelectedSlot(selectedSlot);
    spectralEditor.setSelectedSlot(selectedSlot);
    // Retarget every right-click MIDI-Learn to this (engine, slot) — "fixed slot
    // per button": a mapping learned now drives THIS slot forever.
    rebindMidiLearn();
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
    // (IMG removed — the bank level lives in the grid's per-bank mixer.)
    floorSlider.setValue(
        static_cast<double>(fs->getSlotEqFloor(selectedSlot)) * 100.0,
        juce::dontSendNotification);
    resumeToggle.setToggleState(fs->getSlotResumeMode(selectedSlot),
                                juce::dontSendNotification);
    // Overdub is engine-wide (not per-slot) — mirror the engine flag.
    overdubToggle.setToggleState(fs->getOverdubMode(),
                                 juce::dontSendNotification);
    // Crop / fade chips (the curves can also be edited on the image).
    refreshParamBoxes();
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

void SlotEditorComponent::refreshParamBoxes()
{
    // The chips read the engine inside paint() — a repaint IS the refresh.
    for (auto* b : { &cropStartBox_, &cropEndBox_,
                     &fadeInLenBox_,  &fadeInTypeBox_,  &fadeInPowBox_,
                     &fadeOutLenBox_, &fadeOutTypeBox_, &fadeOutPowBox_ })
        b->repaint();
}

void SlotEditorComponent::refreshLoopButtons()
{
    auto* fs = processor.getSampler(samplerIndex_);
    const LoopMode m = (fs != nullptr) ? fs->getSlotLoopMode(selectedSlot)
                                       : LoopMode::LOOP;
    bool f, b, r;
    decomposeLoopMode(m, f, b, r);
    loopBtns[0].setActive(f);
    loopBtns[1].setActive(b);
    loopBtns[2].setActive(r);
}

void SlotEditorComponent::applyLoopMode(LoopMode m)
{
    if (auto* fs = processor.getSampler(samplerIndex_))
        fs->setSlotLoopMode(selectedSlot, m);
    refreshLoopButtons();
}

// ─────────────────────────────────────────────────────────────────────────────
// Unified MIDI-Learn — right-click any play control / action button.
//
// Each control gets a MidiLearnAttachment whose target is a synthetic id
// encoding (engine, THIS slot, kind). Recreated on every slot / engine change so
// the mapping learned while a slot is shown drives THAT slot forever ("fixed
// slot per button"). The controls keep writing LuxSampler directly (unchanged);
// MIDI is an additional writer through the processor's IVirtualMidiSink.
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::rebindMidiLearn()
{
    using K = SamplerMidiTargets::Kind;
    midiLearn_.clear();

    auto& mm = processor.getMidiMap();
    const int e = samplerIndex_;
    const int s = selectedSlot;

    auto add = [&](juce::Component& c, K kind)
    {
        midiLearn_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, c, SamplerMidiTargets::makeId(e, s, kind)));
    };

    // Value play params. (K::Img now learns on the grid's bank level fader.)
    add(speedSlider,        K::Speed);
    add(floorSlider,        K::Floor);
    add(resumeToggle,       K::Resume);
    add(overdubToggle,      K::Overdub);          // engine-wide (slot ignored)
    // Crop / fade chips under the image — one learn target per box, so every
    // strip parameter (bounds, widths, curve types, powers) is CC-mappable.
    add(cropStartBox_,   K::CropStart);
    add(cropEndBox_,     K::CropEnd);
    add(fadeInLenBox_,   K::FadeInLen);
    add(fadeInTypeBox_,  K::FadeInType);
    add(fadeInPowBox_,   K::FadeInPow);
    add(fadeOutLenBox_,  K::FadeOutLen);
    add(fadeOutTypeBox_, K::FadeOutType);
    add(fadeOutPowBox_,  K::FadeOutPow);
    // Loop toggles — each maps its OWN 2-state target (a controller pad per
    // toggle). The legacy "loopmode" cycle target stays resolvable for
    // existing mappings (now 6 states).
    add(loopBtns[0], K::LoopFwd);
    add(loopBtns[1], K::LoopBwd);
    add(loopBtns[2], K::LoopRepeat);

    // Action buttons — momentary REC / PLAY, one-shot SAVE / CLEAR.
    add(recBtn,   K::Rec);
    add(playBtn,  K::Play);
    add(saveBtn,  K::Save);
    add(clearBtn, K::Clear);

    // Slot EQ: right-click the nearest band node → MIDI Learn (9 bands, this slot).
    eqEditor.setBandMidiLearn(&mm, [e, s](int band)
    { return SamplerMidiTargets::makeEqBandId(e, s, band); });
}

// ─────────────────────────────────────────────────────────────────────────────
// saveSlotToDisk — timestamped .fslot (+ optional image) for ONE slot. Shared by
// the SAVE button (selected slot) and the MIDI-mapped SAVE trigger (fixed slot).
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::saveSlotToDisk(int slot)
{
    if (LicenseGate::blockIfDemo(this, "Save sampler slot"))
        return;
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr || !fs->slotHasContent(slot))
        return;

    const juce::File dir = resolveSaveDirectory();
    if (!dir.isDirectory())
        return;

    const juce::Time now = juce::Time::getCurrentTime();
    const juce::String stamp = juce::String::formatted(
        "%04d%02d%02d-%02d%02d%02d",
        now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
        now.getHours(), now.getMinutes(), now.getSeconds());

    const juce::String base = stamp + "_slot"
                              + juce::String(slot).paddedLeft('0', 2);

    fs->saveSlotToFile(slot, dir.getChildFile(base + ".fslot"));

    // Optional image export — controlled by user settings.
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
        fs->exportSlotImage(slot, dir.getChildFile(base + ext), asPng);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drainMidiActionPulses — run REC / PLAY / SAVE triggers latched by the MIDI
// engine for the bound engine, across ALL slots (fixed slot per button). Uses
// LuxSampler's idempotent toggles (uiToggleRecord/uiPlaySlot stop when already
// in the matching state, so we gate on the current slot state).
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::drainMidiActionPulses()
{
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;

    // Momentary → act on both press and release edges; Toggle → act on the press
    // edge only (flip on/off) and ignore the release. Same rule as the buttons.
    const bool recMom  = processor.samplerRecMomentary (samplerIndex_);
    const bool playMom = processor.samplerPlayMomentary(samplerIndex_);

    for (int s = 0; s < LuxSamplerConstants::NUM_SLOTS; ++s)
    {
        const bool recP  = processor.consumeSmpRecPressed  (samplerIndex_, s);
        const bool recR  = processor.consumeSmpRecReleased (samplerIndex_, s);
        const bool playP = processor.consumeSmpPlayPressed (samplerIndex_, s);
        const bool playR = processor.consumeSmpPlayReleased(samplerIndex_, s);
        const bool saveT = processor.consumeSmpSaveTrigger (samplerIndex_, s);

        // REC — Momentary: press starts if idle, release stops if recording
        // (sequential ifs: a quick tap can land press+release in one tick).
        // Toggle: press flips record on↔off, release ignored.
        if (recMom)
        {
            if (recP && fs->getSlotState(s) != SlotState::RECORDING)
            {
                fs->uiToggleRecord(s);
                if (s == selectedSlot) spectralEditor.markDirty();
            }
            if (recR && fs->getSlotState(s) == SlotState::RECORDING)
            {
                fs->uiToggleRecord(s);
                if (s == selectedSlot) spectralEditor.markDirty();
            }
        }
        else if (recP)
        {
            fs->uiToggleRecord(s);
            if (s == selectedSlot) spectralEditor.markDirty();
        }

        // PLAY — Momentary: press starts if idle (and arms the sampler transport,
        // like the PLAY button), release stops if playing. Toggle: press flips
        // play↔stop (arm only when it actually starts), release ignored.
        auto armTransport = [this]
        {
            if (auto* p = processor.getAPVTS().getParameter("samplerFreezeMode"))
                p->setValueNotifyingHost(0.0f); // 0 = PLAY
        };
        if (playMom)
        {
            if (playP && fs->getSlotState(s) != SlotState::PLAYING)
            {
                fs->uiPlaySlot(s);
                armTransport();
            }
            if (playR && fs->getSlotState(s) == SlotState::PLAYING)
                fs->uiPlaySlot(s);
        }
        else if (playP)
        {
            const bool wasPlaying = (fs->getSlotState(s) == SlotState::PLAYING);
            fs->uiPlaySlot(s);
            if (! wasPlaying) armTransport();
        }

        if (saveT)
            saveSlotToDisk(s);

        // CLEAR (one-shot) → wipe the slot; refresh the view if it is shown.
        if (processor.consumeSmpClearTrigger(samplerIndex_, s))
        {
            fs->uiClearSlot(s);
            if (s == selectedSlot) { refreshSliderValues(); spectralEditor.markDirty(); }
        }

        // EQ bands (continuous) → apply latched gains (non-RT), reload the curve
        // if this slot's editor is on screen.
        bool eqTouched = false;
        for (int b = 0; b < LuxSampler::kEqBands; ++b)
        {
            const float v = processor.consumeSmpEqPending(samplerIndex_, s, b);
            if (v >= 0.0f)
            {
                fs->setSlotEqBandGain(s, b, v * 48.0f - 24.0f);   // 0..1 → ±24 dB
                eqTouched = true;
            }
        }
        if (eqTouched && s == selectedSlot)
            refreshFreqCurve();
    }
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
static constexpr int kEdRows     = 4;    // rows per column (incl. button row)

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
    g.drawText("BANK " + juce::String(selectedSlot + 1),
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
//   Left column  : [REC][PLAY][CLEAR] · Speed · Loop · Floor
//   Right column : [CROP][SAVE][LOAD] · Resume · Overdub
//   (fade curves are edited ON the image; their info labels sit under it)
//   Bottom       : SlotSpectralEditorComponent (fills the remaining height)
// (IMG removed — the bank level lives in the grid's per-bank mixer.)
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
            const int bW   = (ctrlW - 2 * bGap) / 3;
            loopLabel.setBounds(leftX, ry, lW, rowH);
            for (int k = 0; k < 3; ++k)
                loopBtns[k].setBounds(ctrlX + k * (bW + bGap), ry, bW, rowH);
        }
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
    }

    // ── Image editor (middle) + param-box strip + EQ panel (bottom) ───────────
    const int edY   = edParamBottom() + kEdGap;
    const int eqH   = juce::jmin(ScoreEqComponent::kPreferredH, (H - edY) / 2);
    const int eqY   = H - kEdPad - eqH;
    const int chipH = 16, chipGap = 2;
    const int infoH = 2 * chipH + chipGap + 2;  // two chip rows UNDER the image
    const int imgH  = juce::jmax(60, eqY - kEdGap - edY - infoH);
    spectralEditor.setBounds(kEdPad, edY, W - 2 * kEdPad, imgH);
    {
        const int stripW = W - 2 * kEdPad;
        const int halfW  = (stripW - chipGap) / 2;
        int y = edY + imgH + 2;

        // Row 1 — crop bounds, two half-width chips.
        cropStartBox_.setBounds(kEdPad, y, halfW, chipH);
        cropEndBox_  .setBounds(kEdPad + halfW + chipGap, y,
                                stripW - halfW - chipGap, chipH);
        y += chipH + chipGap;

        // Row 2 — [len][type][pow] per fade, in on the left / out on the right.
        const int typeW = 40;   // LIN/EXP/LOG/S chip
        auto fadeGroup = [&](int x0, int w, SamplerValueBox& len,
                             SamplerValueBox& type, SamplerValueBox& pow)
        {
            const int valW = (w - typeW - 2 * chipGap) / 2;
            len .setBounds(x0, y, valW, chipH);
            type.setBounds(x0 + valW + chipGap, y, typeW, chipH);
            pow .setBounds(x0 + valW + typeW + 2 * chipGap, y,
                           w - valW - typeW - 2 * chipGap, chipH);
        };
        fadeGroup(kEdPad, halfW,
                  fadeInLenBox_, fadeInTypeBox_, fadeInPowBox_);
        fadeGroup(kEdPad + halfW + chipGap, stripW - halfW - chipGap,
                  fadeOutLenBox_, fadeOutTypeBox_, fadeOutPowBox_);
    }
    eqEditor      .setBounds(kEdPad, eqY, W - 2 * kEdPad, eqH);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer callback (~5 Hz)
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::timerCallback()
{
    blinkOn = !blinkOn;

    // Keep the REC / PLAY buttons' interaction mode in step with the per-engine
    // Toggle / Momentary preference (set in SamplerSetupPanel).
    recBtn .momentary = processor.samplerRecMomentary (samplerIndex_);
    playBtn.momentary = processor.samplerPlayMomentary(samplerIndex_);

    // ── Run MIDI-triggered REC / PLAY / SAVE action pulses (fixed slot each) ──
    drainMidiActionPulses();

    // ── Reflect MIDI-driven value moves on the shown slot ────────────────────
    // A mapped controller writes LuxSampler straight from the audio thread, so
    // the sliders won't follow unless we resync. Refresh only when a value on
    // the shown (engine, slot) actually moved since last tick.
    {
        const uint32_t g = processor.smpValueTouchGen();
        if (g != lastValueTouchGen_)
        {
            lastValueTouchGen_ = g;
            if (processor.smpValueTouchWhere() == ((samplerIndex_ << 8) | selectedSlot))
            {
                refreshSliderValues();   // also refreshParamBoxes() + refreshFreqCurve()
                refreshLoopButtons();
                spectralEditor.markDirty();   // CC on crop/fade moves the overlay
            }
        }
    }

    auto* fs = processor.getSampler(samplerIndex_);
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
//   1. active session exports/ folder (Standalone)
//   2. last export directory used
//   3. ~/Documents
// Creates the directory if it does not yet exist.
// ─────────────────────────────────────────────────────────────────────────────
juce::File SlotEditorComponent::resolveSaveDirectory() const
{
    juce::File dir = processor.sessions()->startDirFor(
        PathKeys::exportDir,
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        /*isExport*/ true);

    if (!dir.isDirectory())
        dir.createDirectory();

    return dir;
}
