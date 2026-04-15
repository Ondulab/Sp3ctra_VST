#include "FrameSamplerSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"
#include "../framesampler/FrameSampler.h"

// Note names for slot index labels (C0..B0 for REC, C1..B1 for PLAY)
static const char* const kNoteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// ── Colour helpers ────────────────────────────────────────────────────────────
static juce::Colour stateColour(SlotState s)
{
    switch (s)
    {
        case SlotState::ARMED:     return juce::Colours::yellow;
        case SlotState::RECORDING: return juce::Colours::red;
        case SlotState::PLAYING:   return juce::Colours::limegreen;
        default:                   return juce::Colours::grey;
    }
}

static const char* stateText(SlotState s)
{
    switch (s)
    {
        case SlotState::ARMED:     return "ARMED";
        case SlotState::RECORDING: return "REC *";
        case SlotState::PLAYING:   return "PLAY >";
        default:                   return "idle";
    }
}

// =============================================================================
// Constructor
// =============================================================================

FrameSamplerSettingsTab::FrameSamplerSettingsTab(Sp3ctraAudioProcessor& processor)
    : audioProcessor(processor),
      apvts(processor.getAPVTS())
{
    // ── Enable toggle ─────────────────────────────────────────────────────
    enableLabel.setText("FrameSampler:", juce::dontSendNotification);
    enableLabel.setJustificationType(juce::Justification::centredRight);
    enableLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(enableLabel);

    enableToggle.setButtonText("Enabled");
    addAndMakeVisible(enableToggle);
    enableAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "frameSamplerEnabled", enableToggle);

    // ── MIDI Channel ──────────────────────────────────────────────────────
    midiChannelLabel.setText("MIDI Channel:", juce::dontSendNotification);
    midiChannelLabel.setJustificationType(juce::Justification::centredRight);
    midiChannelLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(midiChannelLabel);

    for (int i = 1; i <= 16; ++i)
        midiChannelCombo.addItem("Channel " + juce::String(i), i);
    addAndMakeVisible(midiChannelCombo);
    midiChannelAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "frameSamplerMidiChannel", midiChannelCombo);

    // ── Octave Offset ─────────────────────────────────────────────────────
    octaveOffsetLabel.setText("Octave Offset:", juce::dontSendNotification);
    octaveOffsetLabel.setJustificationType(juce::Justification::centredRight);
    octaveOffsetLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(octaveOffsetLabel);

    octaveOffsetCombo.addItem("-2", 1);
    octaveOffsetCombo.addItem("-1", 2);
    octaveOffsetCombo.addItem(" 0", 3);
    octaveOffsetCombo.addItem("+1", 4);
    octaveOffsetCombo.addItem("+2", 5);
    addAndMakeVisible(octaveOffsetCombo);
    octaveOffsetAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "frameSamplerOctaveOffset", octaveOffsetCombo);

    // ── Max Duration ──────────────────────────────────────────────────────
    maxDurationLabel.setText("Max Duration:", juce::dontSendNotification);
    maxDurationLabel.setJustificationType(juce::Justification::centredRight);
    maxDurationLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(maxDurationLabel);

    maxDurationSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    maxDurationSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                       Sp3ctraTheme::kTbXNarrow, Sp3ctraTheme::kTextBoxH);
    maxDurationSlider.setTextValueSuffix(" s");
    addAndMakeVisible(maxDurationSlider);
    maxDurationAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "frameSamplerMaxDuration", maxDurationSlider);

    // ── Slot rows ─────────────────────────────────────────────────────────
    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        // Index label: "C0 / C1"
        juce::String idxText = juce::String(kNoteNames[i]) + "0 / "
                             + juce::String(kNoteNames[i]) + "1";
        slotIndexLabel[i].setText(idxText, juce::dontSendNotification);
        slotIndexLabel[i].setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        slotIndexLabel[i].setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(slotIndexLabel[i]);

        // State label
        slotStateLabel[i].setText("idle", juce::dontSendNotification);
        slotStateLabel[i].setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        slotStateLabel[i].setJustificationType(juce::Justification::centred);
        slotStateLabel[i].setColour(juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible(slotStateLabel[i]);

        // Duration label
        slotDurLabel[i].setText("0.0 s", juce::dontSendNotification);
        slotDurLabel[i].setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        slotDurLabel[i].setJustificationType(juce::Justification::centred);
        addAndMakeVisible(slotDurLabel[i]);

        // Clear button
        slotClearBtn[i].setButtonText("X");
        slotClearBtn[i].onClick = [this, i]()
        {
            if (auto* fs = audioProcessor.getFrameSampler())
                fs->clearSlot(i);
        };
        addAndMakeVisible(slotClearBtn[i]);
    }

    startTimerHz(10); // Refresh at 10 Hz
}

