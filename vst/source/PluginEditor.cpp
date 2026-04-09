#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Static helper — configure a horizontal slider with an optional value suffix.
static void initSlider(juce::Slider& s, const char* suffix = nullptr)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 20);
    if (suffix)
        s.setTextValueSuffix(suffix);
}

//==============================================================================
Sp3ctraAudioProcessorEditor::Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    auto& apvts = audioProcessor.getAPVTS();

    // ── CIS Visualizer ───────────────────────────────────────────────────────
    cisVisualizer = std::make_unique<CisVisualizerComponent>(audioProcessor);
    addAndMakeVisible(cisVisualizer.get());

    // ── LuxStral — Device On ─────────────────────────────────────────────────
    deviceOnToggle.setButtonText("Active");
    addAndMakeVisible(deviceOnToggle);
    deviceOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "deviceEnabled", deviceOnToggle);

    // ── LuxStral — Master Volume ──────────────────────────────────────────────
    initSlider(masterVolumeSlider);
    addAndMakeVisible(masterVolumeSlider);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "masterVolume", masterVolumeSlider);

    // ── LuxStral — Gamma ─────────────────────────────────────────────────────
    initSlider(gammaSlider);
    addAndMakeVisible(gammaSlider);
    gammaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralGammaValue", gammaSlider);

    // ── LuxStral — Attack ────────────────────────────────────────────────────
    initSlider(attackSlider, " ms");
    addAndMakeVisible(attackSlider);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralAttackMs", attackSlider);

    // ── LuxStral — Release ───────────────────────────────────────────────────
    initSlider(releaseSlider, " ms");
    addAndMakeVisible(releaseSlider);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralReleaseMs", releaseSlider);

    // ── LuxStral — Contrast Min ───────────────────────────────────────────────
    initSlider(contrastMinSlider);
    addAndMakeVisible(contrastMinSlider);
    contrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralContrastMin", contrastMinSlider);

    // ── LuxStral — Stereo Temperature Amplification ──────────────────────────
    initSlider(stereoTempSlider);
    addAndMakeVisible(stereoTempSlider);
    stereoTempAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralStereoTempAmp", stereoTempSlider);

    // ── LuxStral — Summation Response Exponent ───────────────────────────────
    initSlider(sumExpSlider);
    addAndMakeVisible(sumExpSlider);
    sumExpAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSummationResponseExp", sumExpSlider);

    // ── LuxStral — Noise Gate Threshold ──────────────────────────────────────
    initSlider(noiseGateSlider);
    addAndMakeVisible(noiseGateSlider);
    noiseGateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNoiseGateThreshold", noiseGateSlider);

    // ── StrokeForge — Enable ─────────────────────────────────────────────────
    sfEnabledToggle.setButtonText("Active");
    addAndMakeVisible(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfEnabled", sfEnabledToggle);

    // ── StrokeForge — Blob Threshold ─────────────────────────────────────────
    initSlider(sfBlobThreshSlider);
    addAndMakeVisible(sfBlobThreshSlider);
    sfBlobThreshAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobBaseThreshold", sfBlobThreshSlider);

    // ── StrokeForge — Merge Gap ───────────────────────────────────────────────
    initSlider(sfMergeGapSlider, " pix");
    addAndMakeVisible(sfMergeGapSlider);
    sfMergeGapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMergeGap", sfMergeGapSlider);

    // ── StrokeForge — Focus Sigma ─────────────────────────────────────────────
    initSlider(sfFocusSigmaSlider, " pix");
    addAndMakeVisible(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    // ── StrokeForge — Spectral Width Threshold ────────────────────────────────
    initSlider(sfSpectralWidthSlider, " pix");
    addAndMakeVisible(sfSpectralWidthSlider);
    sfSpectralWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfSpectralWidthThreshold", sfSpectralWidthSlider);

    // ── StrokeForge — Focus Only (no morph) ──────────────────────────────────
    sfFocusOnlyToggle.setButtonText("On (no morph)");
    addAndMakeVisible(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfFocusOnly", sfFocusOnlyToggle);

    // ── Frame Sampler — Enable toggle ────────────────────────────────────────
    fsEnabledToggle.setButtonText("Active");
    addAndMakeVisible(fsEnabledToggle);
    fsEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "frameSamplerEnabled", fsEnabledToggle);

    // ── Frame Sampler — Bank record buttons (12 slots, 6×2 grid) ─────────────
    // Single click = context-sensitive (play/rec/stop).
    // Double-click (< 400 ms) = clear slot.
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
    {
        fsBankBtns[i].setButtonText("Bank " + juce::String(i + 1) + "\n-");
        fsBankBtns[i].onClick = [this, i]
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::int64 ms = (now - fsBankLastClickTime[i]).inMilliseconds();
            fsBankLastClickTime[i] = now;

            auto* fs = audioProcessor.getFrameSampler();
            if (fs == nullptr) return;

            // Double-click → clear
            if (ms < 400 && ms >= 0)
            {
                fs->uiClearSlot(i);
                return;
            }

            // Single click: context-sensitive
            const SlotState st         = fs->getSlotState(i);
            const bool       hasContent = fs->slotHasContent(i);

            if (st == SlotState::RECORDING || st == SlotState::ARMED)
                fs->uiToggleRecord(i);   // stop recording / cancel arm
            else if (st == SlotState::PLAYING)
                fs->uiPlaySlot(i);       // stop playback
            else if (hasContent)
                fs->uiPlaySlot(i);       // start playback
            else
                fs->uiToggleRecord(i);   // start recording
        };
        addAndMakeVisible(fsBankBtns[i]);
    }

    // ── Sequencer — Enable toggle ─────────────────────────────────────────────
    seqEnabledToggle.setButtonText("Active");
    addAndMakeVisible(seqEnabledToggle);
    seqEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqEnabled", seqEnabledToggle);

    // ── Sequencer — Play / Stop buttons ──────────────────────────────────────
    seqPlayBtn.onClick = [this]
    {
        if (auto* seq = audioProcessor.getFrameSequencer())
            seq->uiPlay();
    };
    addAndMakeVisible(seqPlayBtn);

    seqStopBtn.onClick = [this]
    {
        if (auto* seq = audioProcessor.getFrameSequencer())
            seq->uiStop();
    };
    addAndMakeVisible(seqStopBtn);

    // ── Sequencer — BPM slider ────────────────────────────────────────────────
    seqBpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    seqBpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
    seqBpmSlider.setTextValueSuffix(" BPM");
    addAndMakeVisible(seqBpmSlider);
    seqBpmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "seqBpm", seqBpmSlider);

    seqBpmLabel.setText("BPM", juce::dontSendNotification);
    seqBpmLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(seqBpmLabel);

    // ── Sequencer — Steps combo ───────────────────────────────────────────────
    seqStepsCombo.addItemList({"4", "8", "12", "16", "24", "32"}, 1);
    // Sync initial selection from APVTS
    {
        const int cur = static_cast<int>(apvts.getRawParameterValue("seqNumSteps")->load());
        const int choices[] = { 4, 8, 12, 16, 24, 32 };
        int selected = 4; // default item ID
        for (int k = 0; k < 6; ++k)
            if (choices[k] == cur) { selected = k + 1; break; }
        seqStepsCombo.setSelectedId(selected, juce::dontSendNotification);
    }
    seqStepsCombo.onChange = [this]
    {
        const int choices[] = { 4, 8, 12, 16, 24, 32 };
        const int id = seqStepsCombo.getSelectedId();
        if (id >= 1 && id <= 6)
        {
            const int nSteps = choices[id - 1];
            auto& apvts2 = audioProcessor.getAPVTS();
            if (auto* param = apvts2.getParameter("seqNumSteps"))
                param->setValueNotifyingHost(
                    param->convertTo0to1(static_cast<float>(nSteps)));
            if (auto* seq = audioProcessor.getFrameSequencer())
                seq->setNumSteps(nSteps);
        }
    };
    seqStepsLabel.setText("Steps", juce::dontSendNotification);
    seqStepsLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(seqStepsCombo);
    addAndMakeVisible(seqStepsLabel);

    // ── Sequencer — Loop toggle ───────────────────────────────────────────────
    seqLoopToggle.setButtonText("Loop");
    addAndMakeVisible(seqLoopToggle);
    seqLoopAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqLoop", seqLoopToggle);

    // ── Sequencer — DAW sync toggle ───────────────────────────────────────────
    seqDawSyncToggle.setButtonText("DAW sync");
    seqDawSyncToggle.setTooltip(
        "DAW sync: step timing follows Ableton/Live PPQ transport.\n"
        "Uncheck = internal BPM clock (standalone use).");
    addAndMakeVisible(seqDawSyncToggle);
    seqDawSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqDawSync", seqDawSyncToggle);

    // ── Sequencer — Step grid (32 cells) ─────────────────────────────────────
    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        seqStepBtns[i].setButtonText("-");   // "-" = empty step
        seqStepBtns[i].onClick = [this, i]
        {
            if (auto* seq = audioProcessor.getFrameSequencer())
            {
                const int cur  = seq->getStep(i);
                const int next = (cur < 0) ? 0 : (cur + 1) % FrameSamplerConstants::NUM_SLOTS;
                // After bank 11 (0-indexed), wrap to empty (-1)
                seq->setStep(i, (cur >= FrameSamplerConstants::NUM_SLOTS - 1) ? -1 : next);
            }
        };
        addAndMakeVisible(seqStepBtns[i]);
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    settingsButton.setButtonText("Settings...");
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(statusLabel);

    // 200 ms refresh: fast enough to animate the RECORDING blink (~2.5 Hz)
    startTimer(200);

    // Height: seqGridY=643, footerY=717, window=760
    setSize(740, 760);
}

