#include "TransportBarComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include "../ui/ModuleParamManifest.h"   // fsEngineParam — per-engine seq bank ids

// ─────────────────────────────────────────────────────────────────────────────
// IconTextButton::paintButton
// Draws a flat background (no border/outline) and a centred path icon.
// ─────────────────────────────────────────────────────────────────────────────
void IconTextButton::paintButton(juce::Graphics& g, bool isHighlighted, bool isDown)
{
    // Background — flat fill, no rounded-rect outline
    auto bg = findColour(juce::TextButton::buttonColourId);
    if (isHighlighted) bg = bg.brighter(0.12f);
    if (isDown)        bg = bg.darker(0.18f);
    g.setColour(bg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);

    // Icon — 60 % of shortest side, perfectly centred
    if (hasIcon)
    {
        const float size = (float)juce::jmin(getWidth(), getHeight()) * 0.60f;
        const float cx   = ((float)getWidth()  - size) * 0.5f;
        const float cy   = ((float)getHeight() - size) * 0.5f;
        Icons::fillPath(g, iconPath,
                         juce::Rectangle<float>(cx, cy, size, size),
                         findColour(juce::TextButton::textColourOffId));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportBarComponent
// ─────────────────────────────────────────────────────────────────────────────

TransportBarComponent::TransportBarComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    // ── Play / Hold / Stop ────────────────────────────────────────────────────
    // These buttons drive ONLY this engine's FrameSequencer.  They do NOT
    // write samplerFreezeMode — that parameter is owned exclusively by the
    // IMAGE page's S–Sampler transport (SourcesTabComponent).
    // The sequencer overrides freeze_mode internally via seqControlledPlay.
    // They go through the engine's SeqTransport param (0=Stop 1=Play 2=Hold)
    // so the DAW can automate / MIDI-map the transport; the processor relays
    // to the engine's sequencer.
    seqPlayBtn.setIconPath(Icons::play());
    seqPlayBtn.onClick = [this] { requestTransport(1); };
    addAndMakeVisible(seqPlayBtn);

    seqHoldBtn.setIconPath(Icons::pause());
    seqHoldBtn.onClick = [this] { requestTransport(2); };
    addAndMakeVisible(seqHoldBtn);

    seqStopBtn.setIconPath(Icons::stop());
    seqStopBtn.onClick = [this] { requestTransport(0); };
    addAndMakeVisible(seqStopBtn);

    updateTransportButtons();

    // ── BPM ──────────────────────────────────────────────────────────────────
    bpmSlider.setTextValueSuffix(" BPM");
    bpmSlider.setDoubleClickReturnValue(true, 120.0);   // cycle centre
    addAndMakeVisible(bpmSlider);
    bpmLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    bpmLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(bpmLabel);

    // ── Steps ─────────────────────────────────────────────────────────────────
    // Draggable value bar over the FULL SeqNumSteps range (2..16) — max 16
    // matches the 8×2 display grid in SequencerComponent. The attachment sets
    // range/interval from the int param and relays edits; the processor's
    // parameter listener forwards them to FrameSequencer::setNumSteps.
    addAndMakeVisible(stepsSlider);
    stepsLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    stepsLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(stepsLabel);

    // ── Loop / DAW sync ───────────────────────────────────────────────────────
    addAndMakeVisible(loopToggle);
    addAndMakeVisible(dawSyncToggle);

    rebindAttachments();   // engine 0 by default

    startTimer(200);
}

TransportBarComponent::~TransportBarComponent() { stopTimer(); }

void TransportBarComponent::setSamplerIndex(int i)
{
    if (samplerIndex_ == i) return;
    samplerIndex_ = i;
    rebindAttachments();
    updateTransportButtons();
}

// (Re)bind every attachment to samplerIndex_'s sequencer bank. Reset-first:
// destroy ALL old attachments before creating the new ones, so no stale
// attachment can push its param value into a control now bound elsewhere.
void TransportBarComponent::rebindAttachments()
{
    bpmAttach.reset();
    stepsAttach.reset();
    loopAttach.reset();
    dawSyncAttach.reset();
    learnAtts_.clear();

    auto& apvts   = processor.getAPVTS();
    const int e   = samplerIndex_;

    bpmAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, fsEngineParam(e, "SeqBpm"), bpmSlider);
    stepsAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, fsEngineParam(e, "SeqNumSteps"), stepsSlider);
    loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, fsEngineParam(e, "SeqLoop"), loopToggle);
    dawSyncAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, fsEngineParam(e, "SeqDawSync"), dawSyncToggle);

    // Right-click MIDI Learn — per-engine bank. The three transport buttons
    // share the engine's SeqTransport param (one CC spans 0=Stop / 1=Play /
    // 2=Hold), so each carries the same mapping badge.
    auto& mm = processor.getMidiMap();
    auto learn = [&](juce::Component& c, const juce::String& id)
    {
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, c, id));
    };
    learn(seqPlayBtn,    fsEngineParam(e, "SeqTransport"));
    learn(seqHoldBtn,    fsEngineParam(e, "SeqTransport"));
    learn(seqStopBtn,    fsEngineParam(e, "SeqTransport"));
    learn(bpmSlider,     fsEngineParam(e, "SeqBpm"));
    learn(stepsSlider,   fsEngineParam(e, "SeqNumSteps"));
    learn(loopToggle,    fsEngineParam(e, "SeqLoop"));
    learn(dawSyncToggle, fsEngineParam(e, "SeqDawSync"));
}

