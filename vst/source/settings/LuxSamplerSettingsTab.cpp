#include "LuxSamplerSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"
#include "../luxsampler/LuxSampler.h"

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

LuxSamplerSettingsTab::LuxSamplerSettingsTab(Sp3ctraAudioProcessor& processor)
    : audioProcessor(processor),
      apvts(processor.getAPVTS())
{
    // ── Enable toggle ─────────────────────────────────────────────────────
    enableLabel.setText("LuxSampler:", juce::dontSendNotification);
    enableLabel.setJustificationType(juce::Justification::centredRight);
    enableLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(enableLabel);

    enableToggle.setButtonText("Enabled");
    addAndMakeVisible(enableToggle);
    enableAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxSamplerEnabled", enableToggle);

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
        apvts, "luxSamplerMidiChannel", midiChannelCombo);

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
        apvts, "luxSamplerOctaveOffset", octaveOffsetCombo);

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
        apvts, "luxSamplerMaxDuration", maxDurationSlider);

    // ── Image export on Save Session ──────────────────────────────────────
    exportImagesLabel.setText("Export Images:", juce::dontSendNotification);
    exportImagesLabel.setJustificationType(juce::Justification::centredRight);
    exportImagesLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(exportImagesLabel);

    exportImagesToggle.setButtonText("Export PNG/JPEG on Save Session");
    exportImagesToggle.setTooltip(
        "When enabled, clicking SAVE SESSION also exports every non-empty "
        "slot as an image (one row per captured CIS line) next to the .fsmp file.");
    addAndMakeVisible(exportImagesToggle);
    exportImagesAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxSamplerExportImages", exportImagesToggle);

    exportFormatLabel.setText("Format:", juce::dontSendNotification);
    exportFormatLabel.setJustificationType(juce::Justification::centredRight);
    exportFormatLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(exportFormatLabel);

    exportFormatCombo.addItem("PNG",  1);
    exportFormatCombo.addItem("JPEG", 2);
    addAndMakeVisible(exportFormatCombo);
    exportFormatAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxSamplerExportFormat", exportFormatCombo);

    // ── Output Directory (shared by SAVE SESSION and image export) ────────
    outputDirLabel.setText("Output Dir:", juce::dontSendNotification);
    outputDirLabel.setJustificationType(juce::Justification::centredRight);
    outputDirLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(outputDirLabel);

    outputDirValueLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
    outputDirValueLabel.setJustificationType(juce::Justification::centredLeft);
    outputDirValueLabel.setColour(juce::Label::backgroundColourId,
                                  juce::Colours::black.withAlpha(0.4f));
    outputDirValueLabel.setColour(juce::Label::outlineColourId,
                                  juce::Colours::grey.withAlpha(0.5f));
    outputDirValueLabel.setMinimumHorizontalScale(0.5f);
    outputDirValueLabel.setTooltip(
        "Folder used by SAVE SESSION for the .sp3s file and the optional "
        "PNG/JPEG image exports. Leave empty to be prompted each time.");
    addAndMakeVisible(outputDirValueLabel);

    outputDirBrowseBtn.setButtonText("Browse...");
    outputDirBrowseBtn.onClick = [this]()
    {
        const juce::String current = audioProcessor.getSamplerOutputDir();
        juce::File startDir = current.isNotEmpty()
            ? juce::File(current)
            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        if (! startDir.isDirectory())
            startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

        outputDirChooser = std::make_unique<juce::FileChooser>(
            "Choose output directory for LuxSampler sessions and images",
            startDir);
        const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectDirectories;
        outputDirChooser->launchAsync(flags, [this](const juce::FileChooser& fc)
        {
            const juce::File chosen = fc.getResult();
            if (chosen != juce::File{} && chosen.isDirectory())
            {
                const juce::String full = chosen.getFullPathName();
                audioProcessor.setSamplerOutputDir(full);
                outputDirValueLabel.setText(full, juce::dontSendNotification);
            }
        });
    };
    addAndMakeVisible(outputDirBrowseBtn);

    outputDirClearBtn.setButtonText("X");
    outputDirClearBtn.setTooltip("Clear output directory - SAVE SESSION will prompt again.");
    outputDirClearBtn.onClick = [this]()
    {
        audioProcessor.setSamplerOutputDir({});
        outputDirValueLabel.setText("(not set - file chooser will be used)",
                                    juce::dontSendNotification);
    };
    addAndMakeVisible(outputDirClearBtn);

    // Initialise value label from APVTS-restored value
    {
        const juce::String cur = audioProcessor.getSamplerOutputDir();
        outputDirValueLabel.setText(
            cur.isNotEmpty() ? cur : juce::String("(not set - file chooser will be used)"),
            juce::dontSendNotification);
    }

    // ── Action button MIDI bindings (REC / PLAY / SAVE) ───────────────────
    // Each row exposes a Type combo (Off/Note/CC), a 0..127 number slider and
    // a "Learn" button.  The audio thread captures the next incoming MIDI
    // event matching the configured channel and writes it into the APVTS
    // parameters via the message thread (timerCallback).
    initBindingRow(recBinding,  "REC Bind:",
                   "luxSamplerRecBindType",  "luxSamplerRecBindNum",  0);
    initBindingRow(playBinding, "PLAY Bind:",
                   "luxSamplerPlayBindType", "luxSamplerPlayBindNum", 1);
    initBindingRow(saveBinding, "SAVE Bind:",
                   "luxSamplerSaveBindType", "luxSamplerSaveBindNum", 2);

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
            if (auto* fs = audioProcessor.getLuxSampler())
                fs->clearSlot(i);
        };
        addAndMakeVisible(slotClearBtn[i]);
    }

    startTimerHz(10); // Refresh at 10 Hz
}