Sp3ctraAudioProcessorEditor::~Sp3ctraAudioProcessorEditor()
{
    stopTimer();
    settingsWindow.reset();
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::paint(juce::Graphics& g)
{
    // ── Background ───────────────────────────────────────────────────────────
    g.fillAll(juce::Colour(0xff1e1e1e));

    // ── Header ───────────────────────────────────────────────────────────────
    {
        const juce::Rectangle<float> hdr(0.0f, 0.0f, (float)getWidth(), (float)kHeaderH);
        g.setGradientFill(juce::ColourGradient(
            juce::Colour(0xff383838), 0.0f, 0.0f,
            juce::Colour(0xff262626), 0.0f, (float)kHeaderH, false));
        g.fillRect(hdr);

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(22.0f)).boldened());
        g.drawText("Sp3ctra",
                   juce::Rectangle<int>(12, 0, getWidth() - 24, kHeaderH),
                   juce::Justification::centredLeft, true);

        g.setFont(juce::FontOptions(11.0f));
        g.setColour(juce::Colour(0xff888888));
        g.drawText("v0.0.1",
                   juce::Rectangle<int>(0, 0, getWidth() - 12, kHeaderH),
                   juce::Justification::centredRight, true);
    }

    // Thin separator below header
    g.setColour(juce::Colour(0xff444444));
    g.fillRect(0, kHeaderH, getWidth(), 1);

    // ── Column geometry ───────────────────────────────────────────────────────
    const int cw  = colWidth();
    const int lxp = colLX();
    const int rxp = colRX();
    const int rsy = rowsStartY();

    // ── LuxStral section badge ────────────────────────────────────────────────
    {
        const juce::Rectangle<int> sh(lxp, kContentY, cw, kSectionH);
        g.setColour(juce::Colour(0xff1c3755));
        g.fillRoundedRectangle(sh.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff7ab0f0));
        g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
        g.drawText("LUXSTRAL", sh.reduced(6, 0), juce::Justification::centredLeft, true);
    }

    // ── StrokeForge section badge ─────────────────────────────────────────────
    {
        const juce::Rectangle<int> sh(rxp, kContentY, cw, kSectionH);
        g.setColour(juce::Colour(0xff3d2e00));
        g.fillRoundedRectangle(sh.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xffffc84a));
        g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
        g.drawText("STROKEFORGE", sh.reduced(6, 0), juce::Justification::centredLeft, true);
    }

    // ── Row labels ────────────────────────────────────────────────────────────
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(juce::Colour(0xffb8c4d0));

    // LuxStral labels — order: Device On, Volume, Gamma, Contrast Min, Attack,
    //                          Release, Stereo Temp., Sum. Exp., Noise Gate
    static const char* const lsLabels[kLS_ROWS] = {
        "Device On",
        "Volume",
        "Gamma",
        "Contrast Min",
        "Attack",
        "Release",
        "Stereo Temp.",
        "Sum. Exp.",
        "Noise Gate",
    };
    for (int i = 0; i < kLS_ROWS; ++i)
    {
        g.drawText(lsLabels[i],
                   juce::Rectangle<int>(lxp, rsy + i * kRowStep, kLabelW, kRowH),
                   juce::Justification::centredRight, true);
    }

    // StrokeForge labels — row 3 contains the UTF-8 sigma character (U+03C3)
    static const char* const sfLabels[kSF_ROWS] = {
        "SF Active",
        "Blob Thr.",
        "Merge Gap",
        "Focus \xcf\x83",     // "Focus σ" in UTF-8
        "Spectral Thr.",
        "Focus Only",
    };
    for (int i = 0; i < kSF_ROWS; ++i)
    {
        g.drawText(juce::String::fromUTF8(sfLabels[i]),
                   juce::Rectangle<int>(rxp, rsy + i * kRowStep, kLabelW, kRowH),
                   juce::Justification::centredRight, true);
    }

    // ── Frame Sampler section badge ───────────────────────────────────────────
    {
        const int fsy = fsSectionY();
        const juce::Rectangle<int> sh(kHPad, fsy, getWidth() - 2 * kHPad, kFS_SECT_H);
        g.setColour(juce::Colour(0xff2a1a3a));
        g.fillRoundedRectangle(sh.toFloat(), 3.0f);

        // Left label "FRAME SAMPLER"
        g.setColour(juce::Colour(0xffcc88ff));
        g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
        g.drawText("FRAME SAMPLER", sh.reduced(6, 0), juce::Justification::centredLeft, true);

        // Right hint — leave 78 px for the Active toggle on the right
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(0xff886699));
        const juce::Rectangle<int> fsHintR(
            sh.getX() + 100,
            sh.getY(),
            sh.getWidth() - 100 - 78,
            sh.getHeight());
        g.drawText("click = play/rec  |  dbl-click = clear", fsHintR,
                   juce::Justification::centred, true);
    }

    // ── Sequencer section badge ───────────────────────────────────────────────
    {
        const juce::Rectangle<int> sh(kHPad, seqSectionY(),
                                       getWidth() - 2 * kHPad, kSeqSectH);
        g.setColour(juce::Colour(0xff1a2a1a));
        g.fillRoundedRectangle(sh.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff66cc88));
        g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
        g.drawText("STEP SEQUENCER", sh.reduced(6, 0), juce::Justification::centredLeft, true);
        // Hint text — must not overlap the Active toggle (72 px from right)
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(0xff447755));
        const juce::Rectangle<int> hintR(
            sh.getX() + 120,        // start after "STEP SEQUENCER" label
            sh.getY(),
            sh.getWidth() - 120 - 78,  // leave 78 px for Active toggle
            sh.getHeight());
        g.drawText("click = cycle bank  |  empty = pass",
                   hintR, juce::Justification::centred, true);
    }

    // ── Separator above footer ────────────────────────────────────────────────
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
    cisVisualizer->setBounds(kHPad, kVisY, getWidth() - 2 * kHPad, kVisH);

    // ── LuxStral controls — left column ──────────────────────────────────────
    {
        const int cx = lxp + kCtrlOffset;
        int cy = rsy;

        deviceOnToggle      .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        masterVolumeSlider  .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        gammaSlider         .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        contrastMinSlider   .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        attackSlider        .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        releaseSlider       .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        stereoTempSlider    .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        sumExpSlider        .setBounds(cx, cy, kCtrlW, kRowH); cy += kRowStep;
        noiseGateSlider     .setBounds(cx, cy, kCtrlW, kRowH);
    }

    // ── StrokeForge controls — right column ───────────────────────────────────
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

    // ── Frame Sampler — Enable toggle (right side of FS section badge) ────────
    {
        const int fsy      = fsSectionY();
        const int toggleW  = 72;
        const int toggleX  = getWidth() - kHPad - toggleW;
        // Positioned inside the badge row, right-aligned
        fsEnabledToggle.setBounds(toggleX, fsy + 2, toggleW, kFS_SECT_H - 4);
    }

    // ── Frame Sampler — Bank buttons (6 columns × 2 rows = 12 slots) ─────────
    {
        const int totalW    = getWidth() - 2 * kHPad;
        const int gapBetween = 4;
        const int bw        = (totalW - (kFS_BTN_COLS - 1) * gapBetween) / kFS_BTN_COLS;
        const int by        = fsBtnsY();

        for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
        {
            const int col = i % kFS_BTN_COLS;
            const int row = i / kFS_BTN_COLS;
            const int bx  = kHPad + col * (bw + gapBetween);
            const int ry  = by + row * (kFS_BTN_H + kFS_BTN_GAP);
            fsBankBtns[i].setBounds(bx, ry, bw, kFS_BTN_H);
        }
    }

    // ── Sequencer — Enable toggle (right of badge) ────────────────────────────
    {
        const int sy     = seqSectionY();
        const int toggleW = 72;
        seqEnabledToggle.setBounds(getWidth() - kHPad - toggleW, sy + 2, toggleW, kSeqSectH - 4);
    }

    // ── Sequencer — Transport row ─────────────────────────────────────────────
    {
        const int ty      = seqTransY();
        const int totalW  = getWidth() - 2 * kHPad;
        int       cx      = kHPad;
        const int btnW    = 32;
        const int gap     = 4;
        const int bpmLW   = 30;  // "BPM" label width
        const int bpmSlW  = 150; // BPM slider width
        const int stpLW   = 38;  // "Steps" label width
        const int stpCW   = 56;  // steps combo width
        const int togW    = 54;  // toggle width (loop/daw)

        seqPlayBtn  .setBounds(cx, ty, btnW, kSeqTransH);     cx += btnW + gap;
        seqStopBtn  .setBounds(cx, ty, btnW, kSeqTransH);     cx += btnW + gap * 3;
        seqBpmLabel .setBounds(cx, ty, bpmLW, kSeqTransH);    cx += bpmLW + 2;
        seqBpmSlider.setBounds(cx, ty, bpmSlW, kSeqTransH);   cx += bpmSlW + gap * 3;
        seqStepsLabel.setBounds(cx, ty, stpLW, kSeqTransH);   cx += stpLW + 2;
        seqStepsCombo.setBounds(cx, ty, stpCW, kSeqTransH);   cx += stpCW + gap * 3;
        seqLoopToggle.setBounds(cx, ty, togW, kSeqTransH);    cx += togW + gap;
        seqDawSyncToggle.setBounds(cx, ty, togW, kSeqTransH);

        juce::ignoreUnused(totalW);
    }

    // ── Sequencer — Step grid (2 rows × 16 cells) ─────────────────────────────
    {
        const int totalW   = getWidth() - 2 * kHPad;
        const int cols     = 16;
        const int cellGap  = 2;
        const int cellW    = (totalW - (cols - 1) * cellGap) / cols;
        const int gy       = seqGridY();

        for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
        {
            const int col = i % cols;
            const int row = i / cols;
            const int cx2 = kHPad + col * (cellW + cellGap);
            const int cy2 = gy   + row * (kSeqCellH + kSeqCellGap);
            seqStepBtns[i].setBounds(cx2, cy2, cellW, kSeqCellH);
        }
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    const int fy = footerY();
    settingsButton.setBounds(kHPad,           fy, 92, 28);
    statusLabel   .setBounds(kHPad + 100, fy, getWidth() - kHPad - 104, 28);
}

