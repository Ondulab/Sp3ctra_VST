#include "MaskSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"

MaskSetupPanel::MaskSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : apvts(processor.getAPVTS()), accent(accentColour)
{
    // ── Step Mode (LuxStral / Free) ─────────────────────────────────────
    couplingLabel.setText("Step Mode", juce::dontSendNotification);
    couplingLabel.setJustificationType(juce::Justification::centredRight);
    couplingLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(couplingLabel);

    couplingCombo.addItem("LuxStral", 1);
    couplingCombo.addItem("Free",     2);
    addAndMakeVisible(couplingCombo);
    couplingAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "luxmaskCouplingMode", couplingCombo));

    // ── Free pixels per semitone ────────────────────────────────────────
    freeStepLabel.setText("px/semitone", juce::dontSendNotification);
    freeStepLabel.setJustificationType(juce::Justification::centredRight);
    freeStepLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(freeStepLabel);

    freeStepSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    freeStepSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                   50, Sp3ctraTheme::kControlH);
    addAndMakeVisible(freeStepSlider);
    freeStepAttachment.reset(
        new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskFreePixelsPerST", freeStepSlider));

    // ── Pitch Bend Range ────────────────────────────────────────────────
    pbRangeLabel.setText("PB Range", juce::dontSendNotification);
    pbRangeLabel.setJustificationType(juce::Justification::centredRight);
    pbRangeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(pbRangeLabel);

    pbRangeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    pbRangeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                  50, Sp3ctraTheme::kControlH);
    addAndMakeVisible(pbRangeSlider);
    pbRangeAttachment.reset(
        new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "luxmaskPitchBendRange", pbRangeSlider));

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
            apvts, "luxmaskMidiChannel", midiChannelCombo));

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
            apvts, "luxmaskOctaveOffset", octaveOffsetCombo));

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
            apvts, "luxmaskReferenceNote", refNoteCombo));

    // ── Polyphony (up to 10 voices) ──────────────────────────────────
    polyphonyLabel.setText("Polyphony", juce::dontSendNotification);
    polyphonyLabel.setJustificationType(juce::Justification::centredRight);
    polyphonyLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(polyphonyLabel);

    polyphonyToggle.setButtonText("Enable (10 voices max)");
    addAndMakeVisible(polyphonyToggle);
    polyphonyAttachment.reset(
        new juce::AudioProcessorValueTreeState::ButtonAttachment(
            apvts, "luxmaskPolyphony", polyphonyToggle));
}

MaskSetupPanel::~MaskSetupPanel() {}

void MaskSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "MASK -- SETUP", accent);
}

void MaskSetupPanel::resized()
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

    // Row 1: Step Mode
    {
        auto r = row();
        couplingLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        couplingCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 2: px/semitone
    {
        auto r = row();
        freeStepLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        freeStepSlider.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 3: PB Range
    {
        auto r = row();
        pbRangeLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        pbRangeSlider.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 4: MIDI Channel
    {
        auto r = row();
        midiChannelLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        midiChannelCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 5: Octave Offset
    {
        auto r = row();
        octaveOffsetLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        octaveOffsetCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 6: Reference Note
    {
        auto r = row();
        refNoteLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        refNoteCombo.setBounds(r.removeFromLeft(ctrlW));
    }
    // Row 7: Polyphony
    {
        auto r = row();
        polyphonyLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        polyphonyToggle.setBounds(r.removeFromLeft(ctrlW));
    }
}