LuxSamplerSettingsTab::~LuxSamplerSettingsTab()
{
    stopTimer();
}

// =============================================================================
// Timer callback — refresh slot displays
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// initBindingRow — wire one REC/PLAY/SAVE row to the matching APVTS params.
// ─────────────────────────────────────────────────────────────────────────────
void LuxSamplerSettingsTab::initBindingRow(ActionBindingRow& row,
                                           const juce::String& title,
                                           const juce::String& typeParamId,
                                           const juce::String& numParamId,
                                           int learnTargetId)
{
    row.title.setText(title, juce::dontSendNotification);
    row.title.setJustificationType(juce::Justification::centredRight);
    row.title.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(row.title);

    // Type combo (Off / Note / CC)
    row.typeBox.addItem("Off",  1);
    row.typeBox.addItem("Note", 2);
    row.typeBox.addItem("CC",   3);
    addAndMakeVisible(row.typeBox);
    row.typeAtt = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, typeParamId, row.typeBox);

    // Number slider (0..127) — shown as "Note 60" / "CC 7" via text-from-value
    row.numberSlider.setSliderStyle(juce::Slider::IncDecButtons);
    row.numberSlider.setIncDecButtonsMode(
        juce::Slider::incDecButtonsDraggable_Vertical);
    row.numberSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false,
                                      Sp3ctraTheme::kTbXNarrow,
                                      Sp3ctraTheme::kTextBoxH);
    row.numberSlider.setRange(0.0, 127.0, 1.0);
    addAndMakeVisible(row.numberSlider);
    row.numberAtt = std::make_unique<
        juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, numParamId, row.numberSlider);

    // Learn button — arms the processor to capture next matching MIDI event
    row.learnBtn.setClickingTogglesState(true);
    row.learnBtn.setColour(juce::TextButton::buttonOnColourId,
                            juce::Colours::orangered);
    row.learnBtn.setTooltip(
        "Click then press any Note or send a CC on the configured MIDI channel "
        "to assign it to this action.");
    row.learnBtn.onClick = [this, learnTargetId, &row]()
    {
        if (row.learnBtn.getToggleState())
        {
            // Cancel any other pending learn (only one at a time).
            audioProcessor.startSamplerMidiLearn(learnTargetId);
            pendingLearnTarget = learnTargetId;
            recBinding .learnBtn.setToggleState(learnTargetId == 0, juce::dontSendNotification);
            playBinding.learnBtn.setToggleState(learnTargetId == 1, juce::dontSendNotification);
            saveBinding.learnBtn.setToggleState(learnTargetId == 2, juce::dontSendNotification);
        }
        else
        {
            audioProcessor.cancelSamplerMidiLearn();
            pendingLearnTarget = -1;
        }
    };
    addAndMakeVisible(row.learnBtn);
}