//==============================================================================
void Sp3ctraAudioProcessorEditor::timerCallback()
{
    // ── UDP status label ──────────────────────────────────────────────────────
    auto* core = audioProcessor.getSp3ctraCore();
    if (core && core->isInitialized())
    {
        const int  port    = (int)audioProcessor.getAPVTS().getRawParameterValue("udpPort")->load();
        const auto address = audioProcessor.getUdpAddressString();
        statusLabel.setText("UDP: " + address + ":" + juce::String(port),
                            juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    }
    else
    {
        statusLabel.setText("waiting for CIS data...", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    }

    // ── Frame Sampler bank buttons — refresh color + label every 200 ms ───────
    // fsBlinkOn toggles each call → ~2.5 Hz blink for RECORDING state.
    fsBlinkOn = !fsBlinkOn;

    if (auto* fs = audioProcessor.getFrameSampler())
    {
        const bool fsActive = fs->isEnabled();

        for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
        {
            const SlotState st        = fs->getSlotState(i);
            const bool      hasContent = fs->slotHasContent(i);
            const uint64_t  durUs      = fs->getSlotDurationUs(i);

            juce::String stateStr;
            juce::Colour bgColour;
            juce::Colour textColour;

            switch (st)
            {
                case SlotState::RECORDING:
                    stateStr   = "* REC";  // ASCII: bullet + REC
                    bgColour   = fsBlinkOn ? juce::Colour(0xffcc2222)
                                           : juce::Colour(0xff7a1010);
                    textColour = juce::Colours::white;
                    break;

                case SlotState::ARMED:
                    stateStr   = "ARM";
                    bgColour   = juce::Colour(0xff7a4a00);
                    textColour = juce::Colour(0xffffcc66);
                    break;

                case SlotState::PLAYING:
                    stateStr   = "> PLAY"; // ASCII: arrow + PLAY
                    bgColour   = juce::Colour(0xff1a5a1a);
                    textColour = juce::Colour(0xff88ff88);
                    break;

                default: // IDLE
                    if (hasContent)
                    {
                        const float durS = static_cast<float>(durUs) * 1e-6f;
                        stateStr   = juce::String(durS, 1) + " s";
                        bgColour   = juce::Colour(0xff1e3028);
                        textColour = juce::Colour(0xff66cc88);
                    }
                    else
                    {
                        stateStr   = "--";  // ASCII: empty slot
                        bgColour   = juce::Colour(0xff2a2a2a);
                        textColour = juce::Colour(0xff555555);
                    }
                    break;
            }

            // Grey out everything when FrameSampler is disabled
            if (!fsActive)
            {
                bgColour   = bgColour  .withAlpha(0.35f);
                textColour = textColour.withAlpha(0.35f);
            }

            const juce::String label = "Bank " + juce::String(i + 1)
                                       + "\n" + stateStr;

            fsBankBtns[i].setButtonText(label);
            fsBankBtns[i].setColour(juce::TextButton::buttonColourId,   bgColour);
            fsBankBtns[i].setColour(juce::TextButton::buttonOnColourId, bgColour.brighter(0.15f));
            fsBankBtns[i].setColour(juce::TextButton::textColourOffId,  textColour);
            fsBankBtns[i].setColour(juce::TextButton::textColourOnId,   textColour);
            // Allow clicking even when disabled to show clear intent; guard in uiToggleRecord
            fsBankBtns[i].setEnabled(true);
        }
    }

    // ── Sequencer step cells — refresh bank label + active highlight ──────────
    if (auto* seq = audioProcessor.getFrameSequencer())
    {
        const bool seqActive  = seq->isEnabled();
        const int  curStep    = seq->getCurrentStep();
        const int  nSteps     = seq->getNumSteps();

        for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
        {
            const bool beyondActive = (i >= nSteps);
            const int  bankIdx      = seq->getStep(i);
            const bool isCurrent    = (i == curStep);

            juce::String cellText;
            juce::Colour bgCol;
            juce::Colour textCol;

            if (beyondActive)
            {
                // Greyed out — outside active step count
                cellText = "-";
                bgCol    = juce::Colour(0xff1a1a1a);
                textCol  = juce::Colour(0xff303030);
            }
            else if (bankIdx < 0)
            {
                // Empty (passthrough)
                cellText = "-";
                bgCol    = isCurrent ? juce::Colour(0xff2a4a2a)
                                     : juce::Colour(0xff2a2a2a);
                textCol  = isCurrent ? juce::Colour(0xff88ffaa)
                                     : juce::Colour(0xff555555);
            }
            else
            {
                // Assigned to bank N
                cellText = juce::String(bankIdx + 1);
                bgCol    = isCurrent ? juce::Colour(0xff1a6a1a)
                                     : juce::Colour(0xff1e3028);
                textCol  = isCurrent ? juce::Colours::white
                                     : juce::Colour(0xff66cc88);
            }

            if (!seqActive)
            {
                bgCol   = bgCol  .withAlpha(0.35f);
                textCol = textCol.withAlpha(0.35f);
            }

            seqStepBtns[i].setButtonText(cellText);
            seqStepBtns[i].setColour(juce::TextButton::buttonColourId,  bgCol);
            seqStepBtns[i].setColour(juce::TextButton::textColourOffId, textCol);
            seqStepBtns[i].setEnabled(!beyondActive);
        }
    }
}

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
    if (cisVisualizer)
        cisVisualizer->suspend();
}

void Sp3ctraAudioProcessorEditor::resumeVisualizer()
{
    if (cisVisualizer)
        cisVisualizer->resume();
}
