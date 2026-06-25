#include "LuxSynthSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"

//==============================================================================
LuxSynthSetupPanel::LuxSynthSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : apvts(processor.getAPVTS()), accent(accentColour)
{
    // ── Enable toggle ── moved to the rack LED + zone-3 header power switch

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
        apvts, "luxsynthMidiChannel", midiChannelCombo);

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
        apvts, "luxsynthOctaveOffset", octaveOffsetCombo);
}

LuxSynthSetupPanel::~LuxSynthSetupPanel() = default;

//==============================================================================
void LuxSynthSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "LUXSYNTH -- SETUP", accent);
}

//==============================================================================
void LuxSynthSetupPanel::resized()
{
    const int w        = getWidth();
    constexpr int rowH   = Sp3ctraTheme::kRowStep;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    const int ctrlX    = Sp3ctraTheme::kHPad + labelW + Sp3ctraTheme::kGap;
    const int ctrlW    = juce::jmin(260, w - ctrlX - Sp3ctraTheme::kHPad);

    int y = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;

    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    auto row = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        const int vc = (rowH - ctrlH) / 2;
        lbl .setBounds(Sp3ctraTheme::kHPad, y + vc, labelW, ctrlH);
        ctrl.setBounds(ctrlX,               y + vc, ctrlW,  ctrlH);
        y += rowH;
    };

    row(midiChannelLabel,  midiChannelCombo);
    row(octaveOffsetLabel, octaveOffsetCombo);
}
