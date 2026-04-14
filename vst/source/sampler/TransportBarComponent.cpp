#include "TransportBarComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"

// ─────────────────────────────────────────────────────────────────────────────
// Icon paths — normalised [0,1] square coordinates
// ─────────────────────────────────────────────────────────────────────────────
namespace Icons
{
    // Filled right-pointing triangle (play)
    static juce::Path play()
    {
        juce::Path p;
        p.addTriangle(0.12f, 0.04f,  0.12f, 0.96f,  0.92f, 0.50f);
        return p;
    }

    // Two vertical bars (pause / hold)
    static juce::Path pause()
    {
        juce::Path p;
        p.addRectangle(0.15f, 0.10f, 0.28f, 0.80f); // left bar
        p.addRectangle(0.57f, 0.10f, 0.28f, 0.80f); // right bar
        return p;
    }

    // Filled square (stop)
    static juce::Path stop()
    {
        juce::Path p;
        p.addRectangle(0.10f, 0.10f, 0.80f, 0.80f);
        return p;
    }

    // Scale-to-fit helper
    static void fillPath(juce::Graphics& g, const juce::Path& src,
                          juce::Rectangle<float> area, juce::Colour col)
    {
        juce::Path scaled = src;
        scaled.applyTransform(src.getTransformToScaleToFit(area, true));
        g.setColour(col);
        g.fillPath(scaled);
    }
} // namespace Icons

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
    auto& apvts = processor.getAPVTS();

    // ── Play / Hold / Stop ────────────────────────────────────────────────────
    // Each button writes samplerFreezeMode (shared with Image tab) AND drives
    // the FrameSequencer, so both tabs always reflect the same state.
    seqPlayBtn.setIconPath(Icons::play());
    seqPlayBtn.onClick = [this]
    {
        processor.getAPVTS().getParameterAsValue("samplerFreezeMode").setValue(0);
        if (auto* seq = processor.getFrameSequencer())
        {
            // If the sequencer is paused (held), resume from the current step.
            // Otherwise (stopped or fresh start), restart from step 0.
            if (seq->isHeld())
                seq->uiResume();
            else
                seq->uiPlay();
        }
        updateTransportButtons();
    };
    addAndMakeVisible(seqPlayBtn);

    seqHoldBtn.setIconPath(Icons::pause());
    seqHoldBtn.onClick = [this]
    {
        processor.getAPVTS().getParameterAsValue("samplerFreezeMode").setValue(1);
        if (auto* seq = processor.getFrameSequencer()) seq->uiHold();
        updateTransportButtons();
    };
    addAndMakeVisible(seqHoldBtn);

    seqStopBtn.setIconPath(Icons::stop());
    seqStopBtn.onClick = [this]
    {
        processor.getAPVTS().getParameterAsValue("samplerFreezeMode").setValue(2);
        if (auto* seq = processor.getFrameSequencer()) seq->uiStop();
        updateTransportButtons();
    };
    addAndMakeVisible(seqStopBtn);

    updateTransportButtons();

    // ── BPM ──────────────────────────────────────────────────────────────────
    // textBoxHeight = 28 matches all other interactive controls in this bar.
    bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                              Sp3ctraTheme::kTbWide, Sp3ctraTheme::kTextBoxH);
    bpmSlider.setTextValueSuffix(" BPM");
    addAndMakeVisible(bpmSlider);
    bpmAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "seqBpm", bpmSlider);
    bpmLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    bpmLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(bpmLabel);

    // ── Steps ─────────────────────────────────────────────────────────────────
    stepsCombo.addItemList({"4","8","12","16","24","32"}, 1);
    {
        static const int choices[] = { 4, 8, 12, 16, 24, 32 };
        const int cur = static_cast<int>(
            apvts.getRawParameterValue("seqNumSteps")->load());
        for (int k = 0; k < 6; ++k)
            if (choices[k] == cur) { stepsCombo.setSelectedId(k+1, juce::dontSendNotification); break; }
    }
    stepsCombo.onChange = [this]
    {
        static const int choices[] = { 4, 8, 12, 16, 24, 32 };
        const int id = stepsCombo.getSelectedId();
        if (id >= 1 && id <= 6)
        {
            const int n = choices[id-1];
            if (auto* p = processor.getAPVTS().getParameter("seqNumSteps"))
                p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(n)));
            if (auto* seq = processor.getFrameSequencer())
                seq->setNumSteps(n);
        }
    };
    stepsLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    stepsLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(stepsCombo);
    addAndMakeVisible(stepsLabel);

    // ── Loop / DAW sync ───────────────────────────────────────────────────────
    addAndMakeVisible(loopToggle);
    loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqLoop", loopToggle);

    addAndMakeVisible(dawSyncToggle);
    dawSyncAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqDawSync", dawSyncToggle);

    startTimer(200);
}

