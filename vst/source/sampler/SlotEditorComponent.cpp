#include "SlotEditorComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

static const char* kLoopLabels[4] = { "NONE", "LOOP", "INV", "PING" };
static const char* kNoteNamesEd[FrameSamplerConstants::NUM_SLOTS] = {
    "C1","C#1","D1","D#1","E1","F1","F#1","G1","G#1","A1","A#1","B1"
};

SlotEditorComponent::SlotEditorComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      timeline(proc) // SlotTimelineComponent ctor
{
    // ── Timeline ──────────────────────────────────────────────────────────────
    // Start/End handles are dragged directly on the timeline and update
    // FrameSampler atomics in SlotTimelineComponent::mouseDrag() — no sliders.
    // The onStartChanged / onEndChanged callbacks are not used here.
    addAndMakeVisible(timeline);

    // ── Action buttons ────────────────────────────────────────────────────────
    recBtn.onClick = [this]
    {
        if (auto* fs = processor.getFrameSampler())
        {
            fs->uiToggleRecord(selectedSlot);
            timeline.markDirty(); // thumbnail may have changed after record
        }
    };
    addAndMakeVisible(recBtn);

    playBtn.onClick = [this]
    {
        if (auto* fs = processor.getFrameSampler())
            fs->uiPlaySlot(selectedSlot);
    };
    addAndMakeVisible(playBtn);

    clearBtn.onClick = [this]
    {
        if (auto* fs = processor.getFrameSampler())
        {
            fs->uiClearSlot(selectedSlot);
            timeline.markDirty();
        }
    };
    addAndMakeVisible(clearBtn);

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
        if (auto* fs = processor.getFrameSampler())
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
        if (auto* fs = processor.getFrameSampler())
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
        if (auto* fs = processor.getFrameSampler())
            fs->setSlotResumeMode(selectedSlot,
                                   resumeToggle.getToggleState());
    };
    addAndMakeVisible(resumeToggle);

    startTimer(200); // ~5 Hz for state refresh
}

SlotEditorComponent::~SlotEditorComponent()
{
    stopTimer();
}

void SlotEditorComponent::setSelectedSlot(int idx)
{
    selectedSlot = juce::jlimit(0, FrameSamplerConstants::NUM_SLOTS - 1, idx);
    timeline.setSelectedSlot(selectedSlot);
    refreshSliderValues();
    refreshLoopButtons();
    repaint();
}

void SlotEditorComponent::refreshSliderValues()
{
    auto* fs = processor.getFrameSampler();
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
}

void SlotEditorComponent::refreshLoopButtons()
{
    auto* fs = processor.getFrameSampler();
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
    if (auto* fs = processor.getFrameSampler())
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
    auto* fs = processor.getFrameSampler();
    const SlotState st = (fs != nullptr) ? fs->getSlotState(selectedSlot)
                                         : SlotState::IDLE;
    juce::String stateStr;
    juce::Colour stateCol = juce::Colour(0xff666666);
    switch (st)
    {
        case SlotState::RECORDING:
            stateStr = blinkOn ? "* REC" : "  REC";
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

    // ── Left: REC / PLAY / CLEAR (y=30) ──────────────────────────────────────
    {
        const int btnY   = 30;
        constexpr int btnH   = Sp3ctraTheme::kControlH;
        const int btnGap = Sp3ctraTheme::kGap;
        const int bW     = (leftW - 2 * btnGap) / 3;
        recBtn  .setBounds(leftX,                       btnY, bW, btnH);
        playBtn .setBounds(leftX + bW + btnGap,         btnY, bW, btnH);
        clearBtn.setBounds(leftX + 2 * (bW + btnGap),   btnY, bW, btnH);
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer callback (~5 Hz)
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::timerCallback()
{
    blinkOn = !blinkOn;

    auto* fs = processor.getFrameSampler();
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
            recBtn.setColour(juce::TextButton::buttonColourId,
                             blinkOn ? juce::Colour(0xffcc2222)
                                     : juce::Colour(0xff7a1010));
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

    repaint(); // refresh title state indicator
}
