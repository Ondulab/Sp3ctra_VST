#include "LuxMaskSettingsTab.h"

LuxMaskSettingsTab::LuxMaskSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts(processor.getAPVTS())
{
    // ── MIDI Channel (1-16) ────────────────────────────────────────────
    midiChannelLabel.setText("MIDI Channel", juce::dontSendNotification);
    midiChannelLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(midiChannelLabel);

    for (int ch = 1; ch <= 16; ++ch)
        midiChannelCombo.addItem("Channel " + juce::String(ch), ch);
    addAndMakeVisible(midiChannelCombo);
    midiChannelAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskMidiChannel", midiChannelCombo));

    // ── Octave Offset (-2..+2) ─────────────────────────────────────────
    octaveOffsetLabel.setText("Octave Offset", juce::dontSendNotification);
    octaveOffsetLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(octaveOffsetLabel);

    octaveOffsetCombo.addItem("-2", 1);
    octaveOffsetCombo.addItem("-1", 2);
    octaveOffsetCombo.addItem(" 0", 3);
    octaveOffsetCombo.addItem("+1", 4);
    octaveOffsetCombo.addItem("+2", 5);
    addAndMakeVisible(octaveOffsetCombo);
    octaveOffsetAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskOctaveOffset", octaveOffsetCombo));

    // ── Reference Note (C1..B6, default A3) ────────────────────────────
    refNoteLabel.setText("Reference Note", juce::dontSendNotification);
    refNoteLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(refNoteLabel);

    const char* noteLetters[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    int itemId = 1;
    for (int octave = 1; octave <= 6; ++octave)
        for (int note = 0; note < 12; ++note)
            refNoteCombo.addItem(juce::String(noteLetters[note]) + juce::String(octave), itemId++);
    addAndMakeVisible(refNoteCombo);
    refNoteAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskReferenceNote", refNoteCombo));

    // ── Polyphony (up to 10 voices) ──────────────────────────────────
    polyphonyLabel.setText("Polyphony", juce::dontSendNotification);
    polyphonyLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(polyphonyLabel);

    polyphonyToggle.setButtonText("Enable (10 voices max)");
    addAndMakeVisible(polyphonyToggle);
    polyphonyAttachment.reset(
        new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxmaskPolyphony", polyphonyToggle));
}

LuxMaskSettingsTab::~LuxMaskSettingsTab() {}

void LuxMaskSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(16.0f)).boldened());
    g.drawText("LuxMask MIDI Configuration",
               getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

void LuxMaskSettingsTab::resized()
{
    auto area = getLocalBounds().reduced(20);
    area.removeFromTop(35);

    const int rowH   = 30;
    const int gap     = 8;
    const int labelW  = 120;

    auto row = [&]() -> juce::Rectangle<int>
    {
        auto r = area.removeFromTop(rowH);
        area.removeFromTop(gap);
        return r;
    };

    // Row 1: MIDI Channel
    {
        auto r = row();
        midiChannelLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(gap);
        midiChannelCombo.setBounds(r);
    }
    // Row 2: Octave Offset
    {
        auto r = row();
        octaveOffsetLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(gap);
        octaveOffsetCombo.setBounds(r);
    }
    // Row 3: Reference Note
    {
        auto r = row();
        refNoteLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(gap);
        refNoteCombo.setBounds(r);
    }
    // Row 4: Polyphony
    {
        auto r = row();
        polyphonyLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(gap);
        polyphonyToggle.setBounds(r);
    }
}
