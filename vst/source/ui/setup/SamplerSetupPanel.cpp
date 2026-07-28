#include "SamplerSetupPanel.h"
#include "SetupHeader.h"
#include "../../Sp3ctraConstants.h"
#include "../../UITheme.h"
#include "../../Sp3ctraDialog.h"          // destructive-action confirmations
#include "../../luxsampler/LuxSampler.h"

// ── Colour helpers ────────────────────────────────────────────────────────────
static juce::Colour stateColour(SlotState s)
{
    switch (s)
    {
        case SlotState::RECORDING: return juce::Colours::red;
        case SlotState::PLAYING:   return juce::Colours::limegreen;
        default:                   return juce::Colours::grey;
    }
}

static const char* stateText(SlotState s)
{
    switch (s)
    {
        case SlotState::RECORDING: return "REC *";
        case SlotState::PLAYING:   return "PLAY >";
        default:                   return "idle";
    }
}

// =============================================================================
// Constructor
// =============================================================================

SamplerSetupPanel::SamplerSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : audioProcessor(processor),
      apvts(processor.getAPVTS()),
      accent(accentColour)
{
    // ── Enable toggle ── moved to the rack LED + zone-3 header power switch

    // ── Number of banks (1..8) ────────────────────────────────────────────
    banksLabel.setText("Banks:", juce::dontSendNotification);
    banksLabel.setJustificationType(juce::Justification::centredRight);
    banksLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(banksLabel);

    for (int i = 1; i <= LuxSamplerConstants::MAX_UI_BANKS; ++i)
        banksCombo.addItem(juce::String(i), i);
    banksCombo.setTooltip("Number of banks shown in the SAMPLER page (1-6). "
                          "Several banks can play simultaneously; each has its "
                          "own mixer fader and mix mode under its tile.");
    addAndMakeVisible(banksCombo);   // attachment: rebindEngineParams()

    // ── Max Duration ──────────────────────────────────────────────────────
    maxDurationLabel.setText("Max Duration:", juce::dontSendNotification);
    maxDurationLabel.setJustificationType(juce::Justification::centredRight);
    maxDurationLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(maxDurationLabel);

    maxDurationSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    maxDurationSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                       Sp3ctraTheme::kTbXNarrow, Sp3ctraTheme::kTextBoxH);
    maxDurationSlider.setTextValueSuffix(" s");
    addAndMakeVisible(maxDurationSlider);   // attachment: rebindEngineParams()

    // ── REC button mode (Toggle vs Momentary) ────────────────────────────
    recModeLabel.setText("REC Mode:", juce::dontSendNotification);
    recModeLabel.setJustificationType(juce::Justification::centredRight);
    recModeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(recModeLabel);

    recModeCombo.addItem("Toggle",    1);
    recModeCombo.addItem("Momentary", 2);
    recModeCombo.setTooltip(
        "REC button behaviour: Toggle = click to start, click again to stop. "
        "Momentary = record only while the button (or a mapped MIDI key) is held.");
    addAndMakeVisible(recModeCombo);   // attachment: rebindEngineParams()

    // ── PLAY button mode (Toggle vs Momentary) ───────────────────────────
    playModeLabel.setText("PLAY Mode:", juce::dontSendNotification);
    playModeLabel.setJustificationType(juce::Justification::centredRight);
    playModeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(playModeLabel);

    playModeCombo.addItem("Toggle",    1);
    playModeCombo.addItem("Momentary", 2);
    playModeCombo.setTooltip(
        "PLAY button behaviour: Toggle = click to start, click again to stop. "
        "Momentary = play only while the button (or a mapped MIDI key) is held.");
    addAndMakeVisible(playModeCombo);   // attachment: rebindEngineParams()

    // ── Image export on slot SAVE ─────────────────────────────────────────
    // (The former .sp3s "Save Session" flow and its shared Output Dir were
    // retired with the project-session model: slot saves/exports now land in
    // the working session's exports/ folder automatically.)
    exportImagesLabel.setText("Export Images:", juce::dontSendNotification);
    exportImagesLabel.setJustificationType(juce::Justification::centredRight);
    exportImagesLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(exportImagesLabel);

    exportImagesToggle.setButtonText("Export PNG/JPEG on slot SAVE");
    exportImagesToggle.setTooltip(
        "When enabled, saving a slot (.fslot) also exports its content "
        "as an image (one row per captured CIS line) next to the file.");
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

    // REC / PLAY / SAVE MIDI triggering now lives on the editor's transport
    // buttons via the unified right-click MIDI-Learn (no bindings panel here).

    // Bind every engine-scoped attachment to engine A's bank (default).
    rebindEngineParams();

    // ── Slot rows ─────────────────────────────────────────────────────────
    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        // Index label: banks are numbered — no more note addressing.
        slotIndexLabel[i].setText("Bank " + juce::String(i + 1),
                                  juce::dontSendNotification);
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

        // Clear button — destructive: confirm when the bank holds audio
        slotClearBtn[i].setButtonText("X");
        slotClearBtn[i].onClick = [this, i]()
        {
            auto* fs = audioProcessor.getSampler(samplerIndex_);
            if (fs == nullptr || ! fs->slotHasContent(i))
                return;
            Sp3ctraDialog::showConfirm(
                this, "Clear bank",
                ("Clear bank " + juce::String(i + 1)
                 + "? The recorded audio is discarded.").toRawUTF8(),
                "Clear", "Cancel",
                [safe = juce::Component::SafePointer<SamplerSetupPanel>(this), i]
                (bool ok)
                {
                    if (! ok) return;
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    if (auto* eng = self->audioProcessor.getSampler(
                            self->samplerIndex_))
                    {
                        eng->clearSlot(i);
                        self->audioProcessor.sessions()->markBanksDirty();
                    }
                });
        };
        addAndMakeVisible(slotClearBtn[i]);
    }

    startTimerHz(10); // Refresh at 10 Hz
}