TransportBarComponent::~TransportBarComponent() { stopTimer(); }

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff1a1a1a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// resized — all interactive controls share ctrlH for visual consistency.
//
//  [▶] [⏸] [■]   BPM [── slider ─── 000.0 BPM]   Steps [combo]  [Loop]  [DAW Sync]
// ─────────────────────────────────────────────────────────────────────────────
void TransportBarComponent::resized()
{
    const int W      = getWidth();
    const int h      = getHeight();
    constexpr int pad    = Sp3ctraTheme::kGap;      // 6
    constexpr int gap    = Sp3ctraTheme::kGap;      // 6
    constexpr int ctrlH  = Sp3ctraTheme::kControlH; // unified control height

    // Fixed widths
    constexpr int iconBtnW  = Sp3ctraTheme::kIconBtnSize; // square icon button
    constexpr int bpmLabelW = 32;
    constexpr int bpmBoxW   = Sp3ctraTheme::kTbWide;      // BPM text-box
    const int stepsLW   = 40;
    const int stepsComW = 60;
    const int loopW     = 76;
    const int dawW      = 90;

    // Fixed total for 3 transport buttons + all other elements (BPM slider excluded)
    const int fixedW = pad
        + iconBtnW + gap   // play
        + iconBtnW + gap   // hold
        + iconBtnW + gap * 2  // stop + extra gap before BPM
        + bpmLabelW + gap
        + bpmBoxW
        + gap * 2
        + stepsLW + gap
        + stepsComW + gap * 2
        + loopW + gap
        + dawW + pad;

    const int sliderTrackW = juce::jmax(60, W - fixedW);

    int cx = pad;
    auto place = [&](juce::Component& c, int w)
    {
        c.setBounds(cx, (h - ctrlH) / 2, w, ctrlH);
        cx += w + gap;
    };

    // Transport: Play / Hold / Stop
    place(seqPlayBtn, iconBtnW);
    place(seqHoldBtn, iconBtnW);
    place(seqStopBtn, iconBtnW); cx += gap; // extra gap before BPM section

    // BPM label (vertically centred, narrower height)
    bpmLabel.setBounds(cx, (h - 20) / 2, bpmLabelW, 20);
    cx += bpmLabelW + gap;

    // BPM slider: track + textbox together at ctrlH
    bpmSlider.setBounds(cx, (h - ctrlH) / 2, sliderTrackW + bpmBoxW, ctrlH);
    cx += sliderTrackW + bpmBoxW + gap * 2;

    // Steps label + combo
    stepsLabel.setBounds(cx, (h - 20) / 2, stepsLW, 20);
    cx += stepsLW + gap;

    place(stepsCombo, stepsComW); cx += gap;
    place(loopToggle, loopW);
    place(dawSyncToggle, dawW);
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
    // Single source of truth: samplerFreezeMode (shared with Image tab).
    //   0 = PLAY  →  green
    //   1 = HOLD  →  purple
    //   2 = STOP  →  dark red
    static const juce::Colour kPlay  { 0xff2a6040 };
    static const juce::Colour kHold  { 0xff6040a0 };
    static const juce::Colour kStop  { 0xff5a2020 };
    static const juce::Colour kOff   { 0xff2a2a2a };
    static const juce::Colour kFgOn  = juce::Colours::white;
    static const juce::Colour kFgOff { 0xff888888 };

    const int mode = static_cast<int>(
        processor.getAPVTS().getRawParameterValue("samplerFreezeMode")->load());

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
