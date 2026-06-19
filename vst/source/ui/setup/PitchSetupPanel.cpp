#include "PitchSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"

PitchSetupPanel::PitchSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : apvts(processor.getAPVTS()), accent(accentColour)
{
    // ── MIDI Channel (1-16) ────────────────────────────────────────────
    midiChannelLabel.setText("MIDI Channel", juce::dontSendNotification);
    midiChannelLabel.setJustificationType(juce::Justification::centredRight);
    midiChannelLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(midiChannelLabel);

    for (int ch = 1; ch <= 16; ++ch)
        midiChannelCombo.addItem("Channel " + juce::String(ch), ch);
    addAndMakeVisible(midiChannelCombo);
    midiChannelAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchMidiChannel", midiChannelCombo));

    // ── Octave Offset (-2..+2) ─────────────────────────────────────────
    octaveOffsetLabel.setText("Octave Offset", juce::dontSendNotification);
    octaveOffsetLabel.setJustificationType(juce::Justification::centredRight);
    octaveOffsetLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(octaveOffsetLabel);

    octaveOffsetCombo.addItem("-2", 1);
    octaveOffsetCombo.addItem("-1", 2);
    octaveOffsetCombo.addItem(" 0", 3);
    octaveOffsetCombo.addItem("+1", 4);
    octaveOffsetCombo.addItem("+2", 5);
    addAndMakeVisible(octaveOffsetCombo);
    octaveOffsetAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchOctaveOffset", octaveOffsetCombo));

    // ── Reference Note (C1..B6, default A3) ────────────────────────────
    refNoteLabel.setText("Reference Note", juce::dontSendNotification);
    refNoteLabel.setJustificationType(juce::Justification::centredRight);
    refNoteLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(refNoteLabel);

    const char* noteLetters[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    int itemId = 1;
    for (int octave = 1; octave <= 6; ++octave)
        for (int note = 0; note < 12; ++note)
            refNoteCombo.addItem(juce::String(noteLetters[note]) + juce::String(octave), itemId++);
    addAndMakeVisible(refNoteCombo);
    refNoteAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxpitchReferenceNote", refNoteCombo));

    // ── Polyphony (up to 10 voices) ──────────────────────────────────
    polyphonyLabel.setText("Polyphony", juce::dontSendNotification);
    polyphonyLabel.setJustificationType(juce::Justification::centredRight);
    polyphonyLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(polyphonyLabel);

    polyphonyToggle.setButtonText("Enable (10 voices max)");
    addAndMakeVisible(polyphonyToggle);
    polyphonyAttachment.reset(
        new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxpitchPolyphony", polyphonyToggle));
}

PitchSetupPanel::~PitchSetupPanel() {}

void PitchSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "PITCH -- SETUP", accent);
}

void PitchSetupPanel::resized()
{
    auto area = getLocalBounds().reduced(Sp3ctraTheme::kHPad, 0);
    area.removeFromTop(SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap);

    constexpr int rowH   = Sp3ctraTheme::kControlH;
    constexpr int gap    = Sp3ctraTheme::kRowGap * 2;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    const int ctrlW      = juce::jmin(260, area.getWidth() - labelW - Sp3ctraTheme::kGap);

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
        r.removeFromLeft(Sp3ctraTheme::kGap);
        midiChannelCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 2: Octave Offset
    {
        auto r = row();
        octaveOffsetLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        octaveOffsetCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 3: Reference Note
    {
        auto r = row();
        refNoteLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        refNoteCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 4: Polyphony
    {
        auto r = row();
        polyphonyLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        polyphonyToggle.setBounds(r.removeFromLeft(ctrlW));
    }
}
