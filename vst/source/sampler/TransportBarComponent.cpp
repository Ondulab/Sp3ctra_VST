#include "TransportBarComponent.h"
#include "../PluginProcessor.h"

TransportBarComponent::TransportBarComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    auto& apvts = processor.getAPVTS();

    // ── Sampler enable ────────────────────────────────────────────────────────
    addAndMakeVisible(fsEnabledToggle);
    fsEnabledAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "frameSamplerEnabled", fsEnabledToggle);

    // ── Sequencer enable ──────────────────────────────────────────────────────
    addAndMakeVisible(seqEnabledToggle);
    seqEnabledAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "seqEnabled", seqEnabledToggle);

    // ── Play / Stop ───────────────────────────────────────────────────────────
    seqPlayBtn.onClick = [this]
    {
        if (auto* seq = processor.getFrameSequencer()) seq->uiPlay();
    };
    seqStopBtn.onClick = [this]
    {
        if (auto* seq = processor.getFrameSequencer()) seq->uiStop();
    };
    addAndMakeVisible(seqPlayBtn);
    addAndMakeVisible(seqStopBtn);

    // ── BPM ──────────────────────────────────────────────────────────────────
    bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
    bpmSlider.setTextValueSuffix(" BPM");
    addAndMakeVisible(bpmSlider);
    bpmAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "seqBpm", bpmSlider);
    bpmLabel.setFont(juce::FontOptions(10.f));
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
    stepsLabel.setFont(juce::FontOptions(10.f));
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

void TransportBarComponent::resized()
{
    const int h   = getHeight();
    const int pad = 4;
    const int gap = 4;
    int cx = pad;
    auto place = [&](juce::Component& c, int w, int ch) {
        c.setBounds(cx, (h - ch) / 2, w, ch);
        cx += w + gap;
    };

    place(fsEnabledToggle,  72, 22);  cx += 4;
    place(seqPlayBtn,       26, 26);
    place(seqStopBtn,       32, 26);  cx += 4;
    place(seqEnabledToggle, 52, 22);  cx += 4;
    place(bpmLabel,         28, 20);
    place(bpmSlider,       130, 26);  cx += 4;
    place(stepsLabel,       36, 20);
    place(stepsCombo,       52, 26);  cx += 4;
    place(loopToggle,       48, 22);
    place(dawSyncToggle,    46, 22);
}

void TransportBarComponent::timerCallback()
{
    // Disable BPM slider when DAW sync is active
    const bool dawSync = dawSyncToggle.getToggleState();
    bpmSlider.setEnabled(!dawSync);
    bpmLabel .setEnabled(!dawSync);
}