void LuxSamplerSettingsTab::timerCallback()
{
    updateSlotDisplays();

    // ── MIDI Learn polling ───────────────────────────────────────────────────
    // The audio thread writes the captured event into samplerMidiLearnResult
    // (encoded as (type << 8) | number) and resets the target to -1 once done.
    // On the message thread we copy the captured values into the APVTS so the
    // ComboBox/Slider attachments reflect the new binding and trigger param
    // change notifications to the host.
    const int currentTarget = audioProcessor.getSamplerMidiLearnTarget();
    const int result        = audioProcessor.getSamplerMidiLearnResult();

    if (currentTarget == -1 && result != -1 && pendingLearnTarget != -1)
    {
        const int type   = (result >> 8) & 0xFF; // 1 = Note, 2 = CC
        const int number = result & 0xFF;

        const char* typeParam = nullptr;
        const char* numParam  = nullptr;
        switch (pendingLearnTarget)
        {
            case 0: typeParam = "luxSamplerRecBindType";
                    numParam  = "luxSamplerRecBindNum";  break;
            case 1: typeParam = "luxSamplerPlayBindType";
                    numParam  = "luxSamplerPlayBindNum"; break;
            case 2: typeParam = "luxSamplerSaveBindType";
                    numParam  = "luxSamplerSaveBindNum"; break;
            default: break;
        }

        if (typeParam != nullptr)
        {
            // ComboBox indices: 0=Off, 1=Note, 2=CC — matches captured type.
            if (auto* p = apvts.getParameter(typeParam))
                p->setValueNotifyingHost(p->convertTo0to1((float)type));
            if (auto* p = apvts.getParameter(numParam))
                p->setValueNotifyingHost(p->convertTo0to1((float)number));
        }

        audioProcessor.clearSamplerMidiLearnResult();
        pendingLearnTarget = -1;
        recBinding .learnBtn.setToggleState(false, juce::dontSendNotification);
        playBinding.learnBtn.setToggleState(false, juce::dontSendNotification);
        saveBinding.learnBtn.setToggleState(false, juce::dontSendNotification);
    }

    // Keep the toggle visible state in sync if Learn was cancelled elsewhere
    if (currentTarget == -1 && result == -1 && pendingLearnTarget != -1)
    {
        pendingLearnTarget = -1;
        recBinding .learnBtn.setToggleState(false, juce::dontSendNotification);
        playBinding.learnBtn.setToggleState(false, juce::dontSendNotification);
        saveBinding.learnBtn.setToggleState(false, juce::dontSendNotification);
    }
}


void LuxSamplerSettingsTab::updateSlotDisplays()
{
    auto* fs = audioProcessor.getLuxSampler();
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

void LuxSamplerSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("LuxSampler", getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);

    // Column headers for slot grid — position must match resized() exactly:
    //   titleH(30) + 5 + 7 control rows (Enable, MIDI, Octave, MaxDur,
    //   ExportToggle, ExportFormat, OutputDir) + 3 binding rows
    //   (REC / PLAY / SAVE) * kRowStep + kHPad
    const int titleH  = 30;
    const int headerY = titleH + 5 + 10 * Sp3ctraTheme::kRowStep + Sp3ctraTheme::kHPad;


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

void LuxSamplerSettingsTab::resized()
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
    row(exportImagesLabel, exportImagesToggle);
    row(exportFormatLabel, exportFormatCombo);

    // ── Output Directory row (special layout: value field + 2 buttons) ────
    {
        const int vc       = (rowH - ctrlH) / 2;
        const int browseW  = 80;
        const int clearW   = 28;
        const int btnGap   = 4;
        const int valueW   = ctrlW - browseW - clearW - 2 * btnGap;

        outputDirLabel.setBounds(20, y + vc, labelW, ctrlH);
        outputDirValueLabel.setBounds(ctrlX, y + vc, valueW, ctrlH);
        outputDirBrowseBtn .setBounds(ctrlX + valueW + btnGap, y + vc, browseW, ctrlH);
        outputDirClearBtn  .setBounds(ctrlX + valueW + btnGap + browseW + btnGap,
                                       y + vc, clearW, ctrlH);
        y += rowH;
    }

    // ── REC / PLAY / SAVE binding rows ──────────────────────────────────────
    // Layout per row:  [LABEL] [Type combo] [Number slider] [LEARN btn]
    auto bindingRow = [&](ActionBindingRow& br)
    {
        const int vc       = (rowH - ctrlH) / 2;
        const int typeW    = 70;
        const int learnW   = 70;
        const int gap      = 4;
        const int numberW  = ctrlW - typeW - learnW - 2 * gap;

        br.title       .setBounds(20,                            y + vc, labelW,  ctrlH);
        br.typeBox     .setBounds(ctrlX,                         y + vc, typeW,   ctrlH);
        br.numberSlider.setBounds(ctrlX + typeW + gap,           y + vc, numberW, ctrlH);
        br.learnBtn    .setBounds(ctrlX + typeW + gap + numberW + gap,
                                   y + vc, learnW,  ctrlH);
        y += rowH;
    };
    bindingRow(recBinding);
    bindingRow(playBinding);
    bindingRow(saveBinding);

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
