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
    // Initial text: "Bank N\n—". Colors + state text are updated by timerCallback().
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
    {
        fsBankBtns[i].setButtonText("Bank " + juce::String(i + 1) + "\n\xe2\x80\x94");
        fsBankBtns[i].onClick = [this, i]
        {
            if (auto* fs = audioProcessor.getFrameSampler())
                fs->uiToggleRecord(i);
        };
        addAndMakeVisible(fsBankBtns[i]);
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

    // Height: header(52) + vis(64+18) + controls(9×31=279) + FS section(24+4+96) + footer(38) + margins
    // fsSectionY=451, fsBtnsY=479, footerY=585, window=628
    setSize(740, 628);
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

        // Right hint — slot trigger info
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(0xff886699));
        g.drawText("click = rec / stop", sh.reduced(6, 0),
                   juce::Justification::centredRight, true);
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
                    // Alternate between bright and dim red for blink effect
                    stateStr   = "\xe2\x97\x8f REC";         // ● REC  (U+25CF UTF-8)
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
                    stateStr   = "\xe2\x96\xba PLAY";        // ► PLAY  (U+25BA UTF-8)
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
                        stateStr   = "\xe2\x80\x94";         // — em-dash (U+2014 UTF-8)
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