// ─────────────────────────────────────────────────────────────────────────────
// requestTransport — route a Play/Hold/Stop press through this engine's
// SeqTransport param so the host records/sees it. Re-pressing the current mode
// cannot change the param (no callback fires), so that case falls through to
// the engine directly — notably Play-while-playing restarts from step 0.
// ─────────────────────────────────────────────────────────────────────────────
void TransportBarComponent::requestTransport(int mode)
{
    if (auto* p = processor.getAPVTS().getParameter(
            fsEngineParam(samplerIndex_, "SeqTransport")))
    {
        const float norm = p->convertTo0to1(static_cast<float>(mode));
        if (! juce::approximatelyEqual(p->getValue(), norm))
        {
            p->setValueNotifyingHost(norm);  // parameterChanged drives the engine
        }
        else if (auto* seq = processor.getFrameSequencer(samplerIndex_))
        {
            if (mode == 1)      { if (seq->isHeld()) seq->uiResume(); else seq->uiPlay(); }
            else if (mode == 2) seq->uiHold();
            else                seq->uiStop();
        }
    }
    updateTransportButtons();
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff1a1a1a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout metrics — shared by resized() and the static size helpers so the
// host page can reserve the right height before the bar is laid out.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    constexpr int kPadX      = Sp3ctraTheme::kGap;         // 6
    constexpr int kGapX      = Sp3ctraTheme::kGap;         // 6
    constexpr int kCtrlH     = Sp3ctraTheme::kControlH;    // unified control height
    constexpr int kIconBtnW  = Sp3ctraTheme::kIconBtnSize; // square icon button
    constexpr int kBpmLabelW = 32;
    constexpr int kBpmBoxW   = Sp3ctraTheme::kTbWide;      // BPM text-box
    constexpr int kStepsLW   = 40;
    constexpr int kStepsComW = 60;
    constexpr int kLoopW     = 76;
    constexpr int kDawW      = 90;
    constexpr int kMinTrackW = 60;   // BPM slider track never shrinks below this

    // Fixed total for the single-row layout — everything but the BPM track.
    constexpr int kSingleRowFixedW = kPadX
        + (kIconBtnW + kGapX) * 3 + kGapX  // play/hold/stop + extra gap before BPM
        + kBpmLabelW + kGapX
        + kBpmBoxW + kGapX * 2
        + kStepsLW + kGapX
        + kStepsComW + kGapX * 2
        + kLoopW + kGapX
        + kDawW + kPadX;

    // Tail of row 1 in single-row mode (Steps section onward, incl. leading gaps).
    constexpr int kTailW = kGapX * 2 + kStepsLW + kGapX + kStepsComW + kGapX * 2
        + kLoopW + kGapX + kDawW;
}

int TransportBarComponent::singleRowMinWidth() noexcept
{
    return kSingleRowFixedW + kMinTrackW;
}

int TransportBarComponent::requiredHeight(int width) noexcept
{
    return width < singleRowMinWidth() ? kTwoRowH : kOneRowH;
}

