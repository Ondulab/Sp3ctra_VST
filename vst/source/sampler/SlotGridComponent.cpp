#include "SlotGridComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleParamManifest.h"   // fsEngineParam (SETUP "Banks" count)
#include "SamplerMidiTargets.h"

SlotGridComponent::SlotGridComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    using namespace LuxSamplerConstants;

    for (int i = 0; i < MAX_UI_BANKS; ++i)
    {
        // ── Level fader — fade to white (engine param = 1 − brightnessLift) ──
        auto& sl = levelSlider[i];
        sl.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        sl.setRange(0.0, 1.0, 0.01);
        sl.setValue(1.0, juce::dontSendNotification);
        sl.setColour(juce::Slider::trackColourId, juce::Colour(0xffcc88ff).withAlpha(0.55f));
        sl.setColour(juce::Slider::thumbColourId, juce::Colour(0xffcc88ff));
        sl.setTooltip("Bank " + juce::String(i + 1)
                      + " level: fades the bank to white (silence) in the mix");
        sl.onValueChange = [this, i]
        {
            if (auto* fs = processor.getSampler(samplerIndex_))
                fs->setSlotBrightnessLift(i,
                    1.0f - static_cast<float>(levelSlider[i].getValue()));
        };
        addAndMakeVisible(sl);

        // ── Mix-mode box — composite rule vs the other playing banks ─────────
        auto& box = modeBox[i];
        box.addItem("MIX",  1 + static_cast<int>(SlotMixMode::MIX));
        box.addItem("ADD",  1 + static_cast<int>(SlotMixMode::ADD));
        box.addItem("DARK", 1 + static_cast<int>(SlotMixMode::DARKEN));
        box.setSelectedId(1 + static_cast<int>(SlotMixMode::DARKEN),
                          juce::dontSendNotification);
        box.setTooltip("Bank " + juce::String(i + 1)
                       + " mix mode: MIX = blend (level = opacity), "
                         "ADD = energies add up, DARK = darkest pixel wins");
        box.onChange = [this, i]
        {
            const int id = modeBox[i].getSelectedId();
            if (id > 0)
                if (auto* fs = processor.getSampler(samplerIndex_))
                    fs->setSlotMixMode(i, static_cast<SlotMixMode>(id - 1));
        };
        addAndMakeVisible(box);
    }

    rebindMidiLearn();
    startTimer(100); // 10 Hz — drives blink + mixer resync + repaint
}

SlotGridComponent::~SlotGridComponent()
{
    stopTimer();
}

void SlotGridComponent::setSelectedSlot(int idx) noexcept
{
    const int clamped = juce::jlimit(0, numBanks() - 1, idx);
    if (clamped != selectedSlot)
    {
        selectedSlot = clamped;
        repaint();
    }
}

void SlotGridComponent::setSamplerIndex(int i)
{
    if (samplerIndex_ == i) return;
    samplerIndex_ = i;
    rebindMidiLearn();
    refreshMixerValues();
    repaint();
}

int SlotGridComponent::numBanks() const
{
    using namespace LuxSamplerConstants;
    if (auto* p = processor.getAPVTS().getRawParameterValue(
            fsEngineParam(samplerIndex_, "NumBanks")))
        return juce::jlimit(1, MAX_UI_BANKS, static_cast<int>(p->load()) + 1);
    return MAX_UI_BANKS;
}

juce::Rectangle<int> SlotGridComponent::cellBounds(int i) const noexcept
{
    constexpr int kGap = 3;
    const int n         = juce::jmax(1, numBanks());
    const int totalGaps = (n - 1) * kGap;
    const int cellW     = (getWidth() - totalGaps) / n;
    const int cellH     = getHeight() - kUnderH - 2 * (kMixRowH + kMixRowGap);
    return { i * (cellW + kGap), 0, cellW, cellH };
}

