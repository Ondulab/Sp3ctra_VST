#include "GeneralSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"

//==============================================================================
GeneralSettingsTab::GeneralSettingsTab(Sp3ctraAudioProcessor& processor)
    : audioProcessor(processor),
      apvts(processor.getAPVTS())
{
    // ── Visualizer Mode ───────────────────────────────────────────────────────
    visualizerModeLabel.setText("Visualizer Mode:", juce::dontSendNotification);
    visualizerModeLabel.setJustificationType(juce::Justification::centredRight);
    visualizerModeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(visualizerModeLabel);

    visualizerModeCombo.addItem("Image",            1);
    visualizerModeCombo.addItem("Waveform",         2);
    visualizerModeCombo.addItem("Inverted Waveform",3);
    addAndMakeVisible(visualizerModeCombo);

    visualizerModeAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, "visualizerMode", visualizerModeCombo);

    // ── Log Level ─────────────────────────────────────────────────────────────
    logLevelLabel.setText("Log Level:", juce::dontSendNotification);
    logLevelLabel.setJustificationType(juce::Justification::centredRight);
    logLevelLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(logLevelLabel);

    logLevelCombo.addItem("Error",   1);
    logLevelCombo.addItem("Warning", 2);
    logLevelCombo.addItem("Info",    3);
    logLevelCombo.addItem("Debug",   4);
    addAndMakeVisible(logLevelCombo);

    logLevelAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, "logLevel", logLevelCombo);

}

GeneralSettingsTab::~GeneralSettingsTab() = default;

//==============================================================================
void GeneralSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Section title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("General Configuration",
               getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

//==============================================================================
void GeneralSettingsTab::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(40); // skip title
    bounds.reduce(20, 10);

    constexpr int labelW  = Sp3ctraTheme::kLabelW;      // 110
    constexpr int rowH    = Sp3ctraTheme::kRowStep;      // 32
    constexpr int ctrlH   = Sp3ctraTheme::kControlH;     // 28
    constexpr int padding = Sp3ctraTheme::kGap;          // 6

    auto placeRow = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        auto row = bounds.removeFromTop(rowH);
        lbl .setBounds(row.removeFromLeft(labelW).withSizeKeepingCentre(labelW, ctrlH));
        row .removeFromLeft(padding);
        ctrl.setBounds(row.withSizeKeepingCentre(row.getWidth(), ctrlH));
    };

    placeRow(visualizerModeLabel, visualizerModeCombo);
    bounds.removeFromTop(Sp3ctraTheme::kSectionGap);
    placeRow(logLevelLabel,       logLevelCombo);
}