SamplerSetupPanel::~SamplerSetupPanel()
{
    stopTimer();
}

// =============================================================================
// Timer callback — refresh slot displays
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// setSamplerIndex / rebindEngineParams — per-engine APVTS bank binding
// ─────────────────────────────────────────────────────────────────────────────
void SamplerSetupPanel::setSamplerIndex(int i)
{
    if (samplerIndex_ == i)
        return;
    samplerIndex_ = i;
    rebindEngineParams();
}

void SamplerSetupPanel::rebindEngineParams()
{
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;

    banksAttachment       .reset();
    maxDurationAttachment .reset();
    recModeAttachment     .reset();
    playModeAttachment    .reset();
    banksAttachment = std::make_unique<CA>(
        apvts, fsEngineParam(samplerIndex_, "NumBanks"),     banksCombo);
    maxDurationAttachment = std::make_unique<SA>(
        apvts, fsEngineParam(samplerIndex_, "MaxDuration"),  maxDurationSlider);
    recModeAttachment = std::make_unique<CA>(
        apvts, fsEngineParam(samplerIndex_, "RecMode"),  recModeCombo);
    playModeAttachment = std::make_unique<CA>(
        apvts, fsEngineParam(samplerIndex_, "PlayMode"), playModeCombo);
}

void SamplerSetupPanel::timerCallback()
{
    updateSlotDisplays();
}

void SamplerSetupPanel::updateSlotDisplays()
{
    auto* fs = audioProcessor.getSampler(samplerIndex_);
    if (fs == nullptr) return;

    // Only the first N banks (SETUP "Banks" choice: index 0..7 → 1..8) are
    // reachable from the bank grid — hide the rows beyond.
    int numBanks = LuxSamplerConstants::MAX_UI_BANKS;
    if (auto* p = apvts.getRawParameterValue(
            fsEngineParam(samplerIndex_, "NumBanks")))
        numBanks = juce::jlimit(1, LuxSamplerConstants::MAX_UI_BANKS,
                                static_cast<int>(p->load()) + 1);

    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        const bool visible = (i < numBanks);
        slotIndexLabel[i].setVisible(visible);
        slotStateLabel[i].setVisible(visible);
        slotDurLabel[i]  .setVisible(visible);
        slotClearBtn[i]  .setVisible(visible);
        if (! visible) continue;

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

void SamplerSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "SAMPLER -- SETUP", accent);

    // Column headers for slot grid — position must match resized() exactly:
    //   headerH(30) + 7 control rows (Banks, MaxDur, RecMode, PlayMode,
    //   ExportToggle, ExportFormat, OutputDir) * kRowStep + kHPad.
    const int titleH  = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;
    const int headerY = titleH + 7 * Sp3ctraTheme::kRowStep + Sp3ctraTheme::kHPad;
    const int headerH = 20;

    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
    g.setColour(juce::Colours::lightgrey);

    auto headerBounds = juce::Rectangle<int>(10, headerY, getWidth() - 20, headerH);
    const int colW    = (getWidth() - 20) / 4;
    g.drawText("Bank",              headerBounds.removeFromLeft(colW), juce::Justification::centredLeft, true);
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

void SamplerSetupPanel::resized()
{
    const int w        = getWidth();
    const int titleH   = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;
    constexpr int rowH   = Sp3ctraTheme::kRowStep;   // 32
    constexpr int pad    = Sp3ctraTheme::kHPad;       // 10
    constexpr int labelW = Sp3ctraTheme::kLabelW;     // 110
    const int ctrlX    = pad + labelW + Sp3ctraTheme::kGap;
    const int ctrlW    = juce::jmax(120, w - ctrlX - 20);

    int y = titleH;

    // ── Controls ──────────────────────────────────────────────────────────
    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    auto row = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        const int vc = (rowH - ctrlH) / 2; // vertical centre offset
        lbl .setBounds(pad,   y + vc, labelW, ctrlH);
        ctrl.setBounds(ctrlX, y + vc, ctrlW,  ctrlH);
        y += rowH;
    };

    row(banksLabel,       banksCombo);
    row(maxDurationLabel, maxDurationSlider);
    row(recModeLabel,     recModeCombo);
    row(playModeLabel,    playModeCombo);
    row(exportImagesLabel, exportImagesToggle);
    row(exportFormatLabel, exportFormatCombo);
    // (The Output Directory row was retired with the project-session model:
    // slot saves/exports land in the working session's exports/ folder.)

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
