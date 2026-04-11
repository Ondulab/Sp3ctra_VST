#include "SlotEditorComponent.h"
#include "../PluginProcessor.h"

static const char* kLoopLabels[4] = { "NONE", "LOOP", "INV", "PING" };
static const char* kNoteNamesEd[FrameSamplerConstants::NUM_SLOTS] = {
    "C1","C#1","D1","D#1","E1","F1","F#1","G1","G#1","A1","A#1","B1"
};

SlotEditorComponent::SlotEditorComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      timeline(proc) // SlotTimelineComponent ctor
{
    // ── Timeline ──────────────────────────────────────────────────────────────
    // Bidirectional sync: dragging timeline handles updates sliders, and sliders
    // update FrameSampler atomics (which the timeline reads back via poll).
    timeline.onStartChanged = [this](float v)
    {
        startSlider.setValue(static_cast<double>(v), juce::dontSendNotification);
    };
    timeline.onEndChanged = [this](float v)
    {
        endSlider.setValue(static_cast<double>(v), juce::dontSendNotification);
    };
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
    for (auto* lbl : { &startLabel, &endLabel, &speedLabel, &loopLabel })
    {
        lbl->setFont(juce::FontOptions(11.0f));
        lbl->setColour(juce::Label::textColourId, juce::Colour(0xffb0b0c0));
        lbl->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(lbl);
    }

    // ── Sliders ───────────────────────────────────────────────────────────────
    auto initSl = [](juce::Slider& s, double lo, double hi,
                     double step, const char* suf)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
        s.setRange(lo, hi, step);
        if (suf) s.setTextValueSuffix(suf);
    };

    initSl(startSlider, 0.0, 1.0, 0.001, nullptr);
    startSlider.onValueChange = [this]
    {
        const float v = static_cast<float>(startSlider.getValue());
        if (auto* fs = processor.getFrameSampler())
            fs->setSlotStartFrac(selectedSlot, v);
        // Redraw timeline so the start marker follows the slider immediately
        timeline.repaint();
    };
    addAndMakeVisible(startSlider);

    initSl(endSlider, 0.0, 1.0, 0.001, nullptr);
    endSlider.setValue(1.0, juce::dontSendNotification);
    endSlider.onValueChange = [this]
    {
        const float v = static_cast<float>(endSlider.getValue());
        if (auto* fs = processor.getFrameSampler())
            fs->setSlotEndFrac(selectedSlot, v);
        timeline.repaint();
    };
    addAndMakeVisible(endSlider);

    initSl(speedSlider, 0.01, 32.0, 0.01, "x");
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

    startSlider.setValue(
        static_cast<double>(fs->getSlotStartFrac(selectedSlot)),
        juce::dontSendNotification);
    endSlider.setValue(
        static_cast<double>(fs->getSlotEndFrac(selectedSlot)),
        juce::dontSendNotification);
    speedSlider.setValue(
        static_cast<double>(fs->getSlotSpeed(selectedSlot)),
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
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::paint(juce::Graphics& g)
{
    // Background
    g.setColour(juce::Colour(0xff1a1a2a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    // Title badge
    g.setColour(juce::Colour(0xff2a1a3a));
    g.fillRoundedRectangle(
        juce::Rectangle<float>(4.0f, 4.0f, (float)(getWidth() - 8), 22.0f), 3.0f);

    g.setColour(juce::Colour(0xffcc88ff));
    g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    g.drawText(juce::String("SLOT > ") + kNoteNamesEd[selectedSlot],
               juce::Rectangle<int>(8, 4, getWidth() - 16, 22),
               juce::Justification::centredLeft, false);

    // State indicator (right side of title)
    auto* fs = processor.getFrameSampler();
    const SlotState st = (fs != nullptr) ? fs->getSlotState(selectedSlot)
                                         : SlotState::IDLE;
    juce::String stateStr;
    juce::Colour stateCol = juce::Colour(0xff666666);
    switch (st)
    {
        case SlotState::RECORDING:
            stateStr = blinkOn ? "* REC" : "  REC";
            stateCol = juce::Colour(0xffff4444); break;
        case SlotState::ARMED:
            stateStr = "ARM";
            stateCol = juce::Colour(0xffffcc00); break;
        case SlotState::PLAYING:
            stateStr = "PLAY";
            stateCol = juce::Colour(0xff44ff44); break;
        default:
            stateStr = (fs && fs->slotHasContent(selectedSlot)) ? "IDLE" : "EMPTY";
            break;
    }
    g.setColour(stateCol);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(stateStr, juce::Rectangle<int>(8, 4, getWidth() - 16, 22),
               juce::Justification::centredRight, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout
//
//   y=4    Title badge (h=22)
//   y=30   Buttons REC / PLAY / CLEAR (h=30)
//   y=64   Timeline (h=54)
//   y=122  Sliders Start/End/Speed  (step=31, 3 rows → h=93)
//   y=219  Loop buttons             (h=27)
//   y=250  Resume toggle            (h=26)
//   Total minimum height: ~280
// ─────────────────────────────────────────────────────────────────────────────
void SlotEditorComponent::resized()
{
    const int pad  = 4;
    const int lW   = 52; // label width
    const int ctrlX = pad + lW + 4;
    const int ctrlW = getWidth() - ctrlX - pad;
    const int rowH  = 27;
    const int step  = rowH + 4;

    // ── Buttons (y=30) ───────────────────────────────────────────────────────
    {
        const int btnY = 30;
        const int btnH = 30;
        const int gap  = 4;
        const int bW   = (getWidth() - 2 * pad - 2 * gap) / 3;
        recBtn  .setBounds(pad,              btnY, bW, btnH);
        playBtn .setBounds(pad + bW + gap,   btnY, bW, btnH);
        clearBtn.setBounds(pad + 2*(bW+gap), btnY, bW, btnH);
    }

    // ── Timeline (y=64) ──────────────────────────────────────────────────────
    timeline.setBounds(pad, 64, getWidth() - 2*pad, 54);

    // ── Sliders (y=122) ──────────────────────────────────────────────────────
    {
        const int y0 = 122;
        startLabel .setBounds(pad, y0 + 0*step, lW, rowH);
        endLabel   .setBounds(pad, y0 + 1*step, lW, rowH);
        speedLabel .setBounds(pad, y0 + 2*step, lW, rowH);

        startSlider.setBounds(ctrlX, y0 + 0*step, ctrlW, rowH);
        endSlider  .setBounds(ctrlX, y0 + 1*step, ctrlW, rowH);
        speedSlider.setBounds(ctrlX, y0 + 2*step, ctrlW, rowH);
    }

    // ── Loop buttons (y=122+3*31=215) ────────────────────────────────────────
    {
        const int loopY  = 122 + 3 * step;
        const int gap    = 3;
        const int availW = getWidth() - 2*pad - lW - 4;
        const int bW     = (availW - 3*gap) / 4;
        loopLabel.setBounds(pad, loopY, lW, rowH);
        for (int k = 0; k < 4; ++k)
            loopBtns[k].setBounds(pad + lW + 4 + k*(bW + gap), loopY, bW, rowH);
    }

    // ── Resume toggle (y=122+4*31=246) ───────────────────────────────────────
    {
        const int priY = 122 + 4 * step;
        resumeToggle.setBounds(pad, priY, getWidth() - 2*pad, 26);
    }
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