// ─────────────────────────────────────────────────────────────────────────────
// resized — all interactive controls share kCtrlH for visual consistency.
//
// Wide:    [▶] [⏸] [■]  BPM [── slider ── 000 BPM]  Steps [bar] [Loop] [DAW Sync]
// Narrow:  [▶] [⏸] [■]  BPM [── slider ── 000 BPM]
//          Steps [bar] [Loop] [DAW Sync]
// ─────────────────────────────────────────────────────────────────────────────
void TransportBarComponent::resized()
{
    const int W = getWidth();
    const int h = getHeight();

    const bool twoRows = W < singleRowMinWidth();
    const int  rowH    = twoRows ? h / 2 : h;

    int cx   = kPadX;
    int rowY = 0;
    auto place = [&](juce::Component& c, int w)
    {
        c.setBounds(cx, rowY + (rowH - kCtrlH) / 2, w, kCtrlH);
        cx += w + kGapX;
    };

    // Row 1 — Play / Hold / Stop + BPM (the slider track absorbs the slack)
    place(seqPlayBtn, kIconBtnW);
    place(seqHoldBtn, kIconBtnW);
    place(seqStopBtn, kIconBtnW); cx += kGapX; // extra gap before BPM section

    bpmLabel.setBounds(cx, rowY + (rowH - 20) / 2, kBpmLabelW, 20);
    cx += kBpmLabelW + kGapX;

    const int tailW  = twoRows ? 0 : kTailW;
    const int trackW = juce::jmax(kMinTrackW, W - cx - kBpmBoxW - tailW - kPadX);
    bpmSlider.setBounds(cx, rowY + (rowH - kCtrlH) / 2, trackW + kBpmBoxW, kCtrlH);
    cx += trackW + kBpmBoxW + kGapX * 2;

    // Row 2 (narrow) or tail of row 1 (wide) — Steps / Loop / DAW Sync
    if (twoRows) { cx = kPadX; rowY = rowH; }

    stepsLabel.setBounds(cx, rowY + (rowH - 20) / 2, kStepsLW, 20);
    cx += kStepsLW + kGapX;

    place(stepsSlider, kStepsComW); cx += kGapX;
    place(loopToggle, kLoopW);
    place(dawSyncToggle, kDawW);
}

// ─────────────────────────────────────────────────────────────────────────────
// updateTransportButtons — mirrors the radio-button visual logic of
// ImagePageComponent::updateSamplerTransportButtons().
//
//  playing && !held  → Play  highlighted (green)
//  playing &&  held  → Hold  highlighted (purple)
//  !playing          → Stop  highlighted (dark red)
// ─────────────────────────────────────────────────────────────────────────────
void TransportBarComponent::updateTransportButtons()
{
    // Source of truth: FrameSequencer state (independent of samplerFreezeMode).
    //   playing && !held  → Play  highlighted (green)
    //   playing &&  held  → Hold  highlighted (purple)
    //   !playing          → Stop  highlighted (dark red)
    static const juce::Colour kPlay  { 0xff2a6040 };
    static const juce::Colour kHold  { 0xff6040a0 };
    static const juce::Colour kStop  { 0xff5a2020 };
    static const juce::Colour kOff   { 0xff2a2a2a };
    static const juce::Colour kFgOn  = juce::Colours::white;
    static const juce::Colour kFgOff { 0xff888888 };

    bool playing = false;
    bool held    = false;
    if (auto* seq = processor.getFrameSequencer(samplerIndex_))
    {
        playing = seq->isPlaying();
        held    = seq->isHeld();
    }

    // mode: 0=PLAY, 1=HOLD, 2=STOP (sequencer state, NOT samplerFreezeMode)
    const int mode = playing ? (held ? 1 : 0) : 2;

    auto style = [](IconTextButton& btn, bool active, juce::Colour col)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? col : kOff);
        btn.setColour(juce::TextButton::textColourOffId,
                      active ? kFgOn : kFgOff);
    };

    style(seqPlayBtn, mode == 0, kPlay);
    style(seqHoldBtn, mode == 1, kHold);
    style(seqStopBtn, mode == 2, kStop);
}

void TransportBarComponent::timerCallback()
{
    const bool dawSync = dawSyncToggle.getToggleState();
    bpmSlider.setEnabled(!dawSync);
    bpmLabel .setEnabled(!dawSync);
    updateTransportButtons();   // keep highlight in sync with sequencer state
}
