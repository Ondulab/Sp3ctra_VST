#include "LuxWaveSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"

//==============================================================================
LuxWaveSettingsTab::LuxWaveSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts(processor.getAPVTS())
{
    // ── Enable toggle ─────────────────────────────────────────────────────
    enableLabel.setText("LuxWave:", juce::dontSendNotification);
    enableLabel.setJustificationType(juce::Justification::centredRight);
    enableLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(enableLabel);

    enableToggle.setButtonText("Enabled");
    addAndMakeVisible(enableToggle);
    enableAttachment = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxwaveEnabled", enableToggle);

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
        apvts, "luxwaveMidiChannel", midiChannelCombo);

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
        apvts, "luxwaveOctaveOffset", octaveOffsetCombo);
}

LuxWaveSettingsTab::~LuxWaveSettingsTab() = default;

//==============================================================================
void LuxWaveSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("LuxWave", getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

//==============================================================================
void LuxWaveSettingsTab::resized()
{
    const int w        = getWidth();
    const int titleH   = 30;
    constexpr int rowH   = Sp3ctraTheme::kRowStep;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    const int ctrlX    = 20 + labelW;
    const int ctrlW    = w - ctrlX - 20;

    int y = titleH + 5;

    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    auto row = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        const int vc = (rowH - ctrlH) / 2;
        lbl .setBounds(20,    y + vc, labelW, ctrlH);
        ctrl.setBounds(ctrlX, y + vc, ctrlW,  ctrlH);
        y += rowH;
    };

    row(enableLabel,       enableToggle);
    row(midiChannelLabel,  midiChannelCombo);
    row(octaveOffsetLabel, octaveOffsetCombo);
}