void SlotGridComponent::layoutMixerRow()
{
    using namespace LuxSamplerConstants;
    const int n = numBanks();
    lastNumBanks_ = n;

    if (selectedSlot >= n)
    {
        // The SETUP count shrank below the current selection — reselect the
        // last visible bank (and let the editor follow).
        selectedSlot = n - 1;
        if (onSlotSelected) onSlotSelected(selectedSlot);
    }

    for (int i = 0; i < MAX_UI_BANKS; ++i)
    {
        const bool visible = (i < n);
        levelSlider[i].setVisible(visible);
        modeBox[i]    .setVisible(visible);
        if (! visible) continue;

        const auto cell   = cellBounds(i);
        const int  levelY = cell.getBottom() + kMixRowGap;
        const int  modeY  = levelY + kMixRowH + kMixRowGap;
        levelSlider[i].setBounds(cell.getX(), levelY, cell.getWidth(), kMixRowH);
        modeBox[i]    .setBounds(cell.getX(), modeY,  cell.getWidth(), kMixRowH);
    }
}

void SlotGridComponent::refreshMixerValues()
{
    using namespace LuxSamplerConstants;
    auto* fs = processor.getSampler(samplerIndex_);
    if (fs == nullptr) return;

    for (int i = 0; i < MAX_UI_BANKS; ++i)
    {
        // Don't fight an active drag — MIDI/preset moves land next tick.
        if (! levelSlider[i].isMouseButtonDown())
            levelSlider[i].setValue(
                1.0 - static_cast<double>(fs->getSlotBrightnessLift(i)),
                juce::dontSendNotification);
        modeBox[i].setSelectedId(1 + static_cast<int>(fs->getSlotMixMode(i)),
                                 juce::dontSendNotification);
    }
}

void SlotGridComponent::rebindMidiLearn()
{
    using namespace LuxSamplerConstants;
    using K = SamplerMidiTargets::Kind;
    auto& mm = processor.getMidiMap();

    for (int i = 0; i < MAX_UI_BANKS; ++i)
    {
        levelLearn[i] = std::make_unique<MidiLearnAttachment>(
            mm, levelSlider[i], SamplerMidiTargets::makeId(samplerIndex_, i, K::Img));
        modeLearn[i] = std::make_unique<MidiLearnAttachment>(
            mm, modeBox[i], SamplerMidiTargets::makeId(samplerIndex_, i, K::MixMode));
    }
}

void SlotGridComponent::resized()
{
    layoutMixerRow();
}

