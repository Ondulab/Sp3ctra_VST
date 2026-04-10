#include "SequencerComponent.h"
#include "../PluginProcessor.h"

SequencerComponent::SequencerComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        stepBtns[i].setButtonText("-");
        stepBtns[i].onClick = [this, i]
        {
            if (auto* seq = processor.getFrameSequencer())
            {
                const int cur  = seq->getStep(i);
                const int next = (cur < 0) ? 0
                    : (cur + 1 >= FrameSamplerConstants::NUM_SLOTS ? -1 : cur + 1);
                seq->setStep(i, next);
            }
        };
        addAndMakeVisible(stepBtns[i]);
    }
    startTimer(200);
}

SequencerComponent::~SequencerComponent() { stopTimer(); }

void SequencerComponent::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff1a2a1a));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(juce::Colour(0xff1e3e1e));
    g.fillRoundedRectangle(juce::Rectangle<float>(4.f, 4.f, (float)(getWidth()-8), 22.f), 3.f);
    g.setColour(juce::Colour(0xff66cc88));
    g.setFont(juce::Font(juce::FontOptions(12.f)).boldened());
    g.drawText("STEP SEQUENCER", juce::Rectangle<int>(8, 4, getWidth()-16, 22),
               juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xff447755));
    g.setFont(juce::FontOptions(10.f));
    g.drawText("click = cycle bank", juce::Rectangle<int>(8, 4, getWidth()-16, 22),
               juce::Justification::centredRight, false);
}

void SequencerComponent::resized()
{
    auto* seq = processor.getFrameSequencer();
    const int nSteps = (seq != nullptr) ? seq->getNumSteps() : 16;
    constexpr int cols  = 16;
    const int gap       = 2;
    const int startY    = 30;
    const int cellH     = 30;
    const int cellW     = (getWidth() - 8 - (cols - 1) * gap) / cols;

    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        if (i < nSteps)
        {
            const int col = i % cols;
            const int row = i / cols;
            stepBtns[i].setBounds(4 + col*(cellW+gap), startY + row*(cellH+gap), cellW, cellH);
            stepBtns[i].setVisible(true);
        }
        else
        {
            stepBtns[i].setVisible(false);
        }
    }
}

void SequencerComponent::timerCallback()
{
    auto* seq = processor.getFrameSequencer();
    if (seq == nullptr) return;

    const int nSteps    = seq->getNumSteps();
    const int curStep   = seq->getCurrentStep();
    const bool seqActive= seq->isEnabled();

    if (nSteps != cachedNumSteps)
    {
        cachedNumSteps = nSteps;
        resized();
    }

    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
    {
        if (i >= nSteps) continue;
        const int  bank      = seq->getStep(i);
        const bool isCurrent = (i == curStep) && seq->isPlaying();

        juce::Colour bg, txt;
        if (bank < 0)
        {
            bg  = isCurrent ? juce::Colour(0xff2a4a2a) : juce::Colour(0xff2a2a2a);
            txt = isCurrent ? juce::Colour(0xff88ffaa) : juce::Colour(0xff555555);
        }
        else
        {
            bg  = isCurrent ? juce::Colour(0xff1a6a1a) : juce::Colour(0xff1e3028);
            txt = isCurrent ? juce::Colours::white      : juce::Colour(0xff66cc88);
        }
        if (!seqActive) { bg = bg.withAlpha(0.4f); txt = txt.withAlpha(0.4f); }

        stepBtns[i].setButtonText(bank < 0 ? "-" : juce::String(bank + 1));
        stepBtns[i].setColour(juce::TextButton::buttonColourId,  bg);
        stepBtns[i].setColour(juce::TextButton::textColourOffId, txt);
    }
}