FrameSamplerSettingsTab::~FrameSamplerSettingsTab()
{
    stopTimer();
}

// =============================================================================
// Timer callback — refresh slot displays
// =============================================================================

void FrameSamplerSettingsTab::timerCallback()
{
    updateSlotDisplays();
}

void FrameSamplerSettingsTab::updateSlotDisplays()
{
    auto* fs = audioProcessor.getFrameSampler();
    if (fs == nullptr) return;

    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        const SlotState state = fs->getSlotState(i);
        const uint64_t  dur   = fs->getSlotDurationUs(i);

        // State label text + colour
        slotStateLabel[i].setText(stateText(state), juce::dontSendNotification);
        slotStateLabel[i].setColour(juce::Label::textColourId, stateColour(state));

        // Duration (only show if slot has content or is recording)
        if (state == SlotState::RECORDING)
        {
            // Show elapsed recording time approximately
            slotDurLabel[i].setText("rec...", juce::dontSendNotification);
        }
        else if (fs->slotHasContent(i))
        {
            const double secs = static_cast<double>(dur) / 1e6;
            slotDurLabel[i].setText(juce::String(secs, 1) + " s",
                                    juce::dontSendNotification);
        }
        else
        {
            slotDurLabel[i].setText("-", juce::dontSendNotification);
        }

        // Enable/disable clear button
        slotClearBtn[i].setEnabled(fs->slotHasContent(i)
                                   || state != SlotState::IDLE);
    }
}

// =============================================================================
// paint
// =============================================================================

void FrameSamplerSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("FrameSampler", getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);

    // Column headers for slot grid — position must match resized() exactly:
    //   titleH(30) + 5 + 4*kRowStep + kHPad = 30+5+104+10 = 149
    const int titleH  = 30;
    const int headerY = titleH + 5 + 4 * Sp3ctraTheme::kRowStep + Sp3ctraTheme::kHPad;
    const int headerH = 20;

    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
    g.setColour(juce::Colours::lightgrey);

    auto headerBounds = juce::Rectangle<int>(10, headerY, getWidth() - 20, headerH);
    const int colW    = (getWidth() - 20) / 4;
    g.drawText("Note (REC/PLAY)",  headerBounds.removeFromLeft(colW), juce::Justification::centredLeft, true);
    g.drawText("State",             headerBounds.removeFromLeft(colW), juce::Justification::centred, true);
    g.drawText("Duration",          headerBounds.removeFromLeft(colW), juce::Justification::centred, true);
    g.drawText("",                  headerBounds,                      juce::Justification::centred, true);

    // Separator line below headers
    g.setColour(juce::Colours::grey.withAlpha(0.5f));
    g.drawHorizontalLine(headerY + headerH, 10.0f, static_cast<float>(getWidth() - 10));
}

// =============================================================================
// resized
// =============================================================================

void FrameSamplerSettingsTab::resized()
{
    const int w        = getWidth();
    const int titleH   = 30;
    constexpr int rowH   = Sp3ctraTheme::kRowStep;   // 32
    constexpr int pad    = Sp3ctraTheme::kHPad;       // 10
    constexpr int labelW = Sp3ctraTheme::kLabelW;     // 110
    const int ctrlX    = 20 + labelW;
    const int ctrlW    = w - ctrlX - 20;

    int y = titleH + 5;

    // ── Controls ──────────────────────────────────────────────────────────
    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    auto row = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        const int vc = (rowH - ctrlH) / 2; // vertical centre offset
        lbl .setBounds(20,    y + vc, labelW, ctrlH);
        ctrl.setBounds(ctrlX, y + vc, ctrlW,  ctrlH);
        y += rowH;
    };

    row(enableLabel,      enableToggle);
    row(midiChannelLabel, midiChannelCombo);
    row(octaveOffsetLabel,octaveOffsetCombo);
    row(maxDurationLabel, maxDurationSlider);

    y += pad; // gap before slot grid

    // ── Slot grid header placeholder (painted) ───────────────────────────
    const int headerH = 20;
    y += headerH + 4; // skip header row (painted in paint())

    // ── Slot rows ─────────────────────────────────────────────────────────
    const int slotRowH = 24;
    const int colW     = (w - 20) / 4;

    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        int x = 10;
        slotIndexLabel[i].setBounds(x, y, colW, slotRowH); x += colW;
        slotStateLabel[i].setBounds(x, y, colW, slotRowH); x += colW;
        slotDurLabel[i]  .setBounds(x, y, colW, slotRowH); x += colW;
        slotClearBtn[i]  .setBounds(x + 4, y + 2, colW - 12, slotRowH - 4);
        y += slotRowH;
    }

}