void SlotGridComponent::paint(juce::Graphics& g)
{
    auto* fs  = processor.getSampler(samplerIndex_);
    auto* seq = processor.getFrameSequencer(samplerIndex_);
    const int n = numBanks();

    // Determine which bank is currently sequencer-active
    int activeSeqBank = -1;
    if (seq != nullptr && seq->isPlaying())
    {
        const int step = seq->getCurrentStep();
        if (step >= 0)
            activeSeqBank = seq->getStep(step);
    }
    const int activePlaySlot = (fs != nullptr) ? fs->getActivePlaySlot() : -1;

    for (int i = 0; i < n; ++i)
    {
        const auto  cell        = cellBounds(i);
        const auto  st          = (fs != nullptr) ? fs->getSlotState(i) : SlotState::IDLE;
        const bool  hasContent  = (fs != nullptr) && fs->slotHasContent(i);
        const bool  isSelected  = (i == selectedSlot);
        const bool  isSeqActive = (i == activeSeqBank) || (i == activePlaySlot);

        // ── Background colour per state ───────────────────────────────────────
        juce::Colour bgCol, textCol;
        switch (st)
        {
            case SlotState::RECORDING:
                bgCol   = blinkOn ? juce::Colour(0xffcc2222) : juce::Colour(0xff7a1010);
                textCol = juce::Colours::white;
                break;
            case SlotState::PLAYING:
                bgCol   = juce::Colour(0xff1a6a1a);
                textCol = juce::Colour(0xff88ff88);
                break;
            default: // IDLE
                bgCol   = hasContent ? juce::Colour(0xff1e3028) : juce::Colour(0xff282828);
                textCol = hasContent ? juce::Colour(0xff66cc88) : juce::Colour(0xff484848);
                break;
        }

        // Brighten selected bank slightly (no hard border)
        g.setColour(isSelected ? bgCol.brighter(0.35f) : bgCol);
        g.fillRoundedRectangle(cell.toFloat(), 3.0f);

        // ── Bank number (top half) ────────────────────────────────────────────
        g.setColour(textCol.withAlpha(0.75f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("Bank " + juce::String(i + 1),
                   cell.withHeight(cell.getHeight() / 2),
                   juce::Justification::centredBottom, false);

        // ── State label (bottom half) ─────────────────────────────────────────
        juce::String stateStr;
        switch (st)
        {
            case SlotState::RECORDING: stateStr = "REC";  break;
            case SlotState::PLAYING:   stateStr = "PLAY"; break;
            default:
                if (hasContent && fs != nullptr)
                {
                    const float s = static_cast<float>(fs->getSlotDurationUs(i)) * 1e-6f;
                    stateStr = juce::String(s, 1) + "s";
                }
                else
                    stateStr = "--";
                break;
        }
        g.setColour(textCol);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText(stateStr,
                   cell.withTrimmedTop(cell.getHeight() / 2),
                   juce::Justification::centredTop, false);

        // ── White underline for sequencer-active / primary playing bank ───────
        if (isSeqActive)
        {
            g.setColour(juce::Colours::white);
            g.fillRect(cell.getX(), getHeight() - kUnderH + 1, cell.getWidth(), 3);
        }
    }
}

void SlotGridComponent::mouseDown(const juce::MouseEvent& e)
{
    const int n = numBanks();
    constexpr int kGap  = 3;
    const int totalGaps = (n - 1) * kGap;
    const int cellW     = (getWidth() - totalGaps) / juce::jmax(1, n);
    if (cellW <= 0) return;

    const int idx = e.x / (cellW + kGap);
    if (idx < 0 || idx >= n) return;

    // Only tile clicks select/menu — the mixer rows are child widgets and
    // never reach this handler.
    if (e.y >= cellBounds(idx).getBottom()) return;

    if (e.mods.isRightButtonDown())
    {
        // ── Right-click context menu: Copy / Paste bank ───────────────────
        auto* fs = processor.getSampler(samplerIndex_);
        const bool hasContent    = fs != nullptr && fs->slotHasContent(idx);
        const bool clipboardFull = clipboardSlot >= 0
                                   && fs != nullptr
                                   && fs->slotHasContent(clipboardSlot)
                                   && clipboardSlot != idx;

        juce::PopupMenu menu;
        menu.addSectionHeader("Bank " + juce::String(idx + 1));
        menu.addItem(1, "Copy bank",
                     hasContent,    // enabled only when slot has audio
                     clipboardSlot == idx); // ticked when this slot is already in clipboard
        menu.addItem(2, "Paste here",
                     clipboardFull, // enabled only when clipboard has a valid different slot
                     false);

        // showMenuAsync is correct here: the lambda is called on the message thread
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(this)
                               .withTargetScreenArea(cellBounds(idx).translated(
                                   getScreenX(), getScreenY()).toFloat().toNearestInt()),
            [this, idx](int result)
            {
                auto* fs2 = processor.getSampler(samplerIndex_);
                if (fs2 == nullptr) return;

                switch (result)
                {
                    case 1: // Copy
                        clipboardSlot = idx;
                        break;

                    case 2: // Paste
                        if (clipboardSlot >= 0 && clipboardSlot != idx)
                        {
                            fs2->copySlotTo(clipboardSlot, idx);
                            // Select and refresh the destination slot
                            selectedSlot = idx;
                            repaint();
                            if (onSlotSelected) onSlotSelected(idx);
                        }
                        break;

                    case 3: // Clear
                        fs2->uiClearSlot(idx);
                        repaint();
                        break;

                    default:
                        break;
                }
            });
        return;
    }

    // ── Left-click: select bank ───────────────────────────────────────────
    selectedSlot = idx;
    repaint();
    if (onSlotSelected)
        onSlotSelected(idx);
}

void SlotGridComponent::timerCallback()
{
    blinkOn = !blinkOn;
    // The SETUP "Banks" count is live — relayout when it changes.
    if (numBanks() != lastNumBanks_)
        layoutMixerRow();
    refreshMixerValues();
    repaint();
}
